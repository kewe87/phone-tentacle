// Package link finds boards and opens links to them: over Wi-Fi (wifi.go: mDNS and TCP 7202), or to the simulated
// board (hardware/tests/sim) when TENTACLE_SIM is set.
package link

import (
	"context"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/proto"
)

// Found is one board that was found.
type Found struct {
	Name   string
	Addr   string // host:port
	RSSI   int16
	ID     string // board id (12 hex) when the board told it (mDNS, a remembered address); "" otherwise
	FW     string // firmware version (mDNS)
	Paired int    // computers paired with the board (mDNS); -1 if unknown
	Open   string // the board's pairing window (mDNS): "open", "closed"; "" if unknown
	Source string // how it was found: "mdns", "remembered", "host", "probe"; "" for the simulator
}

// Suffix is the board id's last 4 hex digits: from the id, else from the name ("Tentacle 6AD5" → "6ad5").
func (f Found) Suffix() string {
	if len(f.ID) == 12 {
		return strings.ToLower(f.ID[8:])
	}
	i := strings.LastIndexByte(f.Name, ' ')
	if i < 0 || len(f.Name)-i-1 != 4 {
		return ""
	}
	return strings.ToLower(f.Name[i+1:])
}

// Transport finds boards and connects to them.
type Transport interface {
	Scan(ctx context.Context, d time.Duration) ([]Found, error)
	Connect(ctx context.Context, f Found) (proto.Link, error)
}

// Default is the simulated board when TENTACLE_SIM names its binary, otherwise Wi-Fi. dir is the config dir
// (remembered addresses).
func Default(dir string) Transport {
	if sim := os.Getenv("TENTACLE_SIM"); sim != "" {
		return &Sim{Bin: sim, Screen: os.Getenv("TENTACLE_SIM_SCREEN")}
	}
	return &WiFi{Dir: dir}
}

// chanLink is a Link fed by a reader: the board's frames go into a buffered channel.
type chanLink struct {
	ch    chan []byte
	once  sync.Once
	mu    sync.Mutex
	shut  bool
	write func([]byte) error
	close func()
}

func newChanLink(write func([]byte) error, close func()) *chanLink {
	return &chanLink{ch: make(chan []byte, 8192), write: write, close: close}
}

// deliver hands a frame to the reader; a full buffer drops it (video recovers at the next IDR, a control
// frame lost this way ends the session through its counter).
func (l *chanLink) deliver(f []byte) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.shut {
		return
	}
	select {
	case l.ch <- f:
	default:
	}
}

// deliverWait hands a frame to the reader and waits while the buffer is full (a TCP link: the stack then slows the
// board down, nothing is lost). False once the link is gone or done closes.
func (l *chanLink) deliverWait(f []byte, done <-chan struct{}) bool {
	for {
		l.mu.Lock()
		if l.shut {
			l.mu.Unlock()
			return false
		}
		select {
		case l.ch <- f:
			l.mu.Unlock()
			return true
		default:
		}
		l.mu.Unlock()
		select {
		case <-done:
			return false
		case <-time.After(5 * time.Millisecond):
		}
	}
}

func (l *chanLink) Recv() <-chan []byte { return l.ch }

func (l *chanLink) Write(f []byte) error {
	l.mu.Lock()
	shut := l.shut
	l.mu.Unlock()
	if shut {
		return proto.ErrSession
	}
	return l.write(f)
}

// drop marks the link gone (the board or the OS dropped it).
func (l *chanLink) drop() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if !l.shut {
		l.shut = true
		close(l.ch)
	}
}

func (l *chanLink) Close() error {
	l.once.Do(func() {
		l.drop()
		if l.close != nil {
			l.close()
		}
	})
	return nil
}
