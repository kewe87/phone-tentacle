package daemon

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
	"github.com/kewe87/phone-tentacle/cli/internal/proto"
)

// Firmware update over the session (docs/cli-protocol.md, "Firmware update"): `update begin` announces the image
// (size, SHA-256, signature, version), the image follows in 'u' records on stream C, `update end` has the board
// check it and restart into it. Then the board is found again and asked which firmware it runs.

// UpdateChunk is the image bytes per 'u' record (the board's OTA_CHUNK_MAX, a flash sector).
const UpdateChunk = 4096

// How often the transfer asks the board how far it is (and whether it gave up): every this many bytes.
const updateCheckEvery = 256 << 10

// How long to wait for the board after its restart.
var updateBackWait = 90 * time.Second

func (s *Server) update(ctx context.Context, c *Conn, req Request) Response {
	path := req.Text
	img, err := os.ReadFile(path)
	if err != nil {
		return fail(err)
	}
	if err := fwsign.CheckImage(img); err != nil {
		return fail(fmt.Errorf("%s: %w", path, err))
	}
	newVer := fwsign.Version(img)
	if newVer == "" && !req.Force {
		return fail(fmt.Errorf("%s is not a Tentacle firmware image (no version marker); --force sends it anyway", path))
	}
	sig, err := fwsign.ReadSig(path)
	if err != nil {
		return fail(err)
	}
	st, _ := c.Status()
	oldVer, _ := st["fw"].(string)
	sum := sha256.Sum256(img)
	sigText := "-"
	if sig != nil {
		sigText = hex.EncodeToString(sig)
	}
	line := fmt.Sprintf("update begin %d %s %s", len(img), hex.EncodeToString(sum[:]), sigText)
	if newVer != "" {
		line += " " + newVer
	}
	sctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	reply, err := c.Send(sctx, line)
	cancel()
	if err != nil {
		if strings.HasPrefix(err.Error(), "unknown command") {
			return fail(errors.New("this board's firmware cannot update over Wi-Fi: flash it once over USB"))
		}
		return fail(fmt.Errorf("the board refused the update: %w", err))
	}
	if reply != "ready" {
		return fail(fmt.Errorf("unexpected answer to update begin: %q", reply))
	}
	sess := c.session()
	if sess == nil {
		return fail(proto.ErrSession)
	}
	log.Printf("%s: update %s -> %s, %d bytes, %s", c.ID, oldVer, newVer, len(img), map[bool]string{true: "signed", false: "unsigned"}[sig != nil])
	progress(ctx, map[string]any{"update": "send", "done": 0, "total": len(img), "from": oldVer, "to": newVer, "signed": sig != nil})
	started := time.Now()
	abort := func(why error) Response {
		actx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		c.Send(actx, "update abort")
		cancel()
		return fail(why)
	}
	lastShown := 0
	for off := 0; off < len(img); off += UpdateChunk {
		if ctx.Err() != nil {
			return abort(errors.New("update cancelled; the board keeps its firmware"))
		}
		end := min(off+UpdateChunk, len(img))
		if err := c.upload(sess, uint32(off), img[off:end]); err != nil {
			return fail(fmt.Errorf("the link dropped during the update (the board keeps its firmware): %w", err))
		}
		if end-lastShown >= updateCheckEvery || end == len(img) {
			lastShown = end
			sctx, cancel := context.WithTimeout(ctx, 20*time.Second)
			r, err := c.Send(sctx, "update state")
			cancel()
			if err != nil {
				return abort(fmt.Errorf("the board stopped the update: %w", err))
			}
			if !strings.HasPrefix(r, "ready") {
				return abort(fmt.Errorf("the board stopped the update (%s)", r))
			}
			progress(ctx, map[string]any{"update": "send", "done": end, "total": len(img)})
		}
	}
	sent := time.Since(started)
	sctx, cancel = context.WithTimeout(ctx, 60*time.Second)
	reply, err = c.Send(sctx, "update end")
	cancel()
	if err != nil {
		return fail(fmt.Errorf("the board did not take the image (it keeps its firmware): %w", err))
	}
	log.Printf("%s: update sent in %v, board: %s", c.ID, sent.Round(time.Millisecond), reply)
	progress(ctx, map[string]any{"update": "restart", "message": "the board restarts; waiting for it to come back"})
	links := c.Links()
	restarted := time.Now()
	c.close() // its link is going anyway
	fw, err := s.waitBack(ctx, req.Device, c, links, restarted)
	out := map[string]any{"from": oldVer, "to": newVer, "bytes": len(img), "send_s": sent.Seconds(), "signed": sig != nil}
	if err != nil {
		out["message"] = "the image was sent and verified, but the board was not found again: " + err.Error()
		return Response{Status: "error", Data: out, Error: out["message"].(string)}
	}
	out["fw"] = fw
	out["hint"] = mirroringHint
	switch {
	case newVer == "" || fw == newVer:
		out["message"] = fmt.Sprintf("updated: %s -> %s. %s", oldVer, fw, mirroringHint)
		return ok(out)
	case fw == oldVer:
		return Response{Status: "error", Data: out, Error: fmt.Sprintf("the board came back with %s: the new firmware did not start properly and the board went back to the old one", fw)}
	}
	return Response{Status: "error", Data: out, Error: fmt.Sprintf("the board came back with %s, expected %s", fw, newVer)}
}

// mirroringHint: a restarted board has no mirroring session, and the iPhone does not start one again by itself.
const mirroringHint = "The board restarted: restart Screen Mirroring on the iPhone (Control Center > Screen Mirroring > " +
	"Tentacle; if it still shows as connected, Stop Mirroring first), else screenshots fail or show an old picture"

// waitBack finds the board again after its restart and returns the firmware version its first status names.
func (s *Server) waitBack(ctx context.Context, sel string, old *Conn, links int, since time.Time) (string, error) {
	if sel == "" {
		sel = old.ID
	}
	deadline := time.Now().Add(updateBackWait)
	var last error = errors.New("not found yet")
	for time.Now().Before(deadline) {
		if sleepCtx(ctx, 2*time.Second) != nil {
			return "", ctx.Err()
		}
		s.mu.Lock()
		s.failAt, s.failGap, s.failErr = time.Time{}, 0, nil // no backoff: it is expected back any moment
		s.mu.Unlock()
		c, err := s.conn(ctx, sel)
		if err != nil {
			last = err
			continue
		}
		if c.Links() <= links {
			continue
		}
		for t := time.Now(); time.Since(t) < 5*time.Second; {
			st, age := c.Status()
			if fw, ok := st["fw"].(string); ok && age >= 0 && time.Now().Add(-age).After(since) {
				return fw, nil
			}
			if sleepCtx(ctx, 200*time.Millisecond) != nil {
				return "", ctx.Err()
			}
		}
	}
	return "", last
}

// debug runs one of the board's `dbg` commands (debug builds only) and returns all it printed.
func (s *Server) debug(ctx context.Context, c *Conn, args string) Response {
	args = strings.TrimSpace(args)
	if args == "" {
		args = "state"
	}
	if !strings.HasPrefix(args, "dbg") {
		args = "dbg " + args
	}
	reply, err := c.Send(ctx, args)
	if err != nil {
		return fail(err)
	}
	var b strings.Builder
	for page := 0; page < 100; page++ {
		next, end, text, perr := parseLogPage(reply)
		if perr != nil {
			return fail(perr)
		}
		b.WriteString(text)
		if next >= end {
			break
		}
		if reply, err = c.Send(ctx, "dbgout "+strconv.Itoa(next)); err != nil {
			return fail(err)
		}
	}
	return ok(map[string]string{"reply": b.String()})
}

// logFollow streams the board's new log lines (debug builds: `logs follow on`) to the client until it goes away.
// A session that drops (the board restarts, say) is opened again and asked again.
func (s *Server) logFollow(ctx context.Context, req Request, nc net.Conn) {
	enc := json.NewEncoder(nc)
	c, err := s.conn(ctx, req.Device)
	if err != nil {
		enc.Encode(fail(err))
		return
	}
	ch := make(chan string, 256)
	c.mu.Lock()
	c.logSubs[ch] = true
	c.mu.Unlock()
	defer func() {
		c.mu.Lock()
		delete(c.logSubs, ch)
		last := len(c.logSubs) == 0
		c.mu.Unlock()
		if last && c.connected() {
			octx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			c.Send(octx, "logs follow off")
			cancel()
		}
	}()
	if _, err := c.Send(ctx, "logs follow on"); err != nil {
		enc.Encode(fail(err))
		return
	}
	enc.Encode(ok(map[string]any{"following": c.ID}))
	tick := time.NewTicker(2 * time.Second)
	defer tick.Stop()
	lost := false
	for {
		select {
		case <-ctx.Done():
			return
		case text := <-ch:
			if _, err := nc.Write([]byte(text)); err != nil {
				return
			}
		case <-tick.C:
			if c.connected() {
				continue
			}
			if !lost {
				lost = true
				nc.Write([]byte("-- tentacle: link to the board lost, reconnecting --\n"))
			}
			s.mu.Lock()
			s.failAt, s.failGap, s.failErr = time.Time{}, 0, nil
			s.mu.Unlock()
			if c2, err := s.conn(ctx, req.Device); err == nil && c2 == c { // start() asked for the log again
				lost = false
				nc.Write([]byte("-- tentacle: reconnected --\n"))
			}
		}
	}
}

// onLog hands log text from the board to the followers.
func (c *Conn) onLog(b []byte) {
	text := unescapeLog(string(b))
	c.mu.Lock()
	defer c.mu.Unlock()
	for ch := range c.logSubs {
		select {
		case ch <- text:
		default:
		}
	}
}

// session is the current session (nil: none).
func (c *Conn) session() *proto.Session {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sess
}

// upload sends one piece of a firmware image on want, the session the update began on: on another one (a
// reconnect) the board has dropped the update already.
func (c *Conn) upload(want *proto.Session, off uint32, data []byte) error {
	c.swap.RLock()
	defer c.swap.RUnlock()
	if c.session() != want {
		return proto.ErrSession
	}
	if err := want.SendUpload(off, data); err != nil {
		c.detach(want, err)
		return err
	}
	return nil
}
