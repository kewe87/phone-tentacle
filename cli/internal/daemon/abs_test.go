package daemon

import (
	"context"
	"image"
	"image/color"
	"strings"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/proto"
	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

// absBoard is a paired fake board with an absolute pointer; the commands it got are cleared.
func absBoard(t *testing.T) (*proto.FakeBoard, *Client, *Server) {
	t.Helper()
	b := fakeBoard()
	c, s := start(t, fakeTransport{b})
	if r, _ := call(t, c, Request{Cmd: "pair"}); r.Status != "ok" {
		t.Fatalf("pair: %+v", r)
	}
	b.Status(`{"fw":"0.3.0","abs":1,"up":1000,"ap":"streaming","w":480,"h":1040,"fz":0}`)
	time.Sleep(50 * time.Millisecond)
	b.Commands = nil
	return b, c, s
}

func sent(b *proto.FakeBoard) string { return strings.Join(b.Commands, ",") }

// Taps and swipes on an abs board: screenshot pixels as fractions of the screen in 0..32767, the press only after
// the move, no calibration or homing.
func TestAbsBoardTapsAndSwipesAbsolutely(t *testing.T) {
	b, c, _ := absBoard(t)
	if r, data := call(t, c, Request{Cmd: "calibrate"}); r.Status != "ok" || !strings.Contains(data["message"].(string), "not needed") {
		t.Fatalf("calibrate: %+v", r)
	}
	if len(b.Commands) != 0 {
		t.Fatalf("calibrate sent %s", sent(b))
	}
	// no frame yet: the AirPlay size from the status (480x1040)
	if r, _ := call(t, c, Request{Cmd: "tap", Pts: []float64{240, 260}, Count: 2, Aim: true}); r.Status != "ok" {
		t.Fatalf("tap: %+v", r)
	}
	if got := sent(b); got != "release,abs 16384 8192,click,click" {
		t.Fatalf("tap sent %s", got)
	}
	b.Commands = nil
	if r, _ := call(t, c, Request{Cmd: "swipe", Pts: []float64{120, 832, 120, 208}}); r.Status != "ok" {
		t.Fatalf("swipe: %+v", r)
	}
	if got := sent(b); got != "abs 8192 26214,press,abs 8192 6553,release" {
		t.Fatalf("swipe sent %s", got)
	}
	b.Commands = nil
	if r, _ := call(t, c, Request{Cmd: "longpress", Pts: []float64{480, 1040}, Ms: 10}); r.Status != "ok" {
		t.Fatalf("longpress: %+v", r)
	}
	if got := sent(b); got != "abs 32767 32767,press,release" {
		t.Fatalf("longpress sent %s", got)
	}
}

// The same board without "abs" in a later status (flashed back to a relative build) is calibrated again.
func TestRelativeStatusEndsAbsMode(t *testing.T) {
	b, c, s := absBoard(t)
	b.Status(`{"fw":"0.3.0","up":2000,"ap":"idle","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	conn, err := s.conn(context.Background(), "")
	if err != nil || conn.Abs(context.Background()) {
		t.Fatalf("still abs after a relative status (%v)", err)
	}
	if r, _ := call(t, c, Request{Cmd: "calibrate"}); r.Status != "error" {
		t.Fatalf("calibrate on a relative board without video: %+v", r)
	}
}

// A frame with bars around the picture (a 9:16 phone in a 480x1040 frame): pixels count inside the picture.
func TestAbsTapsCountInsideTheBars(t *testing.T) {
	b, c, s := absBoard(t)
	conn, _ := s.conn(context.Background(), "")
	img := image.NewRGBA(image.Rect(0, 0, 480, 1040))
	for y := 93; y < 1040-93; y++ { // 480x854: 9:16
		for x := 0; x < 480; x++ {
			img.Set(x, y, color.RGBA{200, 200, 200, 255})
		}
	}
	conn.onFrame(video.Frame{Img: img})
	if r, _ := call(t, c, Request{Cmd: "tap", Pts: []float64{240, 93}}); r.Status != "ok" {
		t.Fatalf("tap: %+v", r)
	}
	if got := sent(b); got != "release,abs 16384 0,click" {
		t.Fatalf("tap sent %s", got)
	}
}

// Right after a reconnect the last session's status ("idle") is not taken for the board's word: a status of the
// new session says it streams.
func TestVideoStateWaitsForTheNewSessionsStatus(t *testing.T) {
	b, c, s := absBoard(t)
	b.Status(`{"fw":"0.3.0","abs":1,"up":3000,"ap":"idle","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "esc"}); r.Status != "ok" { // reconnects
		t.Fatalf("keys: %+v", r)
	}
	go func() {
		time.Sleep(300 * time.Millisecond)
		b.Status(`{"fw":"0.3.0","abs":1,"up":4000,"ap":"streaming","fz":0}`)
	}()
	conn, _ := s.conn(context.Background(), "")
	if err := s.videoState(context.Background(), conn); err == errMirroring {
		t.Fatal("mirroring off from the old session's status")
	}
	b.Status(`{"fw":"0.3.0","abs":1,"up":5000,"ap":"idle","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	if err := s.videoState(context.Background(), conn); err != errMirroring {
		t.Fatalf("idle board: %v", err)
	}
}

func TestScreenRect(t *testing.T) {
	for _, tc := range []struct {
		w, h int
		bars [4]int
		want image.Rectangle
	}{
		{718, 1560, [4]int{}, image.Rect(0, 0, 718, 1560)},
		{718, 1560, [4]int{0, 15, 0, 15}, image.Rect(0, 0, 718, 1560)},  // black top and bottom of a dark app
		{480, 1040, [4]int{0, 93, 0, 93}, image.Rect(0, 93, 480, 947)},  // letterbox
		{480, 1040, [4]int{0, 93, 0, 40}, image.Rect(0, 0, 480, 1040)},  // not centred: no bars
		{600, 1040, [4]int{60, 0, 60, 0}, image.Rect(60, 0, 540, 1040)}, // pillarbox
	} {
		if got := screenRect(tc.w, tc.h, tc.bars); got != tc.want {
			t.Errorf("screenRect(%d, %d, %v) = %v, want %v", tc.w, tc.h, tc.bars, got, tc.want)
		}
	}
}
