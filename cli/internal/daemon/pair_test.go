package daemon

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/link"
	"github.com/kewe87/phone-tentacle/cli/internal/proto"
)

// progressLines collects what the daemon sends ahead of a response.
type progressLines struct {
	mu    sync.Mutex
	lines []map[string]any
}

func (p *progressLines) add(line []byte) {
	var r struct {
		Data map[string]any `json:"data"`
	}
	json.Unmarshal(line, &r)
	p.mu.Lock()
	p.lines = append(p.lines, r.Data)
	p.mu.Unlock()
}

func (p *progressLines) find(key string) map[string]any {
	p.mu.Lock()
	defer p.mu.Unlock()
	for _, l := range p.lines {
		if _, ok := l[key]; ok {
			return l
		}
	}
	return nil
}

// Pairing: the daemon sends this computer's name, and hands the code to the client while the board waits.
func TestPairSendsTheNameAndShowsTheCode(t *testing.T) {
	b := fakeBoard()
	var boardName, boardCode string
	b.Boot = func(name, code string) bool { boardName, boardCode = name, code; return true }
	c, s := start(t, fakeTransport{b})
	var p progressLines
	c.Progress = p.add
	r, data := call(t, c, Request{Cmd: "pair"})
	if r.Status != "ok" || data["id"] != "246f28aa6ad5" {
		t.Fatalf("pair: %+v", r)
	}
	host, _ := os.Hostname()
	if len(host) > proto.PairNameMax {
		host = host[:proto.PairNameMax]
	}
	if boardName == "" || !strings.HasPrefix(host, boardName) && boardName != "computer" {
		t.Fatalf("the board got the name %q, this computer is %q", boardName, host)
	}
	shown := p.find("code")
	if shown == nil || shown["code"] != boardCode || len(boardCode) != 6 || shown["pair"] != "confirm" || shown["wait_s"] != 30.0 {
		t.Fatalf("progress %+v, the board shows %q", p.lines, boardCode)
	}
	if s.Config.Key(mustHex("246f28aa6ad5")) == nil {
		t.Fatal("key not stored")
	}
}

// A board that was in use a moment ago (our own session, closed for the pairing) is asked again.
func TestPairAsksAgainWhileTheBoardWasInUse(t *testing.T) {
	old := pairBusyRetry
	pairBusyRetry = 20 * time.Millisecond
	defer func() { pairBusyRetry = old }()
	b := fakeBoard()
	b.PairErr = "busy: board in use (a paired computer's session; end it, wait 10 s)"
	c, _ := start(t, fakeTransport{b})
	var p progressLines
	var once sync.Once
	c.Progress = func(line []byte) {
		p.add(line)
		once.Do(func() { go func() { time.Sleep(30 * time.Millisecond); b.SetPairErr("") }() })
	}
	if r, _ := call(t, c, Request{Cmd: "pair"}); r.Status != "ok" {
		t.Fatalf("pair: %+v", r)
	}
	if p.find("message") == nil {
		t.Fatalf("no word about waiting: %+v", p.lines)
	}
	// ... but a lock (someone's request was not confirmed) is not waited out: it is reported.
	b.SetPairErr("busy: pairing locked for 40 s (a request was not confirmed)")
	if r, _ := call(t, c, Request{Cmd: "pair"}); r.Status != "error" || !strings.Contains(r.Error, "pairing locked for 40 s") {
		t.Fatalf("pair on a locked board: %+v", r)
	}
}

// A board whose pairing window is closed refuses at once, and pair reports it at once with what to do.
func TestPairFailsAtOnceWhenPairingIsClosed(t *testing.T) {
	b := fakeBoard()
	b.PairErr = "pairing closed: double-press BOOT on the board, then pair within 60 s"
	c, s := start(t, fakeTransport{b})
	t0 := time.Now()
	r, _ := call(t, c, Request{Cmd: "pair"})
	if r.Status != "error" || !strings.Contains(r.Error, "Double-press its BOOT button") {
		t.Fatalf("pair on a closed board: %+v", r)
	}
	if d := time.Since(t0); d > 2*time.Second {
		t.Fatalf("pair took %v on a closed board", d)
	}
	if s.Config.Key(mustHex("246f28aa6ad5")) != nil {
		t.Fatal("a key was stored")
	}
}

func TestUnpairHereAndOnTheBoard(t *testing.T) {
	b := fakeBoard()
	c, s := start(t, fakeTransport{b})
	call(t, c, Request{Cmd: "pair"})
	r, data := call(t, c, Request{Cmd: "unpair"})
	if r.Status != "ok" || !strings.Contains(data["message"].(string), "unpair --board") || strings.Contains(data["message"].(string), "app") {
		t.Fatalf("unpair: %+v", r)
	}
	if got := strings.Join(b.Commands, ","); strings.Contains(got, "unpair") {
		t.Fatalf("a local unpair told the board: %s", got)
	}
	call(t, c, Request{Cmd: "pair"})
	r, data = call(t, c, Request{Cmd: "unpair", Text: "board"})
	if r.Status != "ok" || s.Config.Key(mustHex("246f28aa6ad5")) != nil {
		t.Fatalf("unpair --board: %+v", r)
	}
	if got := b.Commands[len(b.Commands)-1]; got != "unpair" {
		t.Fatalf("the board got %q", got)
	}
}

// noBoards finds nothing.
type noBoards struct{ scans *int }

func (n noBoards) Scan(context.Context, time.Duration) ([]link.Found, error) {
	*n.scans++
	return nil, nil
}
func (n noBoards) Connect(context.Context, link.Found) (proto.Link, error) {
	return nil, io.ErrUnexpectedEOF
}

// A board that is not there is not searched for again and again: the next request waits a little.
func TestNotFoundBacksOff(t *testing.T) {
	scans := 0
	c, _ := start(t, noBoards{&scans})
	r, _ := call(t, c, Request{Cmd: "info"})
	if r.Status != "error" || !strings.Contains(r.Error, "not found on the network") || !strings.Contains(r.Error, "--host") {
		t.Fatalf("info without a board: %+v", r)
	}
	r, _ = call(t, c, Request{Cmd: "info"})
	if r.Status != "error" || !strings.Contains(r.Error, "next try in") || scans != 1 {
		t.Fatalf("second info: %+v after %d scans", r, scans)
	}
	time.Sleep(reachBackoffMin + 50*time.Millisecond)
	if r, _ = call(t, c, Request{Cmd: "info"}); strings.Contains(r.Error, "next try in") || scans != 2 {
		t.Fatalf("after the wait: %+v after %d scans", r, scans)
	}
	if r, _ = call(t, c, Request{Cmd: "devices"}); r.Status != "ok" || scans != 3 { // looking never waits
		t.Fatalf("devices: %+v after %d scans", r, scans)
	}
}

// serveTCP puts a fake board behind a real TCP listener, in the board's records ([u16 LE length][frame]).
// drop closes the current connection, as a Wi-Fi dropout does.
func serveTCP(t *testing.T, b *proto.FakeBoard) (addr string, drop func()) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	var mu sync.Mutex
	var cur net.Conn
	go func() {
		for {
			nc, err := ln.Accept()
			if err != nil {
				return
			}
			mu.Lock()
			cur = nc
			mu.Unlock()
			l := b.Connect()
			go func() { // board → computer
				for f := range l.Recv() {
					nc.Write(append(binary.LittleEndian.AppendUint16(nil, uint16(len(f))), f...))
				}
				nc.Close()
			}()
			go func() { // computer → board
				r := bufio.NewReader(nc)
				for {
					var hdr [2]byte
					if _, err := io.ReadFull(r, hdr[:]); err != nil {
						break
					}
					f := make([]byte, binary.LittleEndian.Uint16(hdr[:]))
					if _, err := io.ReadFull(r, f); err != nil {
						break
					}
					if len(f) > 0 { // a zero-length record is a keepalive
						l.Write(f)
					}
				}
				l.Close()
			}()
		}
	}()
	return ln.Addr().String(), func() {
		mu.Lock()
		defer mu.Unlock()
		if cur != nil {
			cur.Close()
		}
	}
}

// The whole way: the Wi-Fi transport with the board's address given by hand, pairing with the
// code, a session, a dropped link and the reconnect, all over a real TCP connection.
func TestWiFiOnlyOverTCP(t *testing.T) {
	t.Setenv("TENTACLE_NO_PROBE", "1")
	b := fakeBoard()
	var boardCode string
	b.Boot = func(_, code string) bool { boardCode = code; return true }
	addr, drop := serveTCP(t, b)
	c, s := start(t, nil)
	s.Transport = &link.WiFi{Dir: s.Dir}
	c.Host = addr
	var p progressLines
	c.Progress = p.add

	r, data := call(t, c, Request{Cmd: "pair"})
	if r.Status != "ok" || data["id"] != "246f28aa6ad5" {
		t.Fatalf("pair over TCP: %+v", r)
	}
	if shown := p.find("code"); shown == nil || shown["code"] != boardCode {
		t.Fatalf("code: progress %+v, board %q", p.lines, boardCode)
	}
	b.Status(`{"fw":"0.2.5","net":"sta","ip":"127.0.0.1","ap":"idle","link":{"kbps":0}}`)
	time.Sleep(50 * time.Millisecond)
	r, data = call(t, c, Request{Cmd: "info"})
	if r.Status != "ok" || data["fw"] != "0.2.5" || data["name"] != "Tentacle 6AD5" {
		t.Fatalf("info: %+v", r)
	}
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "esc"}); r.Status != "ok" {
		t.Fatalf("keys: %+v", r)
	}

	// The address is remembered for the board: when nothing finds it (no --host, no mDNS answer), that is tried.
	raw, _ := os.ReadFile(s.Dir + "/hosts.json")
	if !strings.Contains(string(raw), addr) || !strings.Contains(string(raw), "246f28aa6ad5") {
		t.Fatalf("hosts.json: %s", raw)
	}

	drop() // Wi-Fi dropout: the next request finds the board again and opens a new session
	time.Sleep(100 * time.Millisecond)
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "enter"}); r.Status != "ok" {
		t.Fatalf("keys after the link dropped: %+v", r)
	}
	if got := strings.Join(b.Commands, ","); got != "stream on,key esc,stream on,key enter" {
		t.Fatalf("board got %s", got)
	}

	// devices lists it with where it answers.
	r, data = call(t, c, Request{Cmd: "devices"})
	list, _ := data["devices"].([]any)
	if r.Status != "ok" || len(list) == 0 {
		t.Fatalf("devices: %+v", r)
	}
}

// The remembered address is the fallback when mDNS finds nothing.
func TestRememberedAddress(t *testing.T) {
	t.Setenv("TENTACLE_NO_PROBE", "1")
	t.Setenv("TENTACLE_HOST", "")
	dir := t.TempDir()
	w := &link.WiFi{Dir: dir}
	w.Remember("246f28aa6ad5", link.Found{Name: "Tentacle 6AD5", Addr: "192.0.2.7:7202"})
	found, err := w.Scan(context.Background(), 30*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	var hit *link.Found
	for i := range found {
		if found[i].ID == "246f28aa6ad5" {
			hit = &found[i]
		}
	}
	if hit == nil {
		t.Skipf("a real board answered mDNS here: %+v", found)
	}
	if hit.Addr != "192.0.2.7:7202" || hit.Source != "remembered" || hit.Suffix() != "6ad5" {
		t.Fatalf("remembered: %+v", *hit)
	}
	w.SetHost("10.1.2.3") // by hand wins, with the default port
	if found, _ = w.Scan(context.Background(), time.Millisecond); len(found) != 1 || found[0].Addr != "10.1.2.3:7202" || found[0].Source != "host" {
		t.Fatalf("host: %+v", found)
	}
	w.SetHost("")
	w.Forget("246f28aa6ad5")
	if raw, _ := os.ReadFile(dir + "/hosts.json"); strings.Contains(string(raw), "246f28aa6ad5") {
		t.Fatalf("still remembered: %s", raw)
	}
}
