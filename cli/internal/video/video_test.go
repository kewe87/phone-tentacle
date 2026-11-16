package video

import (
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func chunk(seq uint16, flags byte, payload []byte) []byte {
	c := make([]byte, 3)
	binary.LittleEndian.PutUint16(c, seq)
	c[2] = flags
	return append(c, payload...)
}

func ts(ms uint32, data string) []byte {
	b := binary.LittleEndian.AppendUint32(nil, ms)
	return append(b, data...)
}

func TestReassembler(t *testing.T) {
	r := NewReassembler()
	if au := r.Feed(chunk(1, Start|End, ts(5, "P"))); au != nil {
		t.Fatal("a P-frame before any IDR was kept")
	}
	if au := r.Feed(chunk(2, Start|Config|End, ts(10, "SPS"))); au == nil || string(au.Data) != "SPS" || au.TS != 10 {
		t.Fatalf("config AU %+v", au)
	}
	r.Feed(chunk(3, Start|IDR, ts(20, "ab")))
	r.Feed(chunk(4, 0, []byte("cd")))
	au := r.Feed(chunk(5, End, []byte("ef")))
	if au == nil || string(au.Data) != "abcdef" || au.Flags&IDR == 0 || au.TS != 20 {
		t.Fatalf("idr AU %+v", au)
	}
	r.Feed(chunk(6, Bench, []byte("filler")))
	r.Feed(chunk(7, Start, ts(30, "x")))
	if au := r.Feed(chunk(9, End, []byte("y"))); au != nil || r.Gaps != 1 {
		t.Fatalf("AU across a gap: %+v gaps %d", au, r.Gaps)
	}
	if au := r.Feed(chunk(10, Start|End, ts(40, "P"))); au != nil {
		t.Fatal("P-frame after a gap was kept")
	}
	if au := r.Feed(chunk(11, Start|End|IDR, ts(50, "I"))); au == nil {
		t.Fatal("IDR after a gap was dropped")
	}
	if au := r.Feed(chunk(0xFFFF, Start|End|IDR, ts(60, "I"))); au == nil || r.Gaps != 2 {
		t.Fatal("seq jump")
	}
	if au := r.Feed(chunk(0, Start|End, ts(70, "P"))); au == nil || r.Gaps != 2 {
		t.Fatal("seq wrap counted as a gap")
	}
}

// splitAUs cuts an Annex-B stream at access unit delimiters (x264 aud=1).
func splitAUs(b []byte) [][]byte {
	var out [][]byte
	start := -1
	for i := 0; i+4 < len(b); i++ {
		if b[i] == 0 && b[i+1] == 0 && b[i+2] == 0 && b[i+3] == 1 && b[i+4]&0x1f == 9 {
			if start >= 0 {
				out = append(out, b[start:i])
			}
			start = i
		}
	}
	return append(out, b[start:])
}

// Real ffmpeg: every AU fed comes out as one picture right away (no reordering delay), with its time.
func TestDecoderOneInOneOut(t *testing.T) {
	ff, err := FFmpeg()
	if err != nil {
		t.Skip(err)
	}
	file := filepath.Join(t.TempDir(), "t.h264")
	gen := exec.Command(ff, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=160x320:rate=5",
		"-frames:v", "8", "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline", "-g", "4",
		"-x264-params", "aud=1", "-f", "h264", file)
	if out, err := gen.CombinedOutput(); err != nil {
		t.Skipf("no libx264: %s", out)
	}
	raw, _ := os.ReadFile(file)
	frames := make(chan Frame, 16)
	d, err := NewDecoder(ff, func(f Frame) { frames <- f })
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	for i, au := range splitAUs(raw) {
		d.Write(AU{Flags: Start | End, TS: uint32(100 + i), Data: au[6:]}) // without its AUD, like AirPlay's
		select {
		case f := <-frames:
			if f.TS != uint32(100+i) {
				t.Fatalf("frame %d has time %d", i, f.TS)
			}
			if b := f.Img.Bounds(); b.Dx() != 160 || b.Dy() != 320 {
				t.Fatalf("frame %d is %v", i, b)
			}
		case <-time.After(3 * time.Second):
			t.Fatalf("AU %d gave no picture before the next one", i)
		}
	}
}

// A replayed GOP goes into ffmpeg all at once, far ahead of what it has decoded: every picture still carries its
// own AU's time and BEHIND flag. Only the newest 30 times were kept before, and pictures from minutes ago came out
// with the time of the AUs just written ("39 ms old", 2026-11-16).
func TestDecoderReplayKeepsEachPicturesTime(t *testing.T) {
	ff, err := FFmpeg()
	if err != nil {
		t.Skip(err)
	}
	const n = 300
	file := filepath.Join(t.TempDir(), "t.h264")
	gen := exec.Command(ff, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=64x64:rate=30",
		"-frames:v", fmt.Sprint(n), "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline", "-g", "1000",
		"-x264-params", "aud=1", "-f", "h264", file)
	if out, err := gen.CombinedOutput(); err != nil {
		t.Skipf("no libx264: %s", out)
	}
	raw, _ := os.ReadFile(file)
	aus := splitAUs(raw)
	frames := make(chan Frame, len(aus))
	d, err := NewDecoder(ff, func(f Frame) { frames <- f })
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	for i, au := range aus {
		flags := byte(Start | End)
		if i < len(aus)-1 {
			flags |= Behind
		}
		if !d.Write(AU{Flags: flags, TS: uint32(1000 + 33*i), Data: au[6:]}) {
			t.Fatalf("AU %d not taken", i)
		}
	}
	for i := range aus {
		select {
		case f := <-frames:
			if f.TS != uint32(1000+33*i) || f.Behind != (i < len(aus)-1) {
				t.Fatalf("picture %d: time %d behind %v", i, f.TS, f.Behind)
			}
		case <-time.After(10 * time.Second):
			t.Fatalf("only %d of %d pictures", i, len(aus))
		}
	}
}

// An AU that gave no picture (ffmpeg dropped it) is forgotten once ffmpeg had a second with nothing to do, so the
// pictures after it are not paired with older AUs from then on.
func TestStampsForgetAUsWithoutAPicture(t *testing.T) {
	var s stamps
	t0 := time.Now()
	s.push(stamp{ts: 1}, t0)
	s.push(stamp{ts: 2}, t0)
	if st := s.pop(t0.Add(10 * time.Millisecond)); st.ts != 1 {
		t.Fatalf("first picture: %d", st.ts)
	}
	// AU 2 never gave a picture; the next AU comes 5 s later
	s.push(stamp{ts: 3, behind: true}, t0.Add(5*time.Second))
	if st := s.pop(t0.Add(5*time.Second + 10*time.Millisecond)); st.ts != 3 || !st.behind {
		t.Fatalf("picture after a dropped AU: %+v", st)
	}
	// a burst (a replay): nothing is forgotten while AUs keep coming
	for i := 0; i < 1000; i++ {
		s.push(stamp{ts: uint32(10 + i)}, t0.Add(6*time.Second))
	}
	if st := s.pop(t0.Add(9 * time.Second)); st.ts != 10 || s.len() != 999 {
		t.Fatalf("burst: %+v, %d left", st, s.len())
	}
}
