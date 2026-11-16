// Package flashui is tentacle-flash's local web page: a three-step installer (plug in, install, done with the
// setup Wi-Fi) with an "Advanced" part (port, own image, erase, chip info, log). It listens on 127.0.0.1 only;
// every API call carries the random token from the page's address, so other web pages cannot drive it.
package flashui

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
	"github.com/kewe87/phone-tentacle/cli/internal/flash"
)

//go:embed web
var webFS embed.FS

// Server is the installer page and its API.
type Server struct {
	Sys     flash.System
	Version string
	// Default is where the image comes from until one is uploaded (normally flash.Embedded or a --image file).
	Default flash.Source
	// Port and Erase preset the advanced options (from the command line).
	Port  string
	Erase bool
	// Idle ends Serve when the page has not asked for news that long (0 = never).
	Idle time.Duration

	token string
	addr  string
	quit  chan struct{}
	once  sync.Once

	mu       sync.Mutex
	image    *flash.Image
	imageErr error
	upload   *flash.Bytes // the last uploaded .bin (its signature inside)
	job      *job
	ports    []flash.PortInfo
	portsAt  time.Time
	seen     time.Time
}

type job struct {
	Kind    string    `json:"kind"` // "install" or "chip"
	Running bool      `json:"running"`
	Stage   string    `json:"stage"`
	Done    int       `json:"done"`
	Total   int       `json:"total"`
	Log     []string  `json:"log"`
	Error   *text     `json:"error,omitempty"`
	Result  *result   `json:"result,omitempty"`
	Chip    *chipJSON `json:"chip,omitempty"`
	Notes   []text    `json:"notes,omitempty"`
	started time.Time
}

type text struct {
	Code string `json:"code"`
	EN   string `json:"en"`
	DE   string `json:"de"`
}

func textOf(err error) *text {
	var m *flash.Msg
	if errors.As(err, &m) {
		return &text{Code: m.Code, EN: m.Text("en"), DE: m.Text("de")}
	}
	return &text{Code: "error", EN: err.Error(), DE: err.Error()}
}

func texts(ms []*flash.Msg) []text {
	out := []text{}
	for _, m := range ms {
		out = append(out, *textOf(m))
	}
	return out
}

type chipJSON struct {
	Name     string `json:"name"`
	Revision string `json:"revision"`
	MAC      string `json:"mac"`
	PSRAM    string `json:"psram"`
	Flash    string `json:"flash"`
	FlashID  string `json:"flashId"`
	Embedded string `json:"embeddedFlash,omitempty"`
	USBJTAG  bool   `json:"usbJtag"`
}

func chipOf(c *esprom.ChipInfo) *chipJSON {
	if c == nil {
		return nil
	}
	j := &chipJSON{Name: c.Name, Revision: c.Revision, PSRAM: c.PSRAM, Flash: c.FlashSizeText(),
		FlashID: fmt.Sprintf("%06x", c.FlashID), Embedded: c.EmbeddedFlash, USBJTAG: c.USBJTAG}
	if c.MAC != nil {
		j.MAC = c.MAC.String()
	}
	return j
}

type result struct {
	Port  string           `json:"port"`
	Setup *flash.SetupInfo `json:"setup,omitempty"`
}

// Listen opens a random port on 127.0.0.1 and returns the page's address (with the token).
func (s *Server) Listen() (net.Listener, string, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, "", err
	}
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	s.token = hex.EncodeToString(b)
	s.addr = ln.Addr().String()
	s.quit = make(chan struct{})
	return ln, "http://" + s.addr + "/?t=" + s.token, nil
}

// Serve runs until the page says quit, the page has been gone for Idle, or ctx ends.
func (s *Server) Serve(ctx context.Context, ln net.Listener) error {
	s.loadDefault()
	srv := &http.Server{Handler: s.Handler(), ReadHeaderTimeout: 10 * time.Second}
	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(ln) }()
	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			_ = srv.Close()
			return ctx.Err()
		case err := <-errc:
			return err
		case <-s.quit:
			time.Sleep(200 * time.Millisecond) // let the answer to /api/quit go out
			_ = srv.Close()
			return nil
		case <-tick.C:
			s.mu.Lock()
			idle := s.Idle > 0 && !s.seen.IsZero() && time.Since(s.seen) > s.Idle && (s.job == nil || !s.job.Running)
			s.mu.Unlock()
			if idle {
				_ = srv.Close()
				return nil
			}
		}
	}
}

func (s *Server) loadDefault() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.Default == nil {
		s.image, s.imageErr = nil, flash.ErrNoImage
		return
	}
	s.image, s.imageErr = flash.Open(context.Background(), s.Default)
}

// Handler serves the page and the API.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	static, _ := fs.Sub(webFS, "web")
	mux.Handle("GET /", http.FileServerFS(static))
	mux.HandleFunc("GET /api/state", s.api(s.state))
	mux.HandleFunc("POST /api/install", s.api(s.install))
	mux.HandleFunc("POST /api/chip", s.api(s.chip))
	mux.HandleFunc("POST /api/image", s.api(s.uploadImage))
	mux.HandleFunc("POST /api/image/reset", s.api(s.resetImage))
	mux.HandleFunc("POST /api/quit", s.api(func(w http.ResponseWriter, r *http.Request) any {
		s.once.Do(func() { close(s.quit) })
		return map[string]bool{"ok": true}
	}))
	return s.guard(mux)
}

// guard accepts only requests addressed to 127.0.0.1:port (no DNS rebinding) and, for the API, with the token.
func (s *Server) guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.addr != "" && r.Host != s.addr && r.Host != strings.Replace(s.addr, "127.0.0.1", "localhost", 1) {
			http.Error(w, "wrong host", http.StatusForbidden)
			return
		}
		if strings.HasPrefix(r.URL.Path, "/api/") &&
			subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Token")), []byte(s.token)) != 1 {
			http.Error(w, "missing token", http.StatusForbidden)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; img-src 'self' data:; style-src 'self'")
		next.ServeHTTP(w, r)
	})
}

func (s *Server) api(h func(http.ResponseWriter, *http.Request) any) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		v := h(w, r)
		w.Header().Set("Content-Type", "application/json")
		if e, ok := v.(error); ok {
			w.WriteHeader(http.StatusConflict)
			v = map[string]any{"error": textOf(e)}
		}
		_ = json.NewEncoder(w).Encode(v)
	}
}

type imageJSON struct {
	Name    string `json:"name"`
	Version string `json:"version"`
	Kind    string `json:"kind"`
	Size    int    `json:"size"`
	Signed  bool   `json:"signed"`
	SHA256  string `json:"sha256"`
	Source  string `json:"source"`
	Notes   []text `json:"notes"`
}

// lang picks the page's language from Accept-Language: German if German comes before English.
func lang(r *http.Request) string {
	for _, part := range strings.Split(r.Header.Get("Accept-Language"), ",") {
		tag := strings.ToLower(strings.TrimSpace(strings.SplitN(part, ";", 2)[0]))
		switch {
		case strings.HasPrefix(tag, "de"):
			return "de"
		case strings.HasPrefix(tag, "en"):
			return "en"
		}
	}
	return "en"
}

func (s *Server) state(w http.ResponseWriter, r *http.Request) any {
	s.mu.Lock()
	s.seen = time.Now()
	busy := s.job != nil && s.job.Running
	refresh := !busy && time.Since(s.portsAt) > 1500*time.Millisecond
	s.mu.Unlock()
	if refresh { // not while flashing: listing asks USB devices for their names
		ps, err := s.Sys.Ports()
		if err == nil {
			var shown []flash.PortInfo
			for _, p := range ps {
				if p.Candidate() || p.Kind == flash.KindUART {
					shown = append(shown, p)
				}
			}
			s.mu.Lock()
			s.ports, s.portsAt = shown, time.Now()
			s.mu.Unlock()
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	out := map[string]any{"version": s.Version, "lang": lang(r), "ports": s.ports, "port": s.Port, "erase": s.Erase}
	if out["ports"] == nil {
		out["ports"] = []flash.PortInfo{}
	}
	if s.image != nil {
		src := "file"
		if s.upload == nil && s.Default != nil {
			if _, ok := s.Default.(flash.Embedded); ok {
				src = "embedded"
			}
		}
		out["image"] = imageJSON{Name: s.image.Name, Version: s.image.Version, Kind: s.image.Kind, Size: s.image.Size,
			Signed: s.image.Signed, SHA256: s.image.SHA256, Source: src, Notes: texts(s.image.Notes)}
	}
	if s.imageErr != nil {
		out["imageError"] = textOf(s.imageErr)
	}
	if s.job != nil {
		j := *s.job
		j.Log = append([]string(nil), s.job.Log...)
		out["job"] = j
	}
	return out
}

type startReq struct {
	Port  string `json:"port"`
	Erase bool   `json:"erase"`
}

func (s *Server) start(kind string, r *http.Request, run func(f *flash.Flasher, req startReq, j *job)) any {
	var req startReq
	_ = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req)
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.job != nil && s.job.Running {
		return flash.Busy
	}
	if kind == "install" && s.image == nil {
		if s.imageErr != nil {
			return s.imageErr
		}
		return flash.ErrNoImage
	}
	j := &job{Kind: kind, Running: true, Stage: string(flash.StageConnect), started: time.Now(), Log: []string{}}
	s.job = j
	f := &flash.Flasher{Sys: s.Sys,
		Log: func(line string) {
			s.mu.Lock()
			defer s.mu.Unlock()
			j.Log = append(j.Log, time.Since(j.started).Truncate(time.Millisecond).String()+"  "+line)
			if len(j.Log) > 2000 {
				j.Log = j.Log[len(j.Log)-2000:]
			}
		},
		Progress: func(st flash.Stage, done, total int) {
			s.mu.Lock()
			defer s.mu.Unlock()
			j.Stage, j.Done, j.Total = string(st), done, total
		}}
	go run(f, req, j)
	return map[string]bool{"ok": true}
}

func (s *Server) finish(j *job, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	j.Running = false
	if err != nil {
		j.Error = textOf(err)
		j.Log = append(j.Log, "error: "+err.Error())
		var m *flash.Msg
		if errors.As(err, &m) && m.Err != nil {
			j.Log = append(j.Log, "cause: "+m.Err.Error())
		}
	}
}

func (s *Server) install(w http.ResponseWriter, r *http.Request) any {
	s.mu.Lock()
	im := s.image
	s.mu.Unlock()
	return s.start("install", r, func(f *flash.Flasher, req startReq, j *job) {
		if im == nil {
			s.finish(j, flash.ErrNoImage)
			return
		}
		f.Log(fmt.Sprintf("image %s (%s, %d bytes, version %q, signed %v)", im.Name, im.Kind, im.Size, im.Version, im.Signed))
		res, err := f.Install(context.Background(), flash.Options{Port: req.Port, Image: im, Erase: req.Erase})
		if err == nil {
			s.mu.Lock()
			j.Result = &result{Port: res.Port, Setup: res.Setup}
			j.Chip = chipOf(res.Chip)
			j.Notes = texts(res.Notes)
			j.Stage = "done"
			s.mu.Unlock()
		}
		s.finish(j, err)
	})
}

func (s *Server) chip(w http.ResponseWriter, r *http.Request) any {
	return s.start("chip", r, func(f *flash.Flasher, req startReq, j *job) {
		c, notes, err := f.ChipInfo(context.Background(), req.Port)
		s.mu.Lock()
		j.Chip = chipOf(c)
		j.Notes = texts(notes)
		j.Stage = "done"
		s.mu.Unlock()
		s.finish(j, err)
	})
}

// uploadImage takes a .bin as the request body; its signature is inside it (tentacle sign, fwsign.Embed).
func (s *Server) uploadImage(w http.ResponseWriter, r *http.Request) any {
	name := r.URL.Query().Get("name")
	data, err := io.ReadAll(io.LimitReader(r.Body, 17<<20))
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.job != nil && s.job.Running {
		return flash.Busy
	}
	if strings.HasSuffix(strings.ToLower(name), ".sig") {
		return errors.New("choose the .bin: its signature is inside it (a separate .sig is only read next to --image FILE)")
	}
	s.upload = &flash.Bytes{Name: name, Data: data}
	s.image, s.imageErr = flash.Open(context.Background(), *s.upload)
	return map[string]bool{"ok": s.imageErr == nil}
}

func (s *Server) resetImage(w http.ResponseWriter, r *http.Request) any {
	s.mu.Lock()
	s.upload = nil
	s.mu.Unlock()
	s.loadDefault()
	return map[string]bool{"ok": true}
}
