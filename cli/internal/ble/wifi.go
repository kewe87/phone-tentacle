package ble

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"

	"github.com/onehumanbeing/esp32-iphone-agent/cli/internal/discover"
	"github.com/onehumanbeing/esp32-iphone-agent/cli/internal/proto"
)

// WiFi is the default transport: boards are found on the local network and reached on TCP 7202. No Bluetooth.
// Finding, in this order: the address given by hand (--host, ESP32AGENT_HOST); mDNS; the addresses boards had
// last time (hosts.json in the config dir); and, when all that finds nothing, a try of port 7202 on the local
// subnets (ESP32AGENT_NO_PROBE=1 turns that off).
type WiFi struct {
	Dir string // config dir, for hosts.json; "": nothing is remembered

	mu   sync.Mutex
	host string // set by hand; wins over everything
}

// SetHost sets the board's address by hand ("192.168.1.50" or "192.168.1.50:7202"); "" goes back to finding it.
func (w *WiFi) SetHost(h string) {
	w.mu.Lock()
	w.host = h
	w.mu.Unlock()
}

func (w *WiFi) hostSet() string {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.host != "" {
		return w.host
	}
	return os.Getenv("ESP32AGENT_HOST")
}

// hostPort adds the default port to a bare host.
func hostPort(h string) string {
	if _, _, err := net.SplitHostPort(h); err == nil {
		return h
	}
	return net.JoinHostPort(h, strconv.Itoa(WiFiPort))
}

type knownHost struct {
	Addr string `json:"addr"`
	Name string `json:"name,omitempty"`
	Seen string `json:"seen,omitempty"`
}

func (w *WiFi) hostsPath() string { return filepath.Join(w.Dir, "hosts.json") }

func (w *WiFi) known() map[string]knownHost {
	out := map[string]knownHost{}
	if w.Dir == "" {
		return out
	}
	if raw, err := os.ReadFile(w.hostsPath()); err == nil {
		json.Unmarshal(raw, &out)
	}
	return out
}

// Remember keeps where a board was reached, for the next time mDNS finds nothing.
func (w *WiFi) Remember(id string, f Found) {
	if w.Dir == "" || id == "" || f.Addr == "" {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	k := w.known()
	if old := k[id]; old.Addr == f.Addr && old.Name == f.Name {
		return
	}
	k[id] = knownHost{Addr: f.Addr, Name: f.Name, Seen: time.Now().UTC().Format(time.RFC3339)}
	if raw, err := json.MarshalIndent(k, "", " "); err == nil {
		tmp := w.hostsPath() + ".tmp"
		if os.WriteFile(tmp, raw, 0o600) == nil {
			os.Rename(tmp, w.hostsPath())
		}
	}
}

// Forget drops a remembered address (unpair).
func (w *WiFi) Forget(id string) {
	if w.Dir == "" {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	k := w.known()
	if _, ok := k[id]; !ok {
		return
	}
	delete(k, id)
	if raw, err := json.MarshalIndent(k, "", " "); err == nil {
		os.WriteFile(w.hostsPath(), raw, 0o600)
	}
}

func (w *WiFi) Scan(ctx context.Context, d time.Duration) ([]Found, error) {
	logf := func(format string, args ...any) { proto.Logf(ctx, format, args...) }
	if h := w.hostSet(); h != "" {
		logf("discover: using the address given by hand, %s", h)
		return []Found{{Name: h, Addr: hostPort(h), Via: "wifi", Source: "host"}}, nil
	}
	var out []Found
	seen := map[string]bool{}
	for _, b := range discover.Find(ctx, d, logf) {
		out = append(out, Found{Name: b.Name, Addr: b.Addr(), ID: b.ID, FW: b.FW, Paired: b.Paired, Via: "wifi", Source: "mdns"})
		seen[b.ID] = true
	}
	if len(out) > 0 {
		return out, nil
	}
	for id, k := range w.known() { // where the boards were last time
		logf("discover: mDNS found nothing; %s was last at %s", id, k.Addr)
		out = append(out, Found{Name: k.Name, Addr: k.Addr, ID: id, Paired: -1, Via: "wifi", Source: "remembered"})
	}
	if len(out) > 0 || os.Getenv("ESP32AGENT_NO_PROBE") == "1" {
		return out, nil
	}
	for _, addr := range discover.Probe(ctx, logf) {
		out = append(out, Found{Name: addr, Addr: addr, Paired: -1, Via: "wifi", Source: "probe"})
	}
	return out, nil
}

func (w *WiFi) Connect(ctx context.Context, f Found) (proto.Link, error) {
	if f.Addr == "" {
		return nil, errors.New("no address for the board")
	}
	started := time.Now()
	l, err := DialWiFi(ctx, f.Addr)
	proto.Logf(ctx, "connect: tcp %s (%s) in %v, err %v", f.Addr, f.Source, time.Since(started).Round(time.Millisecond), err)
	if err != nil {
		return nil, &NotReachable{Addr: f.Addr, Err: err}
	}
	return l, nil
}

// NotReachable: the board's address did not accept a connection.
type NotReachable struct {
	Addr string
	Err  error
}

func (e *NotReachable) Error() string {
	return "the board at " + e.Addr + " does not answer on the network (" + e.Err.Error() + ")"
}
func (e *NotReachable) Unwrap() error { return e.Err }
