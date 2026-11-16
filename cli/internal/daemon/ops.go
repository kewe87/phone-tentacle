package daemon

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/jpeg"
	"image/png"
	"io"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/kewe87/phone-tentacle/cli/internal/config"
	"github.com/kewe87/phone-tentacle/cli/internal/pointer"
	"github.com/kewe87/phone-tentacle/cli/internal/video"
	"golang.org/x/image/draw"
)

var (
	errMirroring = errors.New("screen mirroring is off: on the iPhone open Control Center > Screen Mirroring > Tentacle")
	errFrozen    = errors.New("video frozen: restart Screen Mirroring on the iPhone (Control Center > Screen Mirroring > Stop Mirroring, then Tentacle)")
	// errNoFrame: coordinates are pixels of a screenshot, and without a frame its size is unknown. The AirPlay size
	// in the status is not it (480x1040 asked, 718x1560 sent: a tap at x 700 landed on the right edge, 2026-11-16).
	errNoFrame = errors.New("no screen picture yet, so the coordinates cannot be placed: take a screenshot first " +
		"(Screen Mirroring must be on: Control Center > Screen Mirroring > Tentacle)")
)

// DefaultFreshWait is how long a screenshot waits for a picture newer than the last input.
const DefaultFreshWait = 3 * time.Second

func num(v any) float64 { f, _ := v.(float64); return f }

// videoState explains why there is no usable screen, nil when mirroring streams.
func (s *Server) videoState(ctx context.Context, c *Conn) error {
	st, _ := c.StatusNow(ctx, 3*time.Second) // the board sends one a second; one from before a reconnect may be stale
	if num(st["fz"]) == 1 {
		return errFrozen
	}
	// Not streaming by the status: the board may have computed it just before this session, or AirPlay just
	// started. Video coming in on this session, or the next status, says otherwise.
	for t := time.Now(); !videoAlive(st, c); st, _ = c.Status() {
		if time.Since(t) > 1500*time.Millisecond || sleepCtx(ctx, 100*time.Millisecond) != nil {
			return errMirroring
		}
	}
	c.mu.Lock()
	ff := c.ffmpeg
	c.mu.Unlock()
	if ff == "" {
		_, err := video.FFmpeg()
		if err == nil {
			err = errors.New("ffmpeg could not decode the video (see daemon.log)")
		}
		return err
	}
	return nil
}

// videoAlive: mirroring streams by the status, or video came in on this session; or the AirPlay session is up
// ("connected") and there is a picture: iOS sends none while the screen stands still, also for 10-28 s, and the
// status then said "connected" (2026-11-16). The picture is still the screen; its age goes with it (frame()).
func videoAlive(st map[string]any, c *Conn) bool {
	return st["ap"] == "streaming" || c.streaming() || st["ap"] == "connected" && c.Frame() != nil
}

// shot is a screenshot's frame and what is known of its freshness.
type shot struct {
	img      image.Image
	captured time.Time // when the board got it (local clock)
	stale    bool      // input went to the phone after it, and no newer picture came within the wait
	warning  string
}

// frame returns the screen as it is after the last action (like the upstream app's VideoFeed.afterAction). The
// iPhone sends a picture only when the screen changes: when input went to the phone after the latest picture,
// it waits up to wait for a newer one, else the latest is returned as stale.
func (s *Server) frame(ctx context.Context, c *Conn, wait time.Duration) (shot, error) {
	if err := s.videoState(ctx, c); err != nil {
		return shot{}, err
	}
	if c.Frame() == nil && c.Fresh(ctx, time.Time{}, 6*time.Second) == nil {
		// nothing streamed yet (the stream starts at the next keyframe): ask for the current GOP
		sctx, cancel := context.WithTimeout(ctx, 60*time.Second)
		_, err := c.Send(sctx, "snap")
		cancel()
		if err != nil {
			return shot{}, fmt.Errorf("no video from the board: %w", err)
		}
		if c.Fresh(ctx, time.Time{}, 5*time.Second) == nil {
			return shot{}, errors.New("no video from the board yet; try again")
		}
	}
	c.mu.Lock()
	acted, input, last := c.lastAction, c.lastInput, c.lastShot
	c.mu.Unlock()
	var out shot
	// a picture from after the input began: a tap's change comes after its click, and the pointer is not in it
	if captured, _, _, _ := c.times(); !input.IsZero() && input.After(captured) && c.Fresh(ctx, input, wait) == nil {
		out.stale = true
		out.warning = fmt.Sprintf("no new picture since the last input %.1f s ago: the screen may not have changed, "+
			"or the iPhone stopped sending pictures. If a change was expected, take another screenshot; if it stays "+
			"old, restart Screen Mirroring on the iPhone (Control Center > Screen Mirroring > Tentacle)", time.Since(input).Seconds())
	} else if acted.After(last) {
		c.Settled(ctx, 500*time.Millisecond, 300*time.Millisecond, 3*time.Second)
	}
	c.mu.Lock()
	c.lastShot = time.Now()
	out.img, out.captured = c.frame, c.captured
	c.mu.Unlock()
	st, _ := c.Status()
	if num(st["fz"]) == 1 {
		return shot{}, errFrozen
	}
	if num(st["stall"]) == 1 { // the board saw input, heartbeats, and no picture for 15 s (firmware 0.2.2)
		out.stale = true
		out.warning = "the iPhone sends no pictures any more (mirroring is up, but nothing came for a while after input): " +
			"restart Screen Mirroring on the iPhone (Control Center > Screen Mirroring > Stop Mirroring, then Tentacle)"
	}
	return out, nil
}

func encode(img image.Image, format string, quality int, scale float64, maxSize int) ([]byte, image.Rectangle, error) {
	b := img.Bounds()
	w, h := float64(b.Dx()), float64(b.Dy())
	if scale <= 0 || scale > 1 {
		scale = 1
	}
	if maxSize > 0 && max(w, h)*scale > float64(maxSize) {
		scale = float64(maxSize) / max(w, h)
	}
	if scale < 1 {
		dst := image.NewRGBA(image.Rect(0, 0, max(1, int(w*scale+0.5)), max(1, int(h*scale+0.5))))
		draw.CatmullRom.Scale(dst, dst.Bounds(), img, b, draw.Src, nil)
		img = dst
	}
	var buf bytes.Buffer
	var err error
	switch format {
	case "", "png":
		err = (&png.Encoder{CompressionLevel: png.BestSpeed}).Encode(&buf, img)
	case "jpeg", "jpg":
		if quality <= 0 || quality > 100 {
			quality = 90
		}
		err = jpeg.Encode(&buf, img, &jpeg.Options{Quality: quality})
	default:
		err = fmt.Errorf("format must be png or jpeg")
	}
	return buf.Bytes(), img.Bounds(), err
}

func (s *Server) screenshot(ctx context.Context, c *Conn, req Request) Response {
	wait := DefaultFreshWait
	if req.WaitMs != 0 {
		wait = time.Duration(max(req.WaitMs, 0)) * time.Millisecond
	}
	sh, err := s.frame(ctx, c, wait)
	if err != nil {
		return fail(err)
	}
	img := sh.img
	data, r, err := encode(img, req.Format, req.Quality, req.Scale, req.MaxSize)
	if err != nil {
		return fail(err)
	}
	format := "png"
	if req.Format == "jpeg" || req.Format == "jpg" {
		format = "jpeg"
	}
	s.recordShot(img, r.Dx(), r.Dy())
	out := map[string]any{"format": format, "data": base64.StdEncoding.EncodeToString(data),
		"width": r.Dx(), "height": r.Dy(), "device": c.ID, "stale": sh.stale}
	if !sh.captured.IsZero() {
		out["age_ms"] = max(time.Since(sh.captured).Milliseconds(), 0)
		out["captured_at"] = sh.captured.Format(capturedFormat)
	}
	if sh.warning != "" {
		out["warning"] = sh.warning
	}
	return ok(out)
}

const capturedFormat = "2006-01-02T15:04:05.000Z07:00"

func (c *Conn) pointer(dir string) *pointer.Pointer {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.ptr == nil {
		c.ptr = pointer.New(device{c}, c, config.CalibrationPath(dir, c.ID))
	}
	return c.ptr
}

// size is the screenshot size coordinates refer to: the latest frame this daemon got from the board (kept across
// pauses in mirroring and reconnects); none yet is errNoFrame.
func (c *Conn) size() (float64, float64, error) {
	if f := c.Frame(); f != nil {
		return float64(f.Bounds().Dx()), float64(f.Bounds().Dy()), nil
	}
	return 0, 0, errNoFrame
}

func (s *Server) calibrate(ctx context.Context, c *Conn) Response {
	if c.Abs(ctx) {
		return ok(map[string]any{"abs": true, "message": "not needed: this board positions the pointer absolutely (abs build)"})
	}
	if err := s.videoState(ctx, c); err != nil {
		return fail(err)
	}
	if _, err := s.frame(ctx, c, DefaultFreshWait); err != nil {
		return fail(err)
	}
	p := c.pointer(s.Dir)
	if err := p.Calibrate(ctx); err != nil {
		return fail(err)
	}
	out := map[string]any{"span": map[string]int{"w": p.Span.W, "h": p.Span.H}}
	if e, found, err := p.Verify(ctx, 0.5, 0.62); err == nil && found {
		out["error"] = e // miss at a check point, fraction of the screen
	}
	c.mu.Lock()
	c.lastAction = time.Now()
	c.mu.Unlock()
	return ok(out)
}

// textChunk keeps one `type` line well inside one command (the board takes 512 bytes).
const textChunk = 160

func (s *Server) io(ctx context.Context, c *Conn, req Request) Response {
	p := c.pointer(s.Dir)
	began := time.Now()
	err := func() error {
		switch req.Cmd {
		case "text":
			p.Layout = c.layout(ctx)
			typed, err := "", error(nil)
			for rest := pointer.TypeableOn(p.Layout, req.Text); rest != "" && err == nil; {
				n := min(len(rest), textChunk)
				for n > 0 && !utf8.RuneStart(rest[n-1]) {
					n--
				}
				_, err = p.Type(ctx, rest[:n])
				typed, rest = typed+rest[:n], rest[n:]
			}
			if typed == "" && err == nil {
				err = fmt.Errorf("nothing typeable in that text on the %s keyboard layout (see tentacle keyboard layout)", layoutName(p.Layout))
			}
			return err
		case "keys":
			return p.Key(ctx, strings.ToLower(req.Text))
		case "button":
			if !strings.EqualFold(req.Text, "HOME") {
				return fmt.Errorf("button %s not supported on Tentacle (only HOME)", req.Text)
			}
			p.Abs = c.Abs(ctx) // the swipe up, should the Home key and Cmd+H not do it
			return p.GoHome(ctx)
		}
		p.Abs = c.Abs(ctx)
		w, h, err := c.size()
		if err != nil {
			return err
		}
		pt := func(i int) (float64, float64) { return req.Pts[i] / w, req.Pts[i+1] / h }
		if p.Abs { // fractions of the phone screen, not of the frame (area.go)
			r, err := c.screenArea()
			if err != nil {
				return err
			}
			pt = func(i int) (float64, float64) {
				return (req.Pts[i] - float64(r.Min.X)) / float64(r.Dx()), (req.Pts[i+1] - float64(r.Min.Y)) / float64(r.Dy())
			}
		}
		if !p.Calibrated() {
			if s.videoState(ctx, c) != nil {
				return errors.New("pointer not calibrated, and the screen is not visible: turn on Screen Mirroring, then run tentacle calibrate")
			}
			if err := p.Calibrate(ctx); err != nil {
				return err
			}
		}
		need := 2
		if req.Cmd == "swipe" {
			need = 4
		}
		if len(req.Pts) < need {
			return fmt.Errorf("%s needs %d coordinates, got %d", req.Cmd, need, len(req.Pts))
		}
		switch req.Cmd {
		case "tap":
			x, y := pt(0)
			return p.Tap(ctx, x, y, req.Aim, req.Count)
		case "longpress":
			x, y := pt(0)
			d := time.Duration(req.Ms) * time.Millisecond
			if d <= 0 {
				d = time.Second
			}
			return p.LongPress(ctx, x, y, d)
		case "swipe":
			x1, y1 := pt(0)
			x2, y2 := pt(2)
			return p.Swipe(ctx, x1, y1, x2, y2, true)
		}
		return nil
	}()
	c.mu.Lock()
	c.lastAction, c.lastInput = time.Now(), began
	c.mu.Unlock()
	s.recordAction(c, req, err)
	if err != nil {
		return fail(err)
	}
	out := map[string]any{"message": req.Cmd + " done"}
	if req.Cmd == "text" {
		var notes []string
		if pointer.HasHan(req.Text) {
			notes = append(notes, "needs the iPhone's Chinese (pinyin) keyboard; if letters appear, tap the globe key and type again")
		}
		if bad := pointer.NotTyped(p.Layout, req.Text); len(bad) > 0 {
			out["not_typed"] = bad
			notes = append(notes, fmt.Sprintf("the %s keyboard layout has no keys for %s: typed without accents or left out "+
				"(a German iPhone: tentacle keyboard layout de)", layoutName(p.Layout), strings.Join(bad, " ")))
		}
		if len(notes) > 0 {
			out["typed_as"] = pointer.TypeableOn(p.Layout, req.Text)
			out["note"] = strings.Join(notes, "; ")
		}
	}
	return ok(out)
}

// layout is the phone keyboard layout the board types with ("kbd" in its status); "" from a board without layouts.
// layout is the board's keyboard layout: from its status, else (right after connecting, before the first status)
// asked from the board, so a fresh daemon does not type a German phone's text on US keys.
func (c *Conn) layout(ctx context.Context) string {
	st, _ := c.Status()
	if l, _ := st["kbd"].(string); l != "" {
		return l
	}
	reply, err := c.Send(ctx, "kbd layout")
	if err != nil {
		return "" // a board from before layouts: US
	}
	l := strings.TrimSpace(reply)
	c.mu.Lock()
	if c.status != nil {
		c.status["kbd"] = l
	}
	c.mu.Unlock()
	return l
}

func layoutName(l string) string {
	if l == "" {
		return "us"
	}
	return l
}

// keyboard runs `keyboard layout [CODE]`: the phone's keyboard layout the board types with (kept on the board).
func (s *Server) keyboard(ctx context.Context, c *Conn, text string) Response {
	f := strings.Fields(text)
	if len(f) == 0 || f[0] != "layout" || len(f) > 2 {
		return fail(fmt.Errorf("usage: tentacle keyboard layout [%s]", strings.Join(pointer.Layouts, "|")))
	}
	line := "kbd layout"
	if len(f) == 2 {
		l := pointer.KnownLayout(f[1])
		if l == "" {
			return fail(fmt.Errorf("unknown layout %q (known: %s)", f[1], strings.Join(pointer.Layouts, ", ")))
		}
		line += " " + l
	}
	reply, err := c.Send(ctx, line)
	if err != nil {
		if strings.HasPrefix(err.Error(), "unknown command") {
			err = errors.New("this board's firmware has no keyboard layouts (US only): update it (0.2.2 or later)")
		}
		return fail(err)
	}
	l := strings.TrimSpace(reply)
	c.mu.Lock()
	c.status["kbd"] = l // the next status says so too
	c.mu.Unlock()
	out := map[string]any{"layout": l}
	if len(f) == 2 {
		out["message"] = "the board now types for the " + l + " layout; the iPhone's hardware keyboard layout must be the same " +
			"(Settings > General > Keyboard > Hardware Keyboard)"
	}
	return ok(out)
}

// video runs `video fps N` and `video res W H`: what the board asks the iPhone for (kept on the board), from the
// next mirroring session on.
func (s *Server) video(ctx context.Context, c *Conn, text string) Response {
	f := strings.Fields(text)
	if len(f) == 0 || !(f[0] == "fps" && len(f) == 2 || f[0] == "res" && len(f) == 3) {
		return fail(errors.New("usage: tentacle video fps N | video res W H"))
	}
	if _, err := c.Send(ctx, text); err != nil {
		return fail(err)
	}
	return ok(map[string]any{"message": text + " set: restart Screen Mirroring on the iPhone to use it (Control Center > " +
		"Screen Mirroring > Stop Mirroring, then Tentacle)"})
}

// screencapture streams raw H.264 (Annex-B, from the next keyframe) or MJPEG, after one JSON header line.
func (s *Server) screencapture(ctx context.Context, req Request, w io.Writer) {
	enc := json.NewEncoder(w)
	c, err := s.conn(ctx, req.Device)
	if err == nil {
		err = s.videoState(ctx, c)
	}
	if err == nil && req.Format != "h264" && req.Format != "mjpeg" {
		err = errors.New("format must be h264 or mjpeg")
	}
	if err != nil {
		enc.Encode(fail(err))
		return
	}
	enc.Encode(ok(map[string]string{"format": req.Format}))
	if req.Format == "h264" {
		ch := make(chan video.AU, 256)
		c.mu.Lock()
		c.subs[ch] = true
		config, gop := c.config, append([]video.AU{}, c.gop...)
		c.mu.Unlock()
		defer func() { c.mu.Lock(); delete(c.subs, ch); c.mu.Unlock() }()
		started := len(gop) > 0 && config != nil
		if started {
			w.Write(config)
			for _, au := range gop {
				w.Write(au.Data)
			}
		}
		for {
			select {
			case <-ctx.Done():
				return
			case au := <-ch:
				if !started && au.Flags&(video.IDR|video.Config) == 0 {
					continue
				}
				if !started && au.Flags&video.Config == 0 && config != nil {
					w.Write(config)
				}
				started = true
				if _, err := w.Write(au.Data); err != nil {
					return
				}
			}
		}
	}
	ch := make(chan image.Image, 4)
	c.mu.Lock()
	c.frameSubs[ch] = true
	c.mu.Unlock()
	defer func() { c.mu.Lock(); delete(c.frameSubs, ch); c.mu.Unlock() }()
	for {
		select {
		case <-ctx.Done():
			return
		case img := <-ch:
			data, _, _ := encode(img, "jpeg", req.Quality, 1, 0)
			hdr := fmt.Sprintf("--BoundaryString\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(data))
			if _, err := w.Write(append(append([]byte(hdr), data...), "\r\n"...)); err != nil {
				return
			}
		}
	}
}
