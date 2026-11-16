package daemon

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/proto"
	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

// statusOnReconnect makes the board send status once the CLI starts its stream on a new session (as the real one
// does within a second).
func statusOnReconnect(b *proto.FakeBoard, status string) {
	b.Handle = func(cmd string) string {
		if cmd == "stream on" || strings.HasPrefix(cmd, "stream on at") {
			go func() { time.Sleep(20 * time.Millisecond); b.Status(status) }()
		}
		return "ok"
	}
}

// After the board restarted (an update), device info shows the new firmware and nothing of the old session's
// status: the cache once showed the old version for a while (2026-11-16).
func TestInfoAfterARestartShowsTheNewStatus(t *testing.T) {
	b := fakeBoard()
	c, _ := start(t, fakeTransport{b})
	if r, _ := call(t, c, Request{Cmd: "pair"}); r.Status != "ok" {
		t.Fatalf("pair: %+v", r)
	}
	b.Status(`{"fw":"0.3.0","up":500000,"ap":"streaming","heap":1234,"fz":0}`)
	time.Sleep(50 * time.Millisecond)
	statusOnReconnect(b, `{"fw":"0.3.1","up":1200,"ap":"idle","fz":0}`) // the compact form: no heap
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	r, data := call(t, c, Request{Cmd: "info"})
	if r.Status != "ok" || data["fw"] != "0.3.1" || data["ap"] != "idle" || data["stale"] != nil {
		t.Fatalf("info after the restart: %+v", data)
	}
	if _, ok := data["heap"]; ok {
		t.Fatalf("a key of the last session survived: %+v", data)
	}
}

// No status since the link came back: info says so, and the last session's values are only under "previous".
func TestInfoWithoutAFreshStatusIsMarkedStale(t *testing.T) {
	old := infoWait
	infoWait = 200 * time.Millisecond
	defer func() { infoWait = old }()
	b := fakeBoard()
	c, _ := start(t, fakeTransport{b})
	call(t, c, Request{Cmd: "pair"})
	b.Status(`{"fw":"0.3.0","up":500000,"ap":"streaming","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	r, data := call(t, c, Request{Cmd: "info"})
	prev, _ := data["previous"].(map[string]any)
	if r.Status != "ok" || data["stale"] != true || data["fw"] != nil || data["ap"] != nil || prev["fw"] != "0.3.0" {
		t.Fatalf("info without a fresh status: %+v", data)
	}
}

// A screenshot right after a reconnect goes by this session's status: the last one's "idle" is not "mirroring is
// off" while the phone now mirrors (2026-11-16).
func TestScreenshotAfterAReconnectUsesTheNewStatus(t *testing.T) {
	b, c, s := absBoard(t)
	b.Status(`{"fw":"0.3.0","abs":1,"up":2000,"ap":"idle","fz":0}`)
	time.Sleep(50 * time.Millisecond)
	conn, err := s.conn(context.Background(), "")
	if err != nil {
		t.Fatal(err)
	}
	conn.mu.Lock()
	conn.ffmpeg = "ffmpeg" // frames come from onFrame here, never from a decoder
	conn.mu.Unlock()
	conn.onFrame(video.Frame{Img: grey(718, 1560)})
	statusOnReconnect(b, `{"fw":"0.3.0","abs":1,"up":3000,"ap":"streaming","fz":0}`)
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	if r, _ := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: -1}); r.Status != "ok" {
		t.Fatalf("screenshot after the reconnect: %+v", r)
	}
}
