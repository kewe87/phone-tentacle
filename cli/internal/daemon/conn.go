package daemon

import (
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"image"
	"log"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/pointer"
	"github.com/kewe87/phone-tentacle/cli/internal/proto"
	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

// Conn is one board: its current session (reconnected on demand), status, latest frame and pointer.
type Conn struct {
	ID   string // board id, 12 hex
	Name string
	addr string // where it was reached (host:port); guarded by the server's mu

	mu       sync.Mutex
	swap     sync.RWMutex // Send holds it shared across a write, attach exclusively: no frame on a replaced session
	sess     *proto.Session
	quit     chan struct{} // stops the session's reader
	links    int
	nextID   int
	pending  map[string]chan string
	status   map[string]any
	statusAt time.Time
	linked   time.Time // when the current session was attached: a status from before it may be stale
	videoAt  time.Time // when the last video chunk arrived

	re        *video.Reassembler
	dec       *video.Decoder
	ffmpeg    string
	frame     image.Image
	frameHash uint64
	captured  time.Time  // when the board got the shown frame from the iPhone (local clock)
	frameTS   uint32     // the same in board ms (0: unknown), to place it again once the clock is known better
	behind    bool       // the board had newer AUs behind the shown frame's: a replay, not the live edge (0.2.3)
	boardUp   uint32     // the latest `up`: a smaller one is a reboot, whose clock starts over
	updated   time.Time  // when it was decoded here
	changed   time.Time  // when the shown image last differed from the one before
	offset    *int64     // local ms − board ms, the smallest seen (≈ the link latency)
	config    []byte     // last SPS/PPS, for screencapture
	gop       []video.AU // AUs since the last IDR, so a new screencapture starts at once
	// The decoder's place, for `stream on at` after a reconnect: it has the first inGOP AUs of the GOP whose IDR is
	// stamped idrTS, none lost (inStep). The board then goes on from there instead of replaying the GOP from its IDR.
	idrTS     uint32
	inGOP     uint32
	inStep    bool
	subs      map[chan video.AU]bool
	frameSubs map[chan image.Image]bool
	wake      chan struct{} // closed and replaced on every new frame
	bars      [4]int        // black edges seen in every frame of this size so far: left, top, right, bottom (area.go)
	barsSize  image.Point

	ptr        *pointer.Pointer
	lastAction time.Time // when the last io command ended
	lastInput  time.Time // when it began: a screenshot wants a picture from after it (ops.go frame())
	lastShot   time.Time

	keyMu   sync.Mutex // held across the gap and the send of key/type commands
	lastKey time.Time  // when the board acknowledged the last one

	hear      *hearing    // phone audio transcribed here (listen.go); nil: never
	rawWanted func() bool // ask each session for the raw audio

	logSubs map[chan string]bool // `logs --board --follow` clients (update.go); each session is asked again
}

// KeyGap separates key and type commands: sent back to back, iOS dropped a backspace (2026-10-21).
const KeyGap = 80 * time.Millisecond

func newConn(id, name, ffmpeg string) *Conn {
	return &Conn{ID: id, Name: name, ffmpeg: ffmpeg, pending: map[string]chan string{}, status: map[string]any{},
		subs: map[chan video.AU]bool{}, frameSubs: map[chan image.Image]bool{}, wake: make(chan struct{}),
		logSubs: map[chan string]bool{}}
}

func (c *Conn) connected() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sess != nil
}

// attach takes over a fresh session. The session it replaces is closed.
func (c *Conn) attach(s *proto.Session) {
	c.swap.Lock()
	defer c.swap.Unlock()
	c.mu.Lock()
	old := c.sess
	if c.quit != nil {
		close(c.quit)
	}
	quit := make(chan struct{})
	c.sess, c.quit, c.links, c.linked = s, quit, c.links+1, time.Now()
	c.re = video.NewReassembler()
	if c.resumable() { // start() asks the board to go on where the decoder is
		c.re = video.Resume()
	} else if c.dec != nil { // the new stream starts over from the GOP
		c.dec.Close()
		c.dec = nil
	}
	c.mu.Unlock() // pending commands stay: the board sends their replies on the new session
	if old != nil {
		old.Close()
	}
	go c.read(s, quit)
}

// start turns on the CLI's video stream on a fresh session; also the client's proof of K (the board sends
// nothing before it).
func (c *Conn) start() error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	c.mu.Lock()
	resume, line := c.resumable(), "stream on"
	if resume {
		line = fmt.Sprintf("stream on at %d %d", c.idrTS, c.inGOP)
	}
	c.mu.Unlock()
	reply, err := c.Send(ctx, line)
	if resume && err != nil && strings.Contains(err.Error(), "usage") { // a board before 0.2.3
		reply, err = c.Send(ctx, "stream on")
	}
	if resume && err == nil {
		if reply == "resumed" {
			log.Printf("%s: video goes on from AU %d of the GOP (no replay)", c.ID, c.inGOPNow())
		} else {
			log.Printf("%s: video replays the GOP from its IDR", c.ID)
		}
	}
	if err == nil { // the board's clock, for its session log; a board before 0.4.1 answers "unknown command"
		now := time.Now()
		_, off := now.Zone()
		c.Send(ctx, fmt.Sprintf("time %d %d", now.Unix(), off/60))
	}
	if err == nil && c.rawWanted != nil && c.rawWanted() {
		c.Send(ctx, "listen raw on") // the board forgets it with every session
	}
	c.mu.Lock()
	following := len(c.logSubs) > 0
	c.mu.Unlock()
	if err == nil && following {
		c.Send(ctx, "logs follow on") // this too
	}
	return err
}

// resumable: the decoder is in step with a GOP the board may still hold; must hold mu.
func (c *Conn) resumable() bool { return c.dec != nil && c.inStep && c.inGOP > 0 }

func (c *Conn) inGOPNow() uint32 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.inGOP
}

// detach drops a session. The decoder stays, so a reconnect can go on where it is (start()).
func (c *Conn) detach(s *proto.Session, why error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess != s {
		return
	}
	s.Close()
	c.sess = nil
	for id, ch := range c.pending {
		close(ch)
		delete(c.pending, id)
	}
	log.Printf("%s: session ended: %v", c.ID, why)
}

func (c *Conn) close() {
	c.mu.Lock()
	s := c.sess
	c.mu.Unlock()
	if s != nil {
		c.detach(s, errors.New("closed"))
	}
	c.mu.Lock()
	if c.dec != nil {
		c.dec.Close()
		c.dec = nil
	}
	c.mu.Unlock()
}

func (c *Conn) read(s *proto.Session, quit chan struct{}) {
	for {
		var f []byte
		var open bool
		select {
		case <-quit:
			return
		case f, open = <-s.Recv():
		}
		if !open {
			break
		}
		m, ok, err := s.Decode(f)
		if err != nil {
			c.detach(s, err)
			return
		}
		if !ok {
			continue
		}
		switch m.Kind {
		case 'r':
			id, rest, _ := strings.Cut(string(m.Data), " ")
			c.mu.Lock()
			if ch := c.pending[id]; ch != nil {
				ch <- rest
				delete(c.pending, id)
			}
			c.mu.Unlock()
		case 's':
			c.onStatus(m.Data)
		case 'l':
			c.onLog(m.Data)
		case 'v':
			c.onVideo(m.Data, m.Gap)
		case 'a':
			c.onAudio(m.Data, m.Gap)
		}
	}
	c.detach(s, proto.ErrSession)
}

// Send runs one command and returns the reply after "ok"; an "err" reply is an error.
func (c *Conn) Send(ctx context.Context, line string) (string, error) {
	c.mu.Lock()
	s := c.sess
	if s == nil {
		c.mu.Unlock()
		return "", proto.ErrSession
	}
	c.nextID++
	id := strconv.Itoa(c.nextID)
	ch := make(chan string, 1)
	c.pending[id] = ch
	c.mu.Unlock()
	c.swap.RLock() // a reconnect may have replaced the session since: send on the current one
	c.mu.Lock()
	if c.sess != nil {
		s = c.sess
	}
	c.mu.Unlock()
	sent := time.Now()
	err := s.Send(id + " " + line)
	c.swap.RUnlock()
	if verbose.Load() { // the command's name only: never typed text
		name, _, _ := strings.Cut(line, " ")
		defer func() {
			log.Printf("verbose: %s: %s #%s done after %v", c.ID, name, id, time.Since(sent).Round(time.Millisecond))
		}()
	}
	if err != nil {
		c.detach(s, err)
		return "", err
	}
	select {
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		name := line
		if f := strings.Fields(line); len(f) > 0 {
			name = f[0]
		}
		return "", fmt.Errorf("%s: no reply from the board", name)
	case r, ok := <-ch:
		if !ok {
			return "", proto.ErrSession
		}
		if r == "ok" || strings.HasPrefix(r, "ok ") {
			return strings.TrimPrefix(strings.TrimPrefix(r, "ok"), " "), nil
		}
		return "", errors.New(strings.TrimPrefix(r, "err "))
	}
}

// onStatus merges a status notify; the compact one only carries a few keys.
func (c *Conn) onStatus(b []byte) {
	var m map[string]any
	if json.Unmarshal(b, &m) != nil {
		return
	}
	now := time.Now()
	c.mu.Lock()
	defer c.mu.Unlock()
	up, hasUp := m["up"].(float64)
	rebooted := hasUp && uint32(up) < c.boardUp
	// The first status of a session, or one after a restart, starts over: nothing of the last session (the old
	// firmware version after an update, "mirroring off" from before) is merged into it as if it were current.
	if c.statusAt.Before(c.linked) || rebooted {
		c.status = map[string]any{}
	}
	for k, v := range m {
		if sub, ok := v.(map[string]any); ok {
			if old, ok := c.status[k].(map[string]any); ok {
				for sk, sv := range sub {
					old[sk] = sv
				}
				continue
			}
		}
		c.status[k] = v
	}
	if _, full := m["fw"]; full { // both status forms carry fw and, on an abs build, abs: gone means relative
		if _, abs := m["abs"]; !abs {
			delete(c.status, "abs")
		}
	}
	c.statusAt = now
	if hasUp {
		if rebooted { // its clock started over, and the shown frame is of the last boot
			c.offset, c.frameTS = nil, 0
		}
		c.boardUp = uint32(up)
		c.learnClock(now, uint32(up))
	}
}

// learnClock must hold mu. The offset is the smallest seen: the status's `up`, or a live AU's stamp (a replayed AU
// is old, its offset large). A better one places the shown frame again.
func (c *Conn) learnClock(now time.Time, boardMs uint32) {
	off := now.UnixMilli() - int64(boardMs)
	if c.offset == nil || off < *c.offset {
		c.offset = &off
		if c.frameTS != 0 {
			c.captured = time.UnixMilli(int64(c.frameTS) + off)
		}
	}
}

func (c *Conn) onVideo(chunk []byte, gap bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.videoAt = time.Now()
	if gap {
		c.re.Lost()
	}
	if len(chunk) >= 7 && chunk[2]&video.Start != 0 {
		c.learnClock(time.Now(), binary.LittleEndian.Uint32(chunk[3:]))
	}
	gaps := c.re.Gaps
	au := c.re.Feed(chunk)
	if c.re.Gaps != gaps || gap {
		c.inStep = false // the decoder waits for the next IDR
	}
	if au == nil {
		return
	}
	switch {
	case au.Flags&video.Config != 0:
		c.config = au.Data
	case au.Flags&video.IDR != 0:
		c.gop = []video.AU{*au}
	case c.gop != nil && len(c.gop) < 600:
		c.gop = append(c.gop, *au)
	}
	for ch := range c.subs {
		select {
		case ch <- *au:
		default:
		}
	}
	if c.dec == nil && c.ffmpeg != "" && au.Flags&(video.Config|video.IDR) != 0 {
		d, err := video.NewDecoder(c.ffmpeg, c.onFrame)
		if err != nil {
			log.Printf("%s: decoder: %v", c.ID, err)
			c.ffmpeg = ""
			return
		}
		c.dec = d
		if au.Flags&video.Config == 0 && c.config != nil {
			d.Write(video.AU{Flags: video.Config, Data: c.config})
		}
	}
	if c.dec != nil && !c.dec.Write(*au) {
		log.Printf("%s: decoder fell behind or stopped (%v); restarting at the next IDR", c.ID, c.dec.Err())
		c.dec.Close()
		c.dec = nil
		c.re.Lost()
		c.inStep = false
	} else if c.dec != nil {
		c.track(au)
	}
}

// track counts the AUs the decoder got since its GOP's IDR (resumable()); must hold mu.
func (c *Conn) track(au *video.AU) {
	switch {
	case au.Flags&video.Snap != 0: // a snap sends the GOP again into the same decoder: the count is off until an IDR
		c.inStep = false
	case au.Flags&video.IDR != 0:
		c.idrTS, c.inGOP, c.inStep = au.TS, 1, true
	case au.Flags&video.Config != 0:
	case c.inStep:
		c.inGOP++
	}
}

func hashImage(img image.Image) uint64 {
	h := fnv.New64a()
	if r, ok := img.(*image.RGBA); ok {
		h.Write(r.Pix)
	}
	return h.Sum64()
}

func (c *Conn) onFrame(f video.Frame) {
	now := time.Now()
	hash := hashImage(f.Img)
	c.mu.Lock()
	defer c.mu.Unlock()
	c.updated = now
	c.captured, c.frameTS, c.behind = now, f.TS, f.Behind
	if c.offset != nil && f.TS != 0 {
		c.captured = time.UnixMilli(int64(f.TS) + *c.offset)
	}
	if hash != c.frameHash || c.frame == nil {
		c.changed = now
	}
	if size := f.Img.Bounds().Size(); size != c.barsSize {
		c.barsSize, c.bars = size, [4]int{size.X / 4, size.Y / 4, size.X / 4, size.Y / 4}
	}
	c.bars = blackEdges(f.Img, c.bars)
	c.frame, c.frameHash = f.Img, hash
	for ch := range c.frameSubs {
		select {
		case ch <- f.Img:
		default:
		}
	}
	close(c.wake)
	c.wake = make(chan struct{})
}

// Status returns a copy of the merged status and its age.
func (c *Conn) Status() (map[string]any, time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := map[string]any{}
	for k, v := range c.status {
		if sub, ok := v.(map[string]any); ok { // copied: onStatus merges into it
			cp := map[string]any{}
			for sk, sv := range sub {
				cp[sk] = sv
			}
			v = cp
		}
		out[k] = v
	}
	if c.statusAt.IsZero() {
		return out, -1
	}
	return out, time.Since(c.statusAt)
}

// StatusNow waits up to timeout for a status sent on the current session and returns the merged status; fresh is
// false when none came. Right after a reconnect the merged one still holds the last session's (or a status the
// board computed before this session): "screen mirroring is off" while it streamed (2026-11-16).
func (c *Conn) StatusNow(ctx context.Context, timeout time.Duration) (st map[string]any, fresh bool) {
	deadline := time.Now().Add(timeout)
	for {
		c.mu.Lock()
		fresh = !c.statusAt.IsZero() && !c.statusAt.Before(c.linked)
		c.mu.Unlock()
		if fresh || !time.Now().Before(deadline) || sleepCtx(ctx, 50*time.Millisecond) != nil {
			st, _ = c.Status()
			return st, fresh
		}
	}
}

// current is the status the board sent on the current session; empty while none came yet (right after a reconnect
// the merged one still holds the last session's).
func (c *Conn) current() map[string]any {
	c.mu.Lock()
	fresh := !c.statusAt.IsZero() && !c.statusAt.Before(c.linked)
	c.mu.Unlock()
	if !fresh {
		return map[string]any{}
	}
	st, _ := c.Status()
	return st
}

// streaming: video arrived on the current session within the last 2 s.
func (c *Conn) streaming() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return !c.videoAt.Before(c.linked) && time.Since(c.videoAt) < 2*time.Second
}

// Abs: the board positions the pointer absolutely (an ABS_POINTER build says "abs":1 in its status).
func (c *Conn) Abs(ctx context.Context) bool {
	st, _ := c.StatusNow(ctx, 3*time.Second)
	return num(st["abs"]) == 1
}

// Screen (pointer.Screen).

func (c *Conn) Live() bool {
	return c.current()["ap"] == "streaming" && c.Frame() != nil
}

func (c *Conn) Frame() image.Image {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.frame
}

func (c *Conn) times() (captured, updated, changed time.Time, wake chan struct{}) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.captured, c.updated, c.changed, c.wake
}

// Settled waits until the screen stops changing. AirPlay keeps sending frames of a still screen, so this
// watches when the image last changed; a gap is only "quiet" once it also covers the last frame's lag.
func (c *Conn) Settled(ctx context.Context, quiet, least, most time.Duration) {
	start := time.Now()
	if sleepCtx(ctx, least) != nil {
		return
	}
	for time.Since(start) < most {
		captured, updated, changed, _ := c.times()
		lag := min(max(updated.Sub(captured), 0), time.Second)
		if time.Since(changed) >= quiet+lag {
			return
		}
		if sleepCtx(ctx, 100*time.Millisecond) != nil {
			return
		}
	}
}

// Fresh returns the first frame captured after since, nil if none in time.
func (c *Conn) Fresh(ctx context.Context, since time.Time, timeout time.Duration) image.Image {
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	for {
		captured, _, _, wake := c.times()
		if captured.After(since) {
			return c.Frame()
		}
		select {
		case <-ctx.Done():
			return nil
		case <-deadline.C:
			return nil
		case <-wake:
		}
	}
}

func sleepCtx(ctx context.Context, d time.Duration) error {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-t.C:
		return nil
	}
}

// Device (pointer.Device).

func (c *Conn) Links() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.links
}

type device struct{ c *Conn }

func (d device) Send(ctx context.Context, line string) error {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	if strings.HasPrefix(line, "key ") || strings.HasPrefix(line, "type ") {
		d.c.keyMu.Lock()
		defer d.c.keyMu.Unlock()
		if err := sleepCtx(ctx, time.Until(d.c.lastKey.Add(KeyGap))); err != nil {
			return err
		}
		defer func() { d.c.lastKey = time.Now() }()
	}
	_, err := d.c.Send(ctx, line)
	return err
}

func (d device) Links() int { return d.c.Links() }

func idHex(b []byte) string { return hex.EncodeToString(b) }
