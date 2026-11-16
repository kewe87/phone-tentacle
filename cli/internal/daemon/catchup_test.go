package daemon

import (
	"bytes"
	"context"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

// The board's clock runs 60 s ahead of the picture: its `up` is 61000, the picture was taken at 1000.
const replayStatus = `{"fw":"0.2.3","abs":1,"up":61000,"ap":"streaming","fz":0,"vbl":2532550}`

// After a reconnect the board replays its GOP: a picture from it (BEHIND) is not the screen. The screenshot waits
// for the live edge within --wait, then says so: stale, catching_up with what is left, and the true age (2026-11-16:
// minutes-old pictures came back as "39 ms old", not stale).
func TestScreenshotWhileCatchingUp(t *testing.T) {
	c, conn := mirroringBoard(t, replayStatus)
	conn.onFrame(video.Frame{Img: grey(718, 1560), TS: 1000, Behind: true})
	start := time.Now()
	r, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: 300})
	if r.Status != "ok" || data["stale"] != true {
		t.Fatalf("not stale: %+v %v", r, data["stale"])
	}
	if d := time.Since(start); d < 300*time.Millisecond || d > 3*time.Second {
		t.Fatalf("waited %v", d)
	}
	cu, _ := data["catching_up"].(map[string]any)
	if cu == nil || cu["bytes_left"] != float64(2532550) || num(cu["behind_ms"]) < 59000 {
		t.Fatalf("catching_up %v", data["catching_up"])
	}
	if w, _ := data["warning"].(string); !strings.Contains(w, "NOT the current screen") || !strings.Contains(w, "2474 KB") {
		t.Fatalf("warning %q", w)
	}
	if age := num(data["age_ms"]); age < 59000 || age > 62000 {
		t.Fatalf("age %v, want the board's 60 s", age)
	}
}

// The live edge comes within the wait: that picture, not stale.
func TestScreenshotWaitsForTheLiveEdge(t *testing.T) {
	c, conn := mirroringBoard(t, replayStatus)
	conn.onFrame(video.Frame{Img: grey(718, 1560), TS: 1000, Behind: true})
	go func() {
		time.Sleep(150 * time.Millisecond)
		conn.onFrame(video.Frame{Img: grey(718, 1560), TS: 60900}) // the last one of the replay: no BEHIND
	}()
	r, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: 3000})
	if r.Status != "ok" || data["stale"] != false || data["catching_up"] != nil || data["warning"] != nil {
		t.Fatalf("live edge: %+v %v", data["catching_up"], data["warning"])
	}
	if age := num(data["age_ms"]); age > 2000 {
		t.Fatalf("age %v", age)
	}
}

// A picture just one or two frames behind (a link that barely keeps up) is the screen.
func TestScreenshotSlightlyBehindIsLive(t *testing.T) {
	c, conn := mirroringBoard(t, replayStatus)
	conn.onFrame(video.Frame{Img: grey(718, 1560), TS: 60950, Behind: true})
	if r, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: -1}); r.Status != "ok" || data["stale"] != false {
		t.Fatalf("slightly behind: %+v %v", r, data["warning"])
	}
}

// A still screen sends nothing: its last picture is the screen however old, and its age is when it was taken.
func TestAgeIsWhenTheBoardGotThePicture(t *testing.T) {
	c, conn := mirroringBoard(t, replayStatus)
	conn.onFrame(video.Frame{Img: grey(718, 1560), TS: 1000})
	r, data := call(t, c, Request{Cmd: "screenshot", Format: "jpeg", WaitMs: -1})
	if r.Status != "ok" || data["stale"] != false {
		t.Fatalf("still screen: %+v %v", r, data["warning"])
	}
	if age := num(data["age_ms"]); age < 59000 || age > 62000 {
		t.Fatalf("age %v", age)
	}
}

// The board rebooted (its `up` went back): its clock starts over; the shown picture keeps its time.
func TestClockStartsOverAfterAReboot(t *testing.T) {
	_, conn := mirroringBoard(t, replayStatus)
	conn.onFrame(video.Frame{Img: grey(718, 1560), TS: 60000})
	conn.mu.Lock()
	before := conn.captured
	conn.mu.Unlock()
	conn.onStatus([]byte(`{"fw":"0.2.3","up":500,"ap":"streaming"}`))
	conn.mu.Lock()
	defer conn.mu.Unlock()
	if conn.offset == nil || conn.captured != before || conn.frameTS != 0 {
		t.Fatalf("after the reboot: offset %v, captured moved %v", conn.offset, conn.captured.Sub(before))
	}
	if off := time.Now().UnixMilli() - 500 - *conn.offset; off < -100 || off > 100 {
		t.Fatalf("offset off by %d ms", off)
	}
}

// After a reconnect the daemon keeps its decoder and asks the board to go on where it is (`stream on at`): no replay
// of minutes of P-frames. A board before 0.2.3 answers `err usage`: then plain `stream on`.
func TestReconnectResumesTheStream(t *testing.T) {
	ff, err := video.FFmpeg()
	if err != nil {
		t.Skip(err)
	}
	file := filepath.Join(t.TempDir(), "t.h264")
	gen := exec.Command(ff, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=64x64:rate=30",
		"-frames:v", "3", "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline", "-g", "1000",
		"-x264-params", "aud=1", "-f", "h264", file)
	if out, err := gen.CombinedOutput(); err != nil {
		t.Skipf("no libx264: %s", out)
	}
	raw, _ := os.ReadFile(file)
	var aus [][]byte
	for _, p := range bytes.Split(raw, []byte{0, 0, 0, 1, 9}) {
		if len(p) > 0 {
			aus = append(aus, append([]byte{0, 0, 0, 1, 9}, p...))
		}
	}
	if len(aus) != 3 {
		t.Fatalf("%d AUs", len(aus))
	}
	b, c, s := absBoard(t)
	conn, _ := s.conn(context.Background(), "")
	conn.mu.Lock()
	conn.ffmpeg = ff
	conn.mu.Unlock()
	for i, au := range aus {
		flags := byte(video.Start | video.End)
		if i == 0 {
			flags |= video.IDR
		}
		b.Video(uint16(i), flags, append(binary.LittleEndian.AppendUint32(nil, uint32(5000+33*i)), au...))
	}
	for deadline := time.Now().Add(3 * time.Second); conn.inGOPNow() != 3; time.Sleep(10 * time.Millisecond) {
		if time.Now().After(deadline) {
			t.Fatalf("the decoder has %d AUs of the GOP", conn.inGOPNow())
		}
	}
	b.Commands = nil
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "esc"}); r.Status != "ok" { // reconnects
		t.Fatalf("keys: %+v", r)
	}
	if got := sent(b); !strings.HasPrefix(got, "stream on at 5000 3,") {
		t.Fatalf("after a reconnect: %s", got)
	}
	b.Handle = func(cmd string) string {
		if strings.HasPrefix(cmd, "stream on at") {
			return "err usage: stream on|off"
		}
		return "ok"
	}
	b.Commands = nil
	b.Drop()
	time.Sleep(50 * time.Millisecond)
	if r, _ := call(t, c, Request{Cmd: "keys", Text: "esc"}); r.Status != "ok" {
		t.Fatalf("keys: %+v", r)
	}
	if got := sent(b); !strings.HasPrefix(got, "stream on at 5000 3,stream on,") {
		t.Fatalf("an older board: %s", got)
	}
}
