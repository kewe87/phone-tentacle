package daemon

import (
	"context"
	"image"
	"strings"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

func grey(w, h int) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i := range img.Pix {
		img.Pix[i] = 200
	}
	return img
}

// mirroringBoard: an abs board whose screen came in once (718x1560), with a decoder at hand.
func mirroringBoard(t *testing.T, status string) (*Client, *Conn) {
	t.Helper()
	b, c, s := absBoard(t)
	if status != "" {
		b.Status(status)
		time.Sleep(50 * time.Millisecond)
	}
	conn, err := s.conn(context.Background(), "")
	if err != nil {
		t.Fatal(err)
	}
	conn.mu.Lock()
	conn.ffmpeg = "ffmpeg" // frames come from onFrame here, never from a decoder
	conn.mu.Unlock()
	conn.onFrame(video.Frame{Img: grey(718, 1560)})
	return c, conn
}

// Without a frame there is no telling which screenshot the pixels are of: refused, nothing sent. The status's
// AirPlay size (480x1040) sent a tap meant for x 700 of 718 to the right edge (2026-11-16).
func TestTapWithoutAFrameIsRefused(t *testing.T) {
	b, c, _ := absBoard(t)
	for _, req := range []Request{{Cmd: "tap", Pts: []float64{700, 100}}, {Cmd: "swipe", Pts: []float64{1, 2, 3, 4}},
		{Cmd: "longpress", Pts: []float64{1, 2}}} {
		if r, _ := call(t, c, req); r.Status != "error" || !strings.Contains(r.Error, "take a screenshot first") {
			t.Fatalf("%s without a frame: %+v", req.Cmd, r)
		}
	}
	if len(b.Commands) != 0 {
		t.Fatalf("sent %s", sent(b))
	}
}

// A frame stays the coordinate space after mirroring stopped and the session reconnected.
func TestFrameSizeOutlivesAReconnect(t *testing.T) {
	b, c, s := absBoard(t)
	conn, _ := s.conn(context.Background(), "")
	conn.onFrame(video.Frame{Img: grey(718, 1560)})
	b.Status(`{"fw":"0.2.2","abs":1,"up":2000,"ap":"idle","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	if r, _ := call(t, c, Request{Cmd: "tap", Pts: []float64{359, 780}}); r.Status != "ok" { // reconnects
		t.Fatalf("tap: %+v", r)
	}
	if got := sent(b); !strings.HasSuffix(got, "abs 16384 16384,click") {
		t.Fatalf("tap sent %s", got)
	}
}

// The picture of a still screen: the AirPlay session is up ("connected") but sends nothing. The last picture is
// the screen, with its age; not "mirroring is off".
func TestScreenshotOfAQuietSession(t *testing.T) {
	c, _ := mirroringBoard(t, `{"fw":"0.2.2","abs":1,"up":2000,"ap":"connected","fz":0}`)
	r, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg"})
	if r.Status != "ok" {
		t.Fatalf("screenshot: %+v", r)
	}
	if _, ok := data["age_ms"].(float64); !ok || data["captured_at"] == "" || data["stale"] != false {
		t.Fatalf("no age, or stale: %v", data["age_ms"])
	}
}

// After input the screenshot waits for a newer picture; none in time: the latest, marked stale.
func TestScreenshotAfterInputWaitsForANewerPicture(t *testing.T) {
	c, conn := mirroringBoard(t, "")
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "esc"}); r.Status != "ok" {
		t.Fatalf("keys: %+v", r)
	}
	start := time.Now()
	r, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: 200})
	if r.Status != "ok" || data["stale"] != true || !strings.Contains(data["warning"].(string), "no new picture") {
		t.Fatalf("no stale mark: %+v", data["warning"])
	}
	if d := time.Since(start); d < 200*time.Millisecond || d > 2*time.Second {
		t.Fatalf("waited %v", d)
	}
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "esc"}); r.Status != "ok" {
		t.Fatalf("keys: %+v", r)
	}
	go func() {
		time.Sleep(150 * time.Millisecond)
		img := grey(718, 1560)
		img.Pix[0] = 10
		conn.onFrame(video.Frame{Img: img})
	}()
	r, data = call(t, c, Request{Cmd: "screenshot", Format: "jpeg"})
	if r.Status != "ok" || data["stale"] != false || data["warning"] != nil {
		t.Fatalf("the new picture: %+v %v", r, data["warning"])
	}
	if age := data["age_ms"].(float64); age > 5000 {
		t.Fatalf("age %v", age)
	}
}

// The board says the sender keeps the session up but sends no picture after input: the CLI says what to do.
func TestScreenshotPassesOnTheBoardsStall(t *testing.T) {
	c, _ := mirroringBoard(t, `{"fw":"0.2.2","abs":1,"up":2000,"ap":"streaming","fz":0,"stall":1}`)
	_, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: -1})
	if data["stale"] != true || !strings.Contains(data["warning"].(string), "restart Screen Mirroring") {
		t.Fatalf("stall: %v", data["warning"])
	}
}

// Text goes by the board's layout: ä stays on German, and what US cannot type is named.
func TestTextFollowsTheKeyboardLayout(t *testing.T) {
	b, c, _ := absBoard(t)
	b.Handle = func(cmd string) string {
		if cmd == "kbd layout de" {
			return "ok de"
		}
		return "ok"
	}
	if r, data := call(t, c, Request{Cmd: "keyboard", Text: "layout de-DE"}); r.Status != "ok" || data["layout"] != "de" {
		t.Fatalf("keyboard layout: %+v", r)
	}
	if r, data := call(t, c, Request{Cmd: "text", Text: "Grüße"}); r.Status != "ok" || data["not_typed"] != nil {
		t.Fatalf("text on de: %+v", data)
	}
	if got := sent(b); !strings.Contains(got, "type Grüße") {
		t.Fatalf("sent %s", got)
	}
	if r, _ := call(t, c, Request{Cmd: "keyboard", Text: "layout fr"}); r.Status != "error" {
		t.Fatalf("unknown layout: %+v", r)
	}
	b.Status(`{"fw":"0.2.2","abs":1,"up":3000,"ap":"streaming","kbd":"us","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	b.Commands = nil
	r, data := call(t, c, Request{Cmd: "text", Text: "Grüße"})
	if r.Status != "ok" || sent(b) != "type Grue" || !strings.Contains(data["note"].(string), "ü ß") {
		t.Fatalf("text on us: sent %s, %+v", sent(b), data)
	}
}
