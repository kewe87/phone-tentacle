package flash

import (
	"bytes"
	"context"
	"encoding/json"
	"strings"
	"time"
)

// SetupInfo is the firmware's answer to `0 setup` on USB serial (hardware/hid_bridge/netcfg.h):
//
//	wifi-setup {"setup":1,"ssid":"Tentacle-1A2B","pass":"...","url":"http://192.168.4.1/","qr":"fe0b...,..."}
//	wifi-setup {"setup":0,"ssid":"<the stored Wi-Fi>","ip":"<address>"}
type SetupInfo struct {
	Setup bool     `json:"setup"` // true: the setup Wi-Fi is open; false: a Wi-Fi is stored
	SSID  string   `json:"ssid"`
	Pass  string   `json:"pass,omitempty"`
	URL   string   `json:"url,omitempty"`
	IP    string   `json:"ip,omitempty"`
	QR    []string `json:"qr,omitempty"` // one hex string per row, first module in the top bit
}

// ParseSetupLine reads a wifi-setup line; ok is false for any other line.
func ParseSetupLine(line string) (*SetupInfo, bool) {
	line = strings.TrimSpace(line)
	rest, ok := strings.CutPrefix(line, "wifi-setup ")
	if !ok {
		return nil, false
	}
	var raw struct {
		Setup int    `json:"setup"`
		SSID  string `json:"ssid"`
		Pass  string `json:"pass"`
		URL   string `json:"url"`
		IP    string `json:"ip"`
		QR    string `json:"qr"`
	}
	if json.Unmarshal([]byte(rest), &raw) != nil {
		return nil, false
	}
	s := &SetupInfo{Setup: raw.Setup == 1, SSID: raw.SSID, Pass: raw.Pass, URL: raw.URL, IP: raw.IP}
	if raw.QR != "" {
		s.QR = strings.Split(raw.QR, ",")
	}
	return s, true
}

// QRModules turns the qr rows into a square of modules (true = dark); nil if they are not a QR code.
func QRModules(rows []string) [][]bool {
	n := len(rows)
	if n < 21 {
		return nil
	}
	m := make([][]bool, n)
	for y, r := range rows {
		m[y] = make([]bool, n)
		for x := range n {
			i := x / 4
			if i >= len(r) {
				return nil
			}
			h := hexVal(r[i])
			if h < 0 {
				return nil
			}
			m[y][x] = h>>(3-x%4)&1 == 1
		}
	}
	return m
}

func hexVal(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

// AskSetup asks the firmware for its setup Wi-Fi, on port or on every candidate port (the firmware's own first),
// until one answers or wait passes.
func (f *Flasher) AskSetup(ctx context.Context, port string, wait time.Duration) (*SetupInfo, error) {
	deadline := time.Now().Add(wait)
	for {
		var names []string
		if port != "" {
			names = []string{port}
		} else if ps, err := f.Sys.Ports(); err == nil {
			for _, k := range []Kind{KindFirmware, KindUnknown, KindROM} {
				for _, p := range ps {
					if p.Kind == k {
						names = append(names, p.Name)
					}
				}
			}
		}
		for _, n := range names {
			if s := f.askPort(n); s != nil {
				f.logf("setup answer on %s: setup=%v ssid=%q", n, s.Setup, s.SSID)
				return s, nil
			}
		}
		if time.Now().After(deadline) {
			return nil, msg("no_setup", nil)
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(time.Second):
		}
	}
}

// askPort sends `0 setup` and reads lines for up to 3 s. DTR and RTS go up in that order (the Arduino CDC sends
// only while both are set; this order never resets a USB-Serial/JTAG) and down in reverse.
func (f *Flasher) askPort(name string) *SetupInfo {
	c, err := f.Sys.Open(name, 115200)
	if err != nil {
		return nil
	}
	defer func() {
		_ = c.SetRTS(false)
		_ = c.SetDTR(false)
		_ = c.Close()
	}()
	_ = c.SetDTR(true)
	_ = c.SetRTS(true)
	time.Sleep(100 * time.Millisecond)
	if _, err := c.Write([]byte("0 setup\n")); err != nil {
		return nil
	}
	var buf []byte
	tmp := make([]byte, 1024)
	end := time.Now().Add(3 * time.Second)
	for time.Now().Before(end) {
		_ = c.SetReadTimeout(100 * time.Millisecond)
		n, err := c.Read(tmp)
		if err != nil {
			return nil
		}
		buf = append(buf, tmp[:n]...)
		for {
			i := bytes.IndexByte(buf, '\n')
			if i < 0 {
				break
			}
			if s, ok := ParseSetupLine(string(buf[:i])); ok {
				return s
			}
			buf = buf[i+1:]
		}
	}
	return nil
}
