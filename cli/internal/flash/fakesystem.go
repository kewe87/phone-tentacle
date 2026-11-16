package flash

import (
	"bytes"
	"fmt"
	"io"
	"sync"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
)

// FakeSystem is a computer with one board on USB, for tests without hardware. The board runs its firmware
// (TinyUSB port FWPort, product "Tentacle", answers `0 setup` with Setup) or sits in the ROM bootloader
// (USB-Serial/JTAG ROMPort), as the FakeROM says. A 1200 baud open of the firmware port restarts it into the
// bootloader, like the Arduino core does.
type FakeSystem struct {
	mu      sync.Mutex
	ROM     *esprom.FakeROM
	FWPort  string
	ROMPort string
	Product bool       // the system reports USB product strings (Windows, Linux; not macOS)
	Blank   bool       // no firmware: every open reaches the ROM
	Gap     int        // Ports calls that see no board (set after each restart)
	Extra   []PortInfo // other ports on the computer
	Setup   string     // the firmware's answer to `0 setup`
	Touches int        // 1200 baud touches seen
}

// NewFakeSystem puts rom on COM5 (bootloader) / COM6 (firmware).
func NewFakeSystem(rom *esprom.FakeROM, setup string) *FakeSystem {
	s := &FakeSystem{ROM: rom, FWPort: "COM6", ROMPort: "COM5", Product: true, Setup: setup}
	rom.OnBoot = func(bool) { s.Gap = 2 }
	return s
}

func (s *FakeSystem) current() (string, bool) {
	if s.ROM.Download {
		return s.ROMPort, true
	}
	return s.FWPort, false
}

// Ports implements System.
func (s *FakeSystem) Ports() ([]PortInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ps := append([]PortInfo(nil), s.Extra...)
	if s.Gap > 0 {
		s.Gap--
	} else {
		name, rom := s.current()
		p := PortInfo{Name: name, VID: "303A", PID: "1001"}
		if s.Product {
			p.Product = "Tentacle"
			if rom {
				p.Product = "USB JTAG/serial debug unit"
			}
		}
		ps = append(ps, p)
	}
	for i := range ps {
		classify(&ps[i])
	}
	return ps, nil
}

// Open implements System.
func (s *FakeSystem) Open(name string, baud int) (Conn, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	cur, rom := s.current()
	if name != cur || s.Gap > 0 {
		return nil, fmt.Errorf("open %s: no such port", name)
	}
	if rom || s.Blank {
		s.ROM.Reopen()
		return s.ROM, nil
	}
	if baud == 1200 {
		s.Touches++
		s.ROM.RestartToBootloader()
		return nopConn{}, nil
	}
	return &fwConn{s: s}, nil
}

type nopConn struct{}

func (nopConn) Read([]byte) (int, error)           { return 0, io.EOF }
func (nopConn) Write(p []byte) (int, error)        { return len(p), nil }
func (nopConn) SetReadTimeout(time.Duration) error { return nil }
func (nopConn) SetDTR(bool) error                  { return nil }
func (nopConn) SetRTS(bool) error                  { return nil }
func (nopConn) ResetInputBuffer() error            { return nil }
func (nopConn) Close() error                       { return nil }

// fwConn is the firmware's serial console: it answers `0 setup`.
type fwConn struct {
	nopConn
	s   *FakeSystem
	in  []byte
	out []byte
}

func (c *fwConn) Write(p []byte) (int, error) {
	c.in = append(c.in, p...)
	if bytes.Contains(c.in, []byte("0 setup\n")) {
		c.in = nil
		c.s.mu.Lock()
		c.out = append(c.out, "[boot] tentacle\r\n"+c.s.Setup+"\r\n"...)
		c.s.mu.Unlock()
	}
	return len(p), nil
}

func (c *fwConn) Read(p []byte) (int, error) {
	if len(c.out) == 0 {
		time.Sleep(5 * time.Millisecond)
		return 0, nil
	}
	n := copy(p, c.out)
	c.out = c.out[n:]
	return n, nil
}
