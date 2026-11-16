package daemon

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
)

// fakeImage is a small "firmware": the ESP32-S3 header bits CheckImage looks at and a version marker.
func fakeImage(ver string, size int) []byte {
	img := make([]byte, size)
	img[0], img[12] = 0xE9, 9
	for i := 24; i < size; i++ {
		img[i] = byte(i * 7)
	}
	copy(img[100:], "tentacle-fw:"+ver+";")
	return img
}

// otaBoard plays the board's side of an update (hardware/hid_bridge/ota.cpp) on a FakeBoard.
type otaBoard struct {
	mu      sync.Mutex
	pub     ed25519.PublicKey
	fw      string
	size    int
	sha     string
	sig     string
	ver     string
	got     bytes.Buffer
	failed  string
	begins  int
	refused []string
}

func (o *otaBoard) image() *bytes.Buffer {
	o.mu.Lock()
	defer o.mu.Unlock()
	return bytes.NewBuffer(append([]byte{}, o.got.Bytes()...))
}

func TestUpdateSendsSignedImageAndWaitsForTheBoard(t *testing.T) {
	updateBackWait = 20 * time.Second
	pub, priv, _ := ed25519.GenerateKey(nil)
	ob := &otaBoard{pub: pub, fw: "0.1.0"}
	b := fakeBoard()
	b.Upload = func(off uint32, data []byte) {
		ob.mu.Lock()
		defer ob.mu.Unlock()
		if ob.failed == "" && int(off) != ob.got.Len() {
			ob.failed = "out of order"
		}
		ob.got.Write(data)
	}
	b.Handle = func(cmd string) string {
		ob.mu.Lock()
		defer ob.mu.Unlock()
		switch {
		case cmd == "stream on":
			fw := ob.fw
			go func() { time.Sleep(20 * time.Millisecond); b.Status(fmt.Sprintf(`{"fw":%q}`, fw)) }()
		case strings.HasPrefix(cmd, "update begin "):
			f := strings.Fields(cmd)
			ob.begins++
			ob.sha, ob.sig = f[3], f[4]
			fmt.Sscan(f[2], &ob.size)
			if len(f) > 5 {
				ob.ver = f[5]
			}
			sum, _ := hex.DecodeString(ob.sha)
			sig, _ := hex.DecodeString(ob.sig)
			if ob.sig == "-" || !ed25519.Verify(ob.pub, append([]byte("tentacle-fw-v1"), sum...), sig) {
				ob.refused = append(ob.refused, cmd)
				return "err unsigned firmware: this board takes only signed Tentacle releases"
			}
			ob.got.Reset()
			return "ok ready"
		case cmd == "update state":
			if ob.failed != "" {
				return "err " + ob.failed
			}
			return fmt.Sprintf("ok ready %d %d", ob.got.Len(), ob.size)
		case cmd == "update end":
			sum := sha256.Sum256(ob.got.Bytes())
			if ob.got.Len() != ob.size || hex.EncodeToString(sum[:]) != ob.sha {
				return "err SHA-256 does not match"
			}
			ob.fw = ob.ver
			go func() { time.Sleep(50 * time.Millisecond); b.Drop() }()
			return "ok restarting into " + ob.ver
		}
		return "ok"
	}
	c, _ := start(t, fakeTransport{b})
	call(t, c, Request{Cmd: "pair"})
	time.Sleep(100 * time.Millisecond)

	dir := t.TempDir()
	path := filepath.Join(dir, "fw.bin")
	img := fakeImage("0.2.0", 300<<10+123)
	os.WriteFile(path, img, 0o644)

	// unsigned: a release board refuses it before a single byte
	if r, _ := call(t, c, Request{Cmd: "update", Text: path}); r.Status != "error" || !strings.Contains(r.Error, "unsigned") {
		t.Fatalf("unsigned: %+v", r)
	}
	if ob.image().Len() != 0 {
		t.Fatal("bytes were sent for a refused image")
	}

	os.WriteFile(path+fwsign.SigExt, fwsign.Sign(priv, img), 0o644)
	r, data := call(t, c, Request{Cmd: "update", Text: path})
	if r.Status != "ok" || data["from"] != "0.1.0" || data["fw"] != "0.2.0" {
		t.Fatalf("update: %+v", r)
	}
	if !bytes.Equal(ob.image().Bytes(), img) {
		t.Fatal("the board got another image")
	}

	// no marker: refused unless --force
	os.WriteFile(path, fakeImage("x", 5000)[:99], 0o644)
	if r, _ := call(t, c, Request{Cmd: "update", Text: path}); r.Status != "error" {
		t.Fatalf("not an image: %+v", r)
	}
}

func TestDebugPagesAndFollow(t *testing.T) {
	b := fakeBoard()
	b.Handle = func(cmd string) string {
		switch cmd {
		case "dbg state":
			return "ok 5 9 hello\n"
		case "dbgout 5":
			return "ok 9 9 abc\\\\"
		case "logs follow on":
			go func() { time.Sleep(50 * time.Millisecond); b.Log("C 1 tap\nD x\n") }()
		}
		return "ok"
	}
	c, _ := start(t, fakeTransport{b})
	call(t, c, Request{Cmd: "pair"})
	if r, data := call(t, c, Request{Cmd: "debug", Text: "state"}); r.Status != "ok" || data["reply"] != "hello\nabc\\" {
		t.Fatalf("debug: %+v", r)
	}
	var out syncBuf
	done := make(chan struct{})
	go func() {
		c.Stream(Request{Cmd: "logfollow"}, &out)
		close(done)
	}()
	for i := 0; i < 100 && !strings.Contains(out.String(), "D x"); i++ {
		time.Sleep(20 * time.Millisecond)
	}
	if out.String() != "C 1 tap\nD x\n" {
		t.Fatalf("follow: %q", out.String())
	}
}

type syncBuf struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuf) Write(p []byte) (int, error) { s.mu.Lock(); defer s.mu.Unlock(); return s.b.Write(p) }
func (s *syncBuf) String() string              { s.mu.Lock(); defer s.mu.Unlock(); return s.b.String() }
