package daemon

import (
	"context"
	"errors"
	"log"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/audio"
	"github.com/kewe87/phone-tentacle/cli/internal/config"
	"github.com/kewe87/phone-tentacle/cli/internal/video"
)

// What the phone plays while it mirrors, for `tentacle listen` (docs/cli-protocol.md, "Phone audio"): the board
// sends the raw audio, whisper.cpp transcribes it here.

// hearing is a Conn's audio state.
type hearing struct {
	mu     sync.Mutex
	seg    audio.Segmenter
	recent audio.Recent
	work   chan *audio.Segment
	local  func() *audio.Local // nil: raw audio isn't wanted
}

// onAudio takes one record off stream A.
func (c *Conn) onAudio(b []byte, gap bool) {
	h := c.hear
	if h == nil || h.local == nil {
		return
	}
	r, ok := audio.ParseRecord(b)
	if !ok {
		return
	}
	now := time.Now()
	h.mu.Lock()
	before := h.seg.Current()
	done := h.seg.Add(now, r, gap)
	cur := h.seg.Current()
	h.mu.Unlock()
	if done != nil {
		h.recent.End(done.Start, done.End)
		select {
		case h.work <- done:
		default: // transcription is behind: this one stays without words
			h.recent.Done(done.Start, "")
		}
	}
	if cur != nil && cur != before {
		h.recent.Begin(cur.Start)
	}
}

// transcribe runs the segments one at a time.
func (h *hearing) transcribe() {
	for seg := range h.work {
		l := h.local()
		if l == nil {
			h.recent.Done(seg.Start, "")
			continue
		}
		ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
		started := time.Now()
		text, err := l.Transcribe(ctx, seg)
		cancel()
		if err != nil {
			log.Printf("listen: %v", err)
		}
		log.Printf("listen: %.1f s segment transcribed in %s", seg.End.Sub(seg.Start).Seconds(), time.Since(started).Round(10*time.Millisecond))
		h.recent.Done(seg.Start, text)
	}
}

// local is the transcriber for the current settings, nil unless listening.
func (s *Server) local() *audio.Local {
	l := s.Config.GetListen()
	if !l.On {
		return nil
	}
	ff, _ := video.FFmpeg()
	dir, _ := config.Dir()
	second := audio.Lang(l.Second)
	if second == "" {
		second = systemSecond()
	}
	return &audio.Local{FFmpeg: ff, Whisper: audio.FindWhisper(), Model: audio.ModelPath(dir), Second: second}
}

func (s *Server) hearingFor(c *Conn) {
	c.hear = &hearing{work: make(chan *audio.Segment, 4), local: s.local}
	c.rawWanted = func() bool { return s.local() != nil }
	go c.hear.transcribe()
}

// listen: `listen` (what was heard), `listen on|off`, `listen lang CODE|none`.
func (s *Server) listen(ctx context.Context, c *Conn, arg string) Response {
	l := s.Config.GetListen()
	verb, rest, _ := strings.Cut(strings.TrimSpace(arg), " ")
	switch verb {
	case "":
	case "on", "off":
		l.On = verb == "on"
	case "lang":
		if rest == "" {
			return fail(errors.New("usage: listen lang CODE|none (the language besides English, e.g. zh)"))
		}
		if rest == "none" {
			rest = ""
		}
		l.Second = rest
	default:
		return fail(errors.New("usage: listen [on|off | lang CODE|none]"))
	}
	if verb != "" {
		if l.On { // the words are made here: without the tools there is nothing to turn on
			if err := (&audio.Local{FFmpeg: ffmpegPath(), Whisper: audio.FindWhisper(), Model: modelPath()}).Check(); err != nil {
				return fail(errors.New(err.Error() + "; listening stays off"))
			}
		}
		if err := s.Config.SetListen(l); err != nil {
			return fail(err)
		}
		raw := "listen raw off"
		if s.local() != nil {
			raw = "listen raw on"
		}
		c.Send(ctx, raw)
	}
	return ok(s.heard(c, l))
}

// systemSecond is this computer's first preferred language other than English ("" if none).
func systemSecond() string {
	var langs []string
	if runtime.GOOS == "darwin" { // System Settings › Language & Region; LANG often stays en_US there
		out, _ := exec.Command("defaults", "read", "-g", "AppleLanguages").Output()
		for _, f := range strings.FieldsFunc(string(out), func(r rune) bool { return strings.ContainsRune("(),\"\n ", r) }) {
			langs = append(langs, f)
		}
	}
	langs = append(langs, os.Getenv("LANGUAGE"), os.Getenv("LANG"))
	for _, l := range langs {
		if c := audio.Lang(l); c != "" && c != "en" && c != "c" && c != "posix" {
			return c
		}
	}
	return ""
}

func ffmpegPath() string { ff, _ := video.FFmpeg(); return ff }
func modelPath() string  { dir, _ := config.Dir(); return audio.ModelPath(dir) }

// heard is `listen`'s answer: the settings and the text for the agent, which also says when nothing can be heard.
func (s *Server) heard(c *Conn, l config.Listen) map[string]any {
	out := map[string]any{"on": l.On, "second": l.Second}
	text := ""
	if !l.On {
		text = "Phone audio: listening is off (tentacle listen on), so the phone's sound is not heard."
	} else {
		local := s.local()
		how := "words transcribed on this computer by whisper.cpp"
		if local != nil && local.Second != "" {
			out["second"] = local.Second
			how += " (English or " + local.Second + ", labelled)"
		}
		if text = audio.Block(time.Now(), c.hear.recent.Items(), how); text == "" {
			text = "Phone audio: listening; no sound in the last 30 s."
		}
	}
	out["heard"] = text
	return out
}
