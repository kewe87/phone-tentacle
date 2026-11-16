package flashui

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
	"github.com/kewe87/phone-tentacle/cli/internal/flash"
	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
)

// image is a small merged image: bootloader, a partition table with otadata and app0, the app.
func image() []byte {
	img := bytes.Repeat([]byte{0xFF}, 0x20000)
	copy(img, []byte{0xE9, 3, 2, 0x3F})
	img[12], img[13] = 9, 0
	ent := func(i int, typ, sub byte, off, size uint32) {
		e := img[0x8000+32*i:]
		copy(e, []byte{0xAA, 0x50, typ, sub})
		binary.LittleEndian.PutUint32(e[4:], off)
		binary.LittleEndian.PutUint32(e[8:], size)
	}
	ent(0, 1, 0, 0xe000, 0x2000)
	ent(1, 0, 0x10, 0x10000, 0x330000)
	ent(2, 1, 2, 0x9000, 0x5000)
	copy(img[0x10000:], []byte{0xE9, 6, 2, 0x3F})
	img[0x10000+12], img[0x10000+13] = 9, 0
	copy(img[0x10100:], "tentacle-fw:0.4.0;")
	return img
}

const setup = `wifi-setup {"setup":1,"ssid":"Tentacle-1A2B","pass":"abcdefghijkl","url":"http://192.168.4.1/"}`

func newServer() (*Server, *esprom.FakeROM) {
	rom := esprom.NewFakeROM()
	rom.Download = false
	s := &Server{Sys: flash.NewFakeSystem(rom, setup), Version: "1.0.0",
		Default: flash.Bytes{Name: "t.merged.bin", Data: image()}, token: "tok", addr: "127.0.0.1:4711"}
	s.loadDefault()
	return s, rom
}

func call(t *testing.T, h http.Handler, method, path, host, token string, body []byte, hdr map[string]string) (int, map[string]any) {
	t.Helper()
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.Host = host
	if token != "" {
		r.Header.Set("X-Token", token)
	}
	for k, v := range hdr {
		r.Header.Set(k, v)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var m map[string]any
	_ = json.Unmarshal(w.Body.Bytes(), &m)
	return w.Code, m
}

func TestGuard(t *testing.T) {
	s, _ := newServer()
	h := s.Handler()
	if c, _ := call(t, h, "GET", "/api/state", "evil.example:4711", "tok", nil, nil); c != http.StatusForbidden {
		t.Errorf("foreign host: %d", c)
	}
	if c, _ := call(t, h, "POST", "/api/install", "127.0.0.1:4711", "", nil, nil); c != http.StatusForbidden {
		t.Errorf("no token: %d", c)
	}
	if c, _ := call(t, h, "GET", "/api/state", "127.0.0.1:4711", "wrong", nil, nil); c != http.StatusForbidden {
		t.Errorf("wrong token: %d", c)
	}
	if c, _ := call(t, h, "GET", "/", "127.0.0.1:4711", "", nil, nil); c != http.StatusOK {
		t.Errorf("page: %d", c)
	}
	if c, _ := call(t, h, "GET", "/app.js", "localhost:4711", "", nil, nil); c != http.StatusOK {
		t.Errorf("localhost: %d", c)
	}
}

func TestStateAndInstall(t *testing.T) {
	s, rom := newServer()
	h := s.Handler()
	c, st := call(t, h, "GET", "/api/state", s.addr, "tok", nil, map[string]string{"Accept-Language": "de-DE,de;q=0.9,en;q=0.8"})
	if c != 200 || st["lang"] != "de" {
		t.Fatalf("%d %v", c, st)
	}
	im, ok := st["image"].(map[string]any)
	if !ok {
		t.Fatalf("no image: %v", st["imageError"])
	}
	if im["version"] != "0.4.0" || im["signed"] != false || im["kind"] != "merged" {
		t.Errorf("image %v", im)
	}
	ports := st["ports"].([]any)
	if len(ports) != 1 || ports[0].(map[string]any)["kind"] != "firmware" {
		t.Errorf("ports %v", ports)
	}
	if c, _ := call(t, h, "POST", "/api/install", s.addr, "tok", []byte(`{"port":""}`), nil); c != 200 {
		t.Fatalf("install: %d", c)
	}
	if c, m := call(t, h, "POST", "/api/install", s.addr, "tok", []byte(`{}`), nil); c != http.StatusConflict ||
		!strings.Contains(m["error"].(map[string]any)["de"].(string), "läuft schon") {
		t.Errorf("second install: %d %v", c, m)
	}
	deadline := time.Now().Add(30 * time.Second)
	var job map[string]any
	for time.Now().Before(deadline) {
		_, st = call(t, h, "GET", "/api/state", s.addr, "tok", nil, nil)
		job = st["job"].(map[string]any)
		if job["running"] == false {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if job["error"] != nil {
		t.Fatalf("install failed: %v\n%v", job["error"], job["log"])
	}
	res := job["result"].(map[string]any)
	if res["setup"].(map[string]any)["ssid"] != "Tentacle-1A2B" || job["chip"].(map[string]any)["name"] != "ESP32-S3" {
		t.Errorf("result %v %v", res, job["chip"])
	}
	if rom.Download || !bytes.Equal(rom.Flash[0x10000:0x10120], image()[0x10000:0x10120]) {
		t.Error("not written or not restarted")
	}
	if len(job["log"].([]any)) == 0 {
		t.Error("empty log")
	}
}

func TestUpload(t *testing.T) {
	s, _ := newServer()
	h := s.Handler()
	img := image()
	copy(img[0x10100:], "tentacle-fw:0.5.0;")
	if c, _ := call(t, h, "POST", "/api/image?name=own.merged.bin", s.addr, "tok", img, nil); c != 200 {
		t.Fatal(c)
	}
	_, st := call(t, h, "GET", "/api/state", s.addr, "tok", nil, nil)
	if im := st["image"].(map[string]any); im["version"] != "0.5.0" || im["source"] != "file" {
		t.Errorf("uploaded image %v", im)
	}
	// a signature inside it that does not belong to it
	call(t, h, "POST", "/api/image?name=own.merged.bin", s.addr, "tok", fwsign.Embed(img, make([]byte, 64)), nil)
	_, st = call(t, h, "GET", "/api/state", s.addr, "tok", nil, nil)
	if st["image"] != nil || st["imageError"].(map[string]any)["code"] != "bad_sig" {
		t.Errorf("bad signature accepted: %v", st)
	}
	if c, m := call(t, h, "POST", "/api/install", s.addr, "tok", []byte(`{}`), nil); c != http.StatusConflict ||
		m["error"].(map[string]any)["code"] != "bad_sig" {
		t.Errorf("install with a bad image: %d %v", c, m)
	}
	call(t, h, "POST", "/api/image/reset", s.addr, "tok", nil, nil)
	_, st = call(t, h, "GET", "/api/state", s.addr, "tok", nil, nil)
	if im := st["image"].(map[string]any); im["version"] != "0.4.0" {
		t.Errorf("reset: %v", im)
	}
}

func TestLang(t *testing.T) {
	for in, want := range map[string]string{"": "en", "de": "de", "en-US,de;q=0.5": "en", "fr,de-AT;q=0.8": "de"} {
		r := httptest.NewRequest("GET", "/", nil)
		r.Header.Set("Accept-Language", in)
		if got := lang(r); got != want {
			t.Errorf("%q: %s", in, got)
		}
	}
}
