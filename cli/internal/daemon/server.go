// Package daemon holds the sessions with the boards (over Wi-Fi): the first CLI command starts `tentacle daemon`,
// and every command reaches it over a unix socket in the config dir. It exits after 10 minutes without requests.
package daemon

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/config"
	"github.com/kewe87/phone-tentacle/cli/internal/history"
	"github.com/kewe87/phone-tentacle/cli/internal/link"
	"github.com/kewe87/phone-tentacle/cli/internal/proto"
	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

// Request is one command from the CLI.
type Request struct {
	Cmd     string    `json:"cmd"`
	Device  string    `json:"device,omitempty"`
	Pts     []float64 `json:"pts,omitempty"` // screenshot pixel coordinates
	Text    string    `json:"text,omitempty"`
	Ms      int       `json:"ms,omitempty"`
	Format  string    `json:"format,omitempty"`
	Quality int       `json:"quality,omitempty"`
	Scale   float64   `json:"scale,omitempty"`
	MaxSize int       `json:"max_size,omitempty"`
	Aim     bool      `json:"aim,omitempty"`
	Count   int       `json:"count,omitempty"`   // taps in a row at one point (2: double tap)
	Host    string    `json:"host,omitempty"`    // --host: the board's address by hand, from now on ("auto": find it again)
	Verbose bool      `json:"verbose,omitempty"` // --verbose: trace discovery, frames and timings, also to this client
	Force   bool      `json:"force,omitempty"`   // update: send an image without a version marker
	WaitMs  int       `json:"wait_ms,omitempty"` // screenshot: wait for a picture newer than the last input (0: 3 s, <0: none)
}

// Response is the CLI's JSON output, as mobilecli's: {"status":"ok","data":…} or {"status":"error","error":"…"}.
// Before it the daemon may send lines with status "progress" (pairing: the code to compare; --verbose: the trace).
type Response struct {
	Status string `json:"status"`
	Data   any    `json:"data,omitempty"`
	Error  string `json:"error,omitempty"`
}

func ok(data any) Response    { return Response{Status: "ok", Data: data} }
func fail(err error) Response { return Response{Status: "error", Error: err.Error()} }

type Server struct {
	Transport link.Transport
	Config    *config.Config
	Dir       string
	Version   string
	Idle      time.Duration // exit after this long without requests (0 = never)
	ScanTime  time.Duration

	mu     sync.Mutex
	conns  map[string]*Conn
	last   time.Time
	active int
	linkMu sync.Mutex // one scan or connect at a time
	// After the board could not be found or reached, the next tries wait: 1 s, 2 s ... 15 s (a loop of requests
	// then does not scan the network over and over).
	failAt  time.Time
	failGap time.Duration
	failErr error
	host    string // the last --host
	stop    chan struct{}
	once    sync.Once
	hist    *history.Recorder
}

func (s *Server) Serve(l net.Listener) error {
	s.conns, s.last, s.stop = map[string]*Conn{}, time.Now(), make(chan struct{})
	if s.ScanTime == 0 {
		s.ScanTime = 4 * time.Second
	}
	go func() {
		<-s.stop
		l.Close()
	}()
	if s.Idle > 0 {
		go s.idle()
	}
	for {
		nc, err := l.Accept()
		if err != nil {
			select {
			case <-s.stop:
				s.closeAll()
				return nil
			default:
				return err
			}
		}
		go s.handle(nc)
	}
}

func (s *Server) Stop() { s.once.Do(func() { close(s.stop) }) }

func (s *Server) closeAll() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.conns {
		c.close()
	}
}

func (s *Server) idle() {
	for {
		select {
		case <-s.stop:
			return
		case <-time.After(10 * time.Second):
		}
		s.mu.Lock()
		quiet := s.active == 0 && time.Since(s.last) > s.Idle
		s.mu.Unlock()
		if quiet {
			log.Printf("idle for %v, exiting", s.Idle)
			s.Stop()
			return
		}
	}
}

func (s *Server) touch(delta int) {
	s.mu.Lock()
	s.last, s.active = time.Now(), s.active+delta
	s.mu.Unlock()
}

func (s *Server) handle(nc net.Conn) {
	defer nc.Close()
	s.touch(1)
	defer s.touch(-1)
	var req Request
	if err := json.NewDecoder(bufio.NewReader(nc)).Decode(&req); err != nil {
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() { // the CLI went away (Ctrl-C): stop what it asked for
		io.Copy(io.Discard, nc)
		cancel()
	}()
	if req.Host != "" {
		s.setHost(req.Host)
	}
	if req.Verbose {
		verbose.Store(true) // until the daemon exits
	}
	if req.Cmd == "screencapture" {
		s.screencapture(ctx, req, nc)
		return
	}
	if req.Cmd == "logfollow" {
		s.logFollow(ctx, req, nc)
		return
	}
	var wmu sync.Mutex // progress lines and the response share the connection
	progress := func(data any) {
		wmu.Lock()
		defer wmu.Unlock()
		json.NewEncoder(nc).Encode(Response{Status: "progress", Data: data})
	}
	ctx = context.WithValue(ctx, progressKey{}, progress)
	if verbose.Load() {
		ctx = proto.WithLog(ctx, func(format string, args ...any) {
			line := fmt.Sprintf(format, args...)
			log.Print("verbose: " + line)
			if req.Verbose {
				progress(map[string]string{"log": line})
			}
		})
	}
	started := time.Now()
	resp := s.safeDo(ctx, req)
	if Debug || verbose.Load() { // command names and outcomes only: no typed text, screenshots or keys
		log.Printf("debug: %s device=%q -> %s %s (%s)", req.Cmd, req.Device, resp.Status, resp.Error, time.Since(started).Round(time.Millisecond))
	}
	wmu.Lock()
	json.NewEncoder(nc).Encode(resp)
	wmu.Unlock()
	if req.Cmd == "stop" {
		s.Stop()
	}
}

type progressKey struct{}

// progress sends a line ahead of the response to the client of this request, if it has one.
func progress(ctx context.Context, data any) {
	if f, ok := ctx.Value(progressKey{}).(func(any)); ok {
		f(data)
	}
}

// setHost passes --host on to a transport that takes one.
func (s *Server) setHost(h string) {
	if h == "auto" {
		h = ""
	} else if h == "" {
		return
	}
	s.mu.Lock()
	same := s.host == h
	s.host = h
	if !same { // another address: try at once
		s.failAt, s.failGap, s.failErr = time.Time{}, 0, nil
	}
	s.mu.Unlock()
	if t, ok := s.Transport.(interface{ SetHost(string) }); ok && !same {
		t.SetHost(h)
	}
}

// safeDo runs Do but turns a panic in any handler into an error response, so one malformed
// request can't crash the whole daemon (and every other board session with it).
func (s *Server) safeDo(ctx context.Context, req Request) (resp Response) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("recovered from panic handling %q: %v", req.Cmd, r)
			resp = fail(fmt.Errorf("internal error handling %q", req.Cmd))
		}
	}()
	return s.Do(ctx, req)
}

// Do runs one request.
func (s *Server) Do(ctx context.Context, req Request) Response {
	switch req.Cmd {
	case "version":
		return ok(map[string]string{"version": s.Version})
	case "stop":
		return ok(map[string]string{"message": "daemon stopping"})
	case "devices":
		return s.devices(ctx)
	case "pair":
		return s.pair(ctx, req.Device)
	case "unpair":
		return s.unpair(ctx, req.Device, req.Text == "board")
	}
	c, err := s.conn(ctx, req.Device)
	if err != nil {
		return fail(err)
	}
	switch req.Cmd {
	case "info":
		st, age := c.Status()
		st["id"], st["name"] = c.ID, c.Name
		l := s.Config.GetListen()
		st["listen"] = map[string]any{"on": l.On}
		if age >= 0 {
			st["age_ms"] = age.Milliseconds()
		}
		return ok(st)
	case "screenshot":
		return s.screenshot(ctx, c, req)
	case "calibrate":
		return s.calibrate(ctx, c)
	case "tap", "longpress", "swipe", "text", "keys", "button":
		return s.io(ctx, c, req)
	case "send": // debugging: one raw board command; afterwards the pointer position is unknown
		reply, err := c.Send(ctx, req.Text)
		c.pointer(s.Dir).Forget()
		if err != nil {
			return fail(err)
		}
		return ok(map[string]string{"reply": reply})
	case "boardlog":
		return s.boardLog(ctx, c)
	case "net":
		return s.net(ctx, c, req.Text)
	case "listen":
		return s.listen(ctx, c, req.Text)
	case "update":
		return s.update(ctx, c, req)
	case "debug":
		return s.debug(ctx, c, req.Text)
	case "web":
		return s.web(ctx, c, req.Text)
	case "keyboard":
		return s.keyboard(ctx, c, req.Text)
	case "video":
		return s.video(ctx, c, req.Text)
	}
	return fail(fmt.Errorf("unknown command %q", req.Cmd))
}

type deviceInfo struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Platform  string `json:"platform"`
	Type      string `json:"type"`
	State     string `json:"state"`
	Paired    bool   `json:"paired"`
	Connected bool   `json:"connected"`
	Addr      string `json:"addr,omitempty"`    // where it answers on the network
	FW        string `json:"fw,omitempty"`      // firmware version
	Source    string `json:"source,omitempty"`  // mdns, remembered, host, probe
	Pairing   string `json:"pairing,omitempty"` // open, closed: whether the board takes a pairing request now
}

func (s *Server) devices(ctx context.Context) Response {
	list := []deviceInfo{}
	seen, seenAddr := map[string]bool{}, map[string]bool{}
	s.mu.Lock()
	for _, c := range s.conns {
		if c.connected() {
			list = append(list, deviceInfo{ID: c.ID, Name: c.Name, Platform: "ios", Type: "tentacle", State: "online", Paired: true,
				Connected: true, Addr: c.addr})
			seen[c.ID[8:]] = true
			if c.addr != "" {
				seenAddr[c.addr] = true
			}
		}
	}
	s.mu.Unlock()
	found, err := s.scan(ctx)
	if err != nil {
		return fail(err)
	}
	for _, f := range found {
		suf := f.Suffix()
		if seen[suf] && suf != "" || suf == "" && seenAddr[f.Addr] { // the connected one, found again
			continue
		}
		d := deviceInfo{ID: suf, Name: f.Name, Platform: "ios", Type: "tentacle", State: "online", Addr: f.Addr, FW: f.FW, Source: f.Source, Pairing: f.Open}
		if f.ID != "" {
			d.ID = f.ID
		}
		if ids := s.Config.Match(suf); suf != "" && len(ids) == 1 {
			d.ID, d.Paired = ids[0], true
		}
		list = append(list, d)
	}
	return ok(map[string]any{"devices": list})
}

func (s *Server) scan(ctx context.Context) ([]link.Found, error) {
	s.linkMu.Lock()
	defer s.linkMu.Unlock()
	return s.Transport.Scan(ctx, s.ScanTime)
}

// matches: a selector is a full board id or its last 4 hex digits; a board whose name was not seen matches any.
func matches(f link.Found, sel string) bool {
	if sel == "" || f.Suffix() == "" {
		return true
	}
	sel = strings.ToLower(sel)
	return f.Suffix() == sel || len(sel) == 12 && sel[8:] == f.Suffix()
}

// pick chooses the board to connect to: the one matching sel, else the only one, else the only paired one.
func (s *Server) pick(ctx context.Context, sel string, wantPaired bool) (link.Found, error) {
	found, err := s.scan(ctx)
	if err != nil {
		return link.Found{}, err
	}
	var cand, paired []link.Found
	for _, f := range found {
		if matches(f, sel) {
			cand = append(cand, f)
			if f.Suffix() != "" && len(s.Config.Match(f.Suffix())) > 0 {
				paired = append(paired, f)
			}
		}
	}
	switch {
	case len(cand) == 0 && sel != "":
		return link.Found{}, fmt.Errorf("board %s %w", sel, errNotFound)
	case len(cand) == 0:
		return link.Found{}, fmt.Errorf("board %w", errNotFound)
	case len(cand) == 1:
		return cand[0], nil
	case wantPaired && len(paired) == 1:
		return paired[0], nil
	}
	var names []string
	for _, f := range cand {
		if n := f.Suffix(); n != "" {
			names = append(names, n)
		} else {
			names = append(names, f.Addr)
		}
	}
	return link.Found{}, fmt.Errorf("several boards found (%s): pass --device <id> or set TENTACLE_DEVICE", strings.Join(names, ", "))
}

// errNotFound: nothing answered. The text says what to check, in the order it usually is.
var errNotFound = errors.New("not found on the network: is it powered (plugged into the iPhone) and on the same Wi-Fi as this " +
	"computer? A new board first needs its Wi-Fi set up (join its Tentacle-XXXX hotspot). If it is on the Wi-Fi but not " +
	"found, give its address: --host <ip> (it is on the board's display, and in tentacle devices --verbose)")

func explain(err error) error {
	var be proto.BoardError
	if errors.As(err, &be) && be == "not paired" {
		return errors.New("not paired with this board: run tentacle pair, then press the BOOT button on the board when it asks")
	}
	return err
}

const (
	reachBackoffMin = time.Second
	reachBackoffMax = 15 * time.Second
)

// noteReach keeps how the last try to find and reach a board ended.
func (s *Server) noteReach(err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var nr *link.NotReachable
	if err == nil || !(errors.Is(err, errNotFound) || errors.As(err, &nr)) {
		s.failAt, s.failGap, s.failErr = time.Time{}, 0, nil
		return
	}
	s.failGap = min(max(s.failGap*2, reachBackoffMin), reachBackoffMax)
	s.failAt, s.failErr = time.Now().Add(s.failGap), err
}

// conn returns the connected board for sel, connecting (scan + HELLO) if needed.
func (s *Server) conn(ctx context.Context, sel string) (*Conn, error) {
	var live []*Conn
	s.mu.Lock()
	for id, c := range s.conns {
		if c.connected() && (sel == "" || strings.EqualFold(id, sel) || len(sel) == 4 && strings.EqualFold(id[8:], sel)) {
			live = append(live, c)
		}
	}
	s.mu.Unlock()
	if len(live) == 1 {
		return live[0], nil
	}
	s.mu.Lock()
	wait, last := time.Until(s.failAt), s.failErr
	s.mu.Unlock()
	if last != nil && wait > 0 {
		return nil, fmt.Errorf("%w (next try in %d s)", last, int(wait/time.Second)+1)
	}
	f, err := s.pick(ctx, sel, true)
	var c *Conn
	if err == nil {
		c, err = s.open(ctx, f, sel, nil)
	}
	s.noteReach(err)
	return c, err
}

// open connects to f and starts a session. l: an already open link (after PAIR).
func (s *Server) open(ctx context.Context, f link.Found, sel string, l proto.Link) (*Conn, error) {
	s.linkMu.Lock()
	defer s.linkMu.Unlock()
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	var err error
	if l == nil {
		if l, err = s.Transport.Connect(ctx, f); err != nil {
			return nil, err
		}
	}
	sess, err := proto.Hello(ctx, l, s.Config.ClientIDBytes(), s.Config.Key)
	if err != nil {
		l.Close()
		return nil, explain(err)
	}
	id := idHex(sess.BoardID)
	if sel != "" && !strings.EqualFold(id, sel) && !(len(sel) == 4 && strings.EqualFold(id[8:], sel)) {
		sess.Close()
		return nil, fmt.Errorf("board %s not found (found %s)", sel, id)
	}
	if r, ok := s.Transport.(interface{ Remember(string, link.Found) }); ok {
		r.Remember(id, f) // where it is, for when mDNS finds nothing
	}
	name := s.boardName(f, id)
	s.mu.Lock()
	c := s.conns[id]
	if c == nil {
		ff, _ := video.FFmpeg()
		c = newConn(id, name, ff)
		s.hearingFor(c)
		s.conns[id] = c
	}
	c.addr = f.Addr
	s.mu.Unlock()
	c.attach(sess)
	if err := c.start(); err != nil {
		c.detach(sess, err)
		return nil, fmt.Errorf("board %s did not answer: %w", id, err)
	}
	log.Printf("%s: connected (%s, %s)", id, name, f.Addr)
	return c, nil
}

// boardName is the board's name: as it announced it, or, found by address only, the stored one or the default.
func (s *Server) boardName(f link.Found, id string) string {
	if f.Name != "" && f.Source != "host" && f.Source != "probe" {
		return f.Name
	}
	if b, ok := s.Config.Boards[id]; ok && b.Name != "" {
		return b.Name
	}
	return "Tentacle " + strings.ToUpper(id[8:])
}

// hostName is this computer's name for the board's display.
func hostName() string {
	h, err := os.Hostname()
	if err != nil || h == "" {
		return "computer"
	}
	return h
}

// How long pair keeps asking a board that says it is in use (a session of ours, just closed, counts for 10 s),
// and how long it waits between two tries.
const pairBusyWait = 20 * time.Second

var pairBusyRetry = 3 * time.Second

// The board takes a pairing request only for 60 s after a double press of its BOOT button. A closed board's
// refusal ends pair at once with what to do: asking again for a minute only kept the person waiting (2026-11-16).

// pair pairs this computer with a board: over the network, confirmed on the board with its BOOT button. The code
// goes to the client as a progress line before the board waits; the response comes after the button (or its timeout).
func (s *Server) pair(ctx context.Context, sel string) Response {
	// The board serves one link: sessions of ours would be in the way (and it refuses to pair while one is up).
	s.mu.Lock()
	for id, c := range s.conns {
		if sel == "" || strings.EqualFold(id, sel) || len(sel) == 4 && strings.EqualFold(id[8:], sel) {
			c.close()
			delete(s.conns, id)
		}
	}
	s.mu.Unlock()
	f, err := s.pick(ctx, sel, false)
	if err != nil {
		return fail(err)
	}
	var l proto.Link
	var k, boardID []byte
	started := time.Now()
	for {
		cctx, cancel := context.WithTimeout(ctx, 10*time.Second)
		s.linkMu.Lock()
		l, err = s.Transport.Connect(cctx, f)
		s.linkMu.Unlock()
		cancel()
		if err != nil {
			return fail(err)
		}
		k, boardID, err = proto.Pair(ctx, l, s.Config.ClientIDBytes(), hostName(), func(code string, wait time.Duration) {
			progress(ctx, map[string]any{"pair": "confirm", "code": code, "name": f.Name, "wait_s": int(wait / time.Second)})
		})
		if err == nil {
			break
		}
		l.Close()
		var be proto.BoardError
		if errors.As(err, &be) && strings.HasPrefix(string(be), "busy: board in use") && time.Since(started) < pairBusyWait {
			progress(ctx, map[string]any{"pair": "waiting", "message": "the board was in use a moment ago; asking again"})
			if sleepCtx(ctx, pairBusyRetry) != nil {
				return fail(ctx.Err())
			}
			continue
		}
		return fail(explainPair(err))
	}
	if err := s.Config.SetKey(boardID, k, s.boardName(f, idHex(boardID))); err != nil {
		l.Close()
		return fail(err)
	}
	s.noteReach(nil)
	c, err := s.open(ctx, f, "", l)
	if err != nil {
		return fail(fmt.Errorf("paired, but the session failed: %w", err))
	}
	return ok(map[string]any{"id": c.ID, "name": c.Name, "message": "paired"})
}

// explainPair says what to do about the board's answer to a pairing request.
func explainPair(err error) error {
	var be proto.BoardError
	if !errors.As(err, &be) {
		if errors.Is(err, proto.ErrSession) {
			return errors.New("the board closed the link while pairing (Wi-Fi dropped, or the board restarted): try again")
		}
		return err
	}
	switch msg := string(be); {
	case strings.HasPrefix(msg, "pairing closed"):
		return errors.New("not paired: pairing was not opened at the board. Double-press its BOOT button (two quick presses), " +
			"then run tentacle pair within 60 s. While a paired computer drives the phone a double press opens nothing: end that session first")
	case strings.HasPrefix(msg, "pairing not confirmed"):
		return errors.New("not paired: the BOOT button on the board was not pressed in time. Nothing was stored; run tentacle pair again")
	case strings.HasPrefix(msg, "busy: pairing locked"):
		return errors.New("the board refuses pairing for a moment (" + strings.TrimPrefix(msg, "busy: ") + "): wait, then run tentacle pair again")
	case strings.HasPrefix(msg, "busy: board in use"):
		return errors.New("the board is in use by a paired computer: end that session (tentacle daemon stop there), then pair again")
	}
	return err
}

func (s *Server) unpair(ctx context.Context, sel string, board bool) Response {
	var ids []string
	if sel == "" {
		for id := range s.Config.Boards {
			ids = append(ids, id)
		}
	} else {
		ids = s.Config.Match(sel)
	}
	if len(ids) != 1 {
		return fail(fmt.Errorf("unpair: %d paired boards match; pass --device <id>", len(ids)))
	}
	if board { // while the key is still here: the board forgets every paired computer, then drops the link
		c, err := s.conn(ctx, ids[0])
		if err == nil {
			_, err = c.Send(ctx, "unpair")
		}
		if err != nil {
			return fail(fmt.Errorf("the board was not told to unpair (%w); nothing was changed", err))
		}
	}
	s.mu.Lock()
	if c := s.conns[ids[0]]; c != nil {
		c.close()
		delete(s.conns, ids[0])
	}
	s.mu.Unlock()
	if err := s.Config.Forget(ids[0]); err != nil {
		return fail(err)
	}
	if t, ok := s.Transport.(interface{ Forget(string) }); ok {
		t.Forget(ids[0])
	}
	msg := "forgotten on this computer. The board keeps this computer's key (it holds up to 4, the oldest is replaced); " +
		"tentacle unpair --board makes the board forget every paired computer too"
	if board {
		msg = "forgotten on this computer and on the board (every paired computer): pair again with tentacle pair"
	}
	return ok(map[string]string{"id": ids[0], "message": msg})
}

// Listen opens the daemon socket (AF_UNIX, also on Windows 10+).
func Listen(dir string) (net.Listener, error) {
	path := SocketPath(dir)
	if c, err := net.DialTimeout("unix", path, time.Second); err == nil {
		c.Close()
		return nil, errors.New("a daemon is already running")
	}
	os.Remove(path)
	return net.Listen("unix", path)
}
