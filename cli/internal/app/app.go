// Package app is the tentacle command line: mobilecli-style commands, each a request to the daemon.
package app

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/config"
	"github.com/kewe87/phone-tentacle/cli/internal/daemon"
	"github.com/kewe87/phone-tentacle/cli/internal/link"
)

// Version is set at build time (-ldflags "-X .../internal/app.Version=1.2.3").
var Version = "dev"

const usage = `tentacle: see and drive an iPhone through a Tentacle board, over Wi-Fi.

  devices                          boards on this network (found by mDNS)
  pair                             pair this computer. First double-press BOOT on the board (pairing opens for
                                   60 s). The board then shows this computer's name and a 6-digit code (OLED),
                                   its LED blinks; compare the code, press BOOT once more within 30 s
  unpair [--board]                 forget the board here; --board: the board forgets every paired computer too
  screenshot [--format png|jpeg] [--quality N] [--scale F] [--max-size N] [--output PATH|-] [--wait MS]
                                   after input, waits up to --wait ms (3000) for a newer picture; the result
                                   says how old the picture is (age_ms) and "stale" with a warning when none came
                                   or while the board still replays buffered video ("catching_up")
  io tap X,Y [--aim] [--count N]   coordinates are screenshot pixels; --count 2 double-taps
  io longpress X,Y [--duration MS]
  io swipe X1,Y1,X2,Y2
  io text 'TEXT'                   by the board's keyboard layout (ASCII; on de also ä ö ü ß € ...); Chinese is
                                   typed as pinyin
  io keys COMBO                    enter, esc, tab, backspace, space, up, cmd+a, ...
  io button HOME
  screencapture --format h264|mjpeg   video to stdout
  device info                      board status
  device settings                  the board's settings: name, display, abs, battery, battery_div, lang
  device set KEY VALUE             one of them, kept on the board: display auto|on|off (status OLED), abs on|off
                                   (absolute pointer, after a restart), battery none|GPIO, battery_div 1.00-10.00,
                                   lang auto|en|de (the OLED's texts), name NAME (after a restart)
  keyboard layout [us|de]          the iPhone's hardware keyboard layout the board types for (default us); must
                                   match Settings > General > Keyboard > Hardware Keyboard on the iPhone
  web password                     a new password for the board's web interface (asked twice, not shown); counts
                                   once BOOT on the board is pressed within 30 s
  video fps N | video res W H      what the board asks the iPhone for (15 fps, 480x1040), from the next
                                   Screen Mirroring session on
  net                              the board's Wi-Fi
  net join                         the board moves to another Wi-Fi: asks for its name and password and sends them
                                   over the current link (sealed). A new board is set up on its own hotspot's page.
  net join SSID [PASSWORD]         the same, non-interactive
  net hotspot                      the board forgets its Wi-Fi and opens its setup hotspot again
  calibrate                        measure the pointer (automatic on the first tap; none with the absolute pointer)
  listen                           what the iPhone played in the last 30 s, as words (or why it can't be heard)
  listen on|off                    hear the phone's sound while it mirrors (off by default); the words are made
                                   by whisper.cpp on this computer (install whisper-cpp, plus a model)
  listen lang CODE|none            the language besides English (e.g. zh); default this computer's
  history list | show RUN | new    what was done on the phone (runs; a new one after 10 min idle)
  intent list | show NAME          saved intents (tasks that worked, with their steps)
  intent save --name N --prompt P [--hint H ...] [--from-run RUN [--steps 1,3,4]]
  intent delete NAME
  intent export NAME... [--screenshots] [--output FILE]   one .tentacle-intent file, to move intents to another computer
  intent import FILE
  memory list | search TEXT        what the agent remembers (MEMORY.md in the config directory)
  memory add TEXT [--kind preference|fact|app] [--topic APP]
  memory rm ID
  memory export [--output FILE]    a copy of MEMORY.md, e.g. for another computer
  memory import FILE               fold in an exported copy: same id or words keep the newer
  mcp                              MCP server on stdio
  skill install                    agent skill for Claude Code / Codex
  logs [--board] [--save]          the CLI's log; --board the board's; --save both in one file (e.g. for an issue)
  logs --board --follow            the board's new log lines as they come, until Ctrl+C (debug firmware only)
  update FILE.bin [--force]        new firmware over Wi-Fi: FILE.bin.sig next to it is sent too. The board takes
                                   only images signed for it (a debug build unsigned ones too), restarts into it
                                   and goes back to the old one if the new one does not start properly
  debug [dbg] CMD                  a debug build's serial dbg command over Wi-Fi: state, nvs, wifi [scan], reboot
  daemon [stop]                    the background process (started automatically)
  version

  --device ID or TENTACLE_DEVICE when several boards are around.
  --host IP[:PORT] or TENTACLE_HOST: the board's address, when it is not found by itself (--host auto: find again).
  --verbose or TENTACLE_VERBOSE=1: show how the board is found, the handshake and timings (also in the daemon's log).
`

var errUnsupported = errors.New("not supported on Tentacle")

// App runs commands; Out gets the JSON (or raw bytes), Client reaches the daemon.
type App struct {
	In     io.Reader // net join's prompts; os.Stdin when nil
	Out    io.Writer
	Err    io.Writer
	Client *daemon.Client
	Dir    string
}

func Main(args []string) int {
	dir, err := config.Dir()
	a := &App{Out: os.Stdout, Err: os.Stderr, Dir: dir}
	if err != nil {
		return a.print(daemon.Response{Status: "error", Error: err.Error()})
	}
	a.Client = &daemon.Client{Dir: dir, Version: daemonVersion()}
	return a.Run(args)
}

// daemonVersion tells a running daemon whether it is this binary's. Version alone is "dev" in every build without
// -ldflags, so a daemon of an older dev build stayed in use after a rebuild (pair still asked a closed board again
// for a minute, 2026-11-16): such builds add the executable's size and time.
func daemonVersion() string {
	if Version != "dev" {
		return Version
	}
	exe, err := os.Executable()
	if err != nil {
		return Version
	}
	st, err := os.Stat(exe)
	if err != nil {
		return Version
	}
	return fmt.Sprintf("%s+%d.%d", Version, st.Size(), st.ModTime().UnixNano())
}

func (a *App) print(r daemon.Response) int {
	json.NewEncoder(a.Out).Encode(r)
	if r.Status != "ok" {
		return 1
	}
	return 0
}

func (a *App) fail(err error) int {
	if errors.Is(err, flag.ErrHelp) { // a -help the check in Run missed: never a JSON error
		fmt.Fprint(a.Err, usage)
		return 0
	}
	return a.print(daemon.Response{Status: "error", Error: err.Error()})
}

// call sends a request and prints the daemon's response as is.
func (a *App) call(req daemon.Request) int {
	line, err := a.Client.Call(req)
	if err != nil {
		return a.fail(err)
	}
	var r daemon.Response
	if json.Unmarshal(line, &r) != nil {
		return a.fail(errors.New("bad response from the daemon"))
	}
	a.Out.Write(line)
	if r.Status != "ok" {
		return 1
	}
	return 0
}

func (a *App) Run(args []string) int {
	if len(args) == 0 || args[0] == "-h" || args[0] == "--help" || args[0] == "help" && len(args) == 1 {
		fmt.Fprint(a.Err, usage)
		return 0
	}
	if args[0] == "--version" {
		args[0] = "version"
	}
	args = a.global(args)
	if len(args) == 0 {
		fmt.Fprint(a.Err, usage)
		return 0
	}
	cmd, rest := args[0], args[1:]
	if cmd == "help" && len(rest) > 0 { // tentacle help pair
		a.help(rest[0])
		return 0
	}
	if wantsHelp(rest) {
		a.help(cmd)
		return 0
	}
	dev := new(string)
	fs := newFS(dev)
	switch cmd {
	case "version":
		return a.print(daemon.Response{Status: "ok", Data: map[string]string{"version": Version}})
	case "daemon":
		idle := fs.Duration("idle-timeout", 10*time.Minute, "exit after this long without requests (0: never)")
		pos, err := parse(fs, rest)
		if err != nil {
			return a.fail(err)
		}
		if len(pos) > 0 && pos[0] == "stop" {
			return a.call(daemon.Request{Cmd: "stop"})
		}
		return a.daemon(*idle)
	case "mcp":
		return a.mcp(os.Stdin)
	case "history":
		return a.history(rest)
	case "intent", "intents":
		return a.intent(rest)
	case "memory":
		return a.memory(rest)
	case "logs":
		return a.logs(rest)
	case "update":
		return a.update(rest)
	case "debug":
		return a.debugCmd(rest)
	case "keygen":
		return a.keygen(rest)
	case "sign":
		return a.sign(rest)
	case "skill":
		if len(rest) == 0 || rest[0] != "install" {
			return a.fail(errors.New("usage: tentacle skill install"))
		}
		return a.skillInstall()
	case "pair":
		if _, err := parse(fs, rest); err != nil {
			return a.fail(err)
		}
		return a.pair(*dev)
	case "unpair":
		board := fs.Bool("board", false, "the board forgets every paired computer too")
		if _, err := parse(fs, rest); err != nil {
			return a.fail(err)
		}
		text := ""
		if *board {
			text = "board"
		}
		return a.call(daemon.Request{Cmd: cmd, Device: *dev, Text: text})
	case "devices", "calibrate":
		if _, err := parse(fs, rest); err != nil {
			return a.fail(err)
		}
		return a.call(daemon.Request{Cmd: cmd, Device: *dev})
	case "device":
		pos, err := parse(fs, rest)
		if err != nil {
			return a.fail(err)
		}
		if len(pos) >= 2 && pos[0] == "send" { // undocumented: one raw board command, for debugging
			return a.call(daemon.Request{Cmd: "send", Device: *dev, Text: strings.Join(pos[1:], " ")})
		}
		switch {
		case len(pos) == 1 && pos[0] == "settings":
			return a.call(daemon.Request{Cmd: "settings", Device: *dev})
		case len(pos) >= 3 && pos[0] == "set": // a name may hold spaces
			return a.call(daemon.Request{Cmd: "settings", Device: *dev, Text: pos[1] + " " + strings.Join(pos[2:], " ")})
		case len(pos) >= 1 && pos[0] == "set":
			return a.fail(errors.New("usage: tentacle device set KEY VALUE (display, abs, battery, battery_div, lang, name)"))
		}
		if len(pos) == 0 || pos[0] != "info" {
			return a.fail(errUnsupported)
		}
		return a.call(daemon.Request{Cmd: "info", Device: *dev})
	case "net":
		pos, err := parse(fs, rest)
		if err != nil {
			return a.fail(err)
		}
		if len(pos) == 1 && pos[0] == "join" {
			return a.netJoin(*dev)
		}
		text := strings.Join(pos, " ")
		if len(pos) >= 2 && pos[0] == "join" { // SSIDs and passwords may hold spaces
			text = "join " + pos[1] + "\x00" + strings.Join(pos[2:], " ")
		}
		return a.call(daemon.Request{Cmd: "net", Device: *dev, Text: text})
	case "web":
		pos, err := parse(fs, rest)
		if err != nil {
			return a.fail(err)
		}
		if len(pos) != 1 || pos[0] != "password" {
			return a.fail(errors.New("usage: tentacle web password"))
		}
		return a.webPassword(*dev)
	case "listen", "keyboard", "video":
		pos, err := parse(fs, rest)
		if err != nil {
			return a.fail(err)
		}
		return a.call(daemon.Request{Cmd: cmd, Device: *dev, Text: strings.Join(pos, " ")})
	case "screenshot":
		return a.screenshot(fs, dev, rest)
	case "screencapture":
		format := fs.String("format", "mjpeg", "h264 or mjpeg")
		quality := fs.Int("quality", 80, "JPEG quality (mjpeg)")
		if _, err := parse(fs, rest); err != nil {
			return a.fail(err)
		}
		line, err := a.Client.Stream(daemon.Request{Cmd: "screencapture", Device: *dev, Format: *format, Quality: *quality}, a.Out)
		if err != nil {
			return a.fail(err)
		}
		var r daemon.Response
		if json.Unmarshal(line, &r) == nil && r.Status != "ok" {
			return a.print(r)
		}
		return 0
	case "io":
		return a.io(fs, dev, rest)
	}
	return a.fail(errUnsupported)
}

// wantsHelp: -h, -help or --help among the arguments (before a "--").
func wantsHelp(args []string) bool {
	for _, arg := range args {
		switch arg {
		case "--":
			return false
		case "-h", "-help", "--help":
			return true
		}
	}
	return false
}

// help prints the usage lines of cmd (with their continuation lines), or all of it for an unknown command.
func (a *App) help(cmd string) {
	switch cmd {
	case "intents":
		cmd = "intent"
	case "keygen", "sign": // not in the usage: for whoever builds releases
		fmt.Fprintln(a.Err, "usage: tentacle keygen DIR                  writes DIR/tentacle-release.key (keep it out of any repository)\n"+
			"       tentacle sign FILE.bin --key KEYFILE   writes FILE.bin.sig (or TENTACLE_RELEASE_KEY for --key)")
		return
	}
	var out []string
	in := false
	for _, line := range strings.Split(usage, "\n") {
		trimmed := strings.TrimLeft(line, " ")
		indent := len(line) - len(trimmed)
		switch {
		case indent == 2 && (trimmed == cmd || strings.HasPrefix(trimmed, cmd+" ")):
			in = true
		case indent <= 2:
			in = false
		}
		if in {
			out = append(out, line)
		}
	}
	if len(out) == 0 {
		fmt.Fprint(a.Err, usage)
		return
	}
	fmt.Fprintf(a.Err, "usage: tentacle\n%s\n\n  --device ID, --host IP[:PORT], --verbose: see tentacle --help\n", strings.Join(out, "\n"))
}

// global takes --host and --verbose out of the arguments (they may stand anywhere) and hands them to the client,
// which sends them with every request. The environment gives the defaults.
func (a *App) global(args []string) []string {
	host, verbose := os.Getenv("TENTACLE_HOST"), os.Getenv("TENTACLE_VERBOSE") == "1"
	var out []string
	for i := 0; i < len(args); i++ {
		switch arg := args[i]; {
		case arg == "--":
			out = append(out, args[i:]...)
			i = len(args)
		case arg == "--verbose" || arg == "-verbose":
			verbose = true
		case (arg == "--host" || arg == "-host") && i+1 < len(args):
			i++
			host = args[i]
		case strings.HasPrefix(arg, "--host="):
			host = strings.TrimPrefix(arg, "--host=")
		default:
			out = append(out, arg)
		}
	}
	if a.Client != nil {
		a.Client.Host, a.Client.Verbose = host, verbose
		if a.Client.Progress == nil {
			a.Client.Progress = a.progress
		}
	}
	return out
}

// progress shows what the daemon sends ahead of its response: the --verbose trace, and pairing's code.
func (a *App) progress(line []byte) {
	var raw struct {
		Data map[string]any `json:"data"`
	}
	if json.Unmarshal(line, &raw) == nil && raw.Data["update"] != nil {
		a.updateProgress(raw.Data)
		return
	}
	var p struct {
		Data struct {
			Log     string `json:"log"`
			Pair    string `json:"pair"`
			Code    string `json:"code"`
			Name    string `json:"name"`
			WaitS   int    `json:"wait_s"`
			Message string `json:"message"`
		} `json:"data"`
	}
	if json.Unmarshal(line, &p) != nil {
		return
	}
	switch d := p.Data; {
	case d.Log != "":
		fmt.Fprintln(a.Err, "  "+d.Log)
	case d.Pair == "confirm":
		fmt.Fprintf(a.Err, "\nPairing with %s.\n\n    Code: %s\n\n"+
			"Press the BOOT button on the board now (within %d s). Its LED blinks fast while it waits.\n"+
			"If the board has a display, it must show this computer's name and the same code: if it shows\n"+
			"another code or another name, do NOT press, someone else is asking.\n", d.Name, d.Code, d.WaitS)
	case d.Message != "":
		fmt.Fprintln(a.Err, d.Message+"...")
	}
}

// pair runs `pair`: the daemon finds the board and asks; the code arrives as a progress line while the board waits
// for its button, the response once it was pressed (or not).
func (a *App) pair(dev string) int {
	fmt.Fprintln(a.Err, "The board only pairs after a double press of its BOOT button (two quick presses), for 60 s.")
	fmt.Fprintln(a.Err, "Looking for the board on this network...")
	return a.call(daemon.Request{Cmd: "pair", Device: dev})
}

func newFS(dev *string) *flag.FlagSet {
	fs := flag.NewFlagSet("tentacle", flag.ContinueOnError)
	fs.StringVar(dev, "device", os.Getenv("TENTACLE_DEVICE"), "board id")
	return fs
}

func (a *App) io(fs *flag.FlagSet, dev *string, args []string) int {
	aim := fs.Bool("aim", false, "find the pointer on screen and correct a miss before tapping")
	ms := fs.Int("duration", 1000, "long press, ms")
	count := fs.Int("count", 1, "taps in a row at the point (2: double tap)")
	pos, err := parse(fs, args)
	if err != nil {
		return a.fail(err)
	}
	if len(pos) == 0 {
		return a.fail(errors.New("usage: tentacle io tap|longpress|swipe|text|keys|button ..."))
	}
	sub, arg := pos[0], strings.Join(pos[1:], " ")
	req := daemon.Request{Cmd: sub, Device: *dev, Aim: *aim, Ms: *ms, Count: *count, Text: arg}
	switch sub {
	case "tap", "longpress", "swipe":
		n := 2
		if sub == "swipe" {
			n = 4
		}
		if req.Pts, err = coords(arg, n); err != nil {
			return a.fail(err)
		}
	case "text", "keys", "button":
		if len(pos) < 2 {
			return a.fail(fmt.Errorf("usage: tentacle io %s VALUE", sub))
		}
	default:
		return a.fail(errUnsupported)
	}
	return a.call(req)
}

func (a *App) screenshot(fs *flag.FlagSet, dev *string, args []string) int {
	format := fs.String("format", "png", "png or jpeg")
	quality := fs.Int("quality", 90, "JPEG quality 1-100")
	scale := fs.Float64("scale", 1, "0-1")
	maxSize := fs.Int("max-size", 0, "max(width, height) in pixels")
	output := fs.String("output", "", "file, directory/, or - for stdout")
	wait := fs.Int("wait", int(daemon.DefaultFreshWait/time.Millisecond), "after input, wait up to this many ms for a newer picture (0: none)")
	if _, err := parse(fs, args); err != nil {
		return a.fail(err)
	}
	if *format == "jpg" {
		*format = "jpeg"
	}
	waitMs := *wait
	if waitMs <= 0 {
		waitMs = -1 // 0 on the wire is the default
	}
	line, err := a.Client.Call(daemon.Request{Cmd: "screenshot", Device: *dev, Format: *format, Quality: *quality,
		Scale: *scale, MaxSize: *maxSize, WaitMs: waitMs})
	if err != nil {
		return a.fail(err)
	}
	var r struct {
		daemon.Response
		Data struct {
			Format, Data, Device string
			Width, Height        int
			AgeMs                *int64 `json:"age_ms"`
			CapturedAt           string `json:"captured_at"`
			Stale                bool   `json:"stale"`
			CatchingUp           any    `json:"catching_up"`
			Warning              string `json:"warning"`
		} `json:"data"`
	}
	if err := json.Unmarshal(line, &r); err != nil {
		return a.fail(err)
	}
	if r.Status != "ok" {
		return a.print(r.Response)
	}
	img, err := base64.StdEncoding.DecodeString(r.Data.Data)
	if err != nil {
		return a.fail(err)
	}
	if *output == "-" {
		if r.Data.Warning != "" {
			fmt.Fprintln(a.Err, "warning: "+r.Data.Warning)
		}
		a.Out.Write(img)
		return 0
	}
	path, err := screenshotPath(*output, r.Data.Device, r.Data.Format)
	if err != nil {
		return a.fail(err)
	}
	if err := os.WriteFile(path, img, 0o644); err != nil {
		return a.fail(err)
	}
	out := map[string]any{"format": r.Data.Format, "filePath": path, "width": r.Data.Width, "height": r.Data.Height,
		"stale": r.Data.Stale}
	if r.Data.AgeMs != nil {
		out["age_ms"], out["captured_at"] = *r.Data.AgeMs, r.Data.CapturedAt
	}
	if r.Data.CatchingUp != nil {
		out["catching_up"] = r.Data.CatchingUp
	}
	if r.Data.Warning != "" {
		out["warning"] = r.Data.Warning
	}
	return a.print(daemon.Response{Status: "ok", Data: out})
}

// screenshotPath: a file path as given, or screenshot-<device>-<time>.<ext> in a directory (default: cwd).
func screenshotPath(out, device, format string) (string, error) {
	if out != "" && !os.IsPathSeparator(out[len(out)-1]) {
		return filepath.Abs(out)
	}
	ext := "png"
	if format == "jpeg" {
		ext = "jpg"
	}
	base, err := filepath.Abs(filepath.Join(out, fmt.Sprintf("screenshot-%s-%s", device, time.Now().Format("20060102150405"))))
	if err != nil {
		return "", err
	}
	path := base + "." + ext
	for i := 1; ; i++ {
		if _, err := os.Stat(path); err != nil {
			return path, nil
		}
		path = fmt.Sprintf("%s-%d.%s", base, i, ext)
	}
}

func (a *App) daemon(idle time.Duration) int {
	cfg, err := config.Load()
	if err != nil {
		return a.fail(err)
	}
	l, err := daemon.Listen(a.Dir)
	if err != nil {
		return a.fail(err)
	}
	log.SetFlags(log.LstdFlags)
	log.Printf("tentacle %s daemon on %s", Version, daemon.SocketPath(a.Dir))
	s := &daemon.Server{Transport: link.Default(a.Dir), Config: cfg, Dir: a.Dir, Version: daemonVersion(), Idle: idle}
	if err := s.Serve(l); err != nil {
		log.Print(err)
		return 1
	}
	os.Remove(daemon.SocketPath(a.Dir))
	return 0
}
