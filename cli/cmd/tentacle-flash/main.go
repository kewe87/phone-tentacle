// tentacle-flash installs the Tentacle firmware on a Seeed XIAO ESP32-S3 over USB, with nothing else to install.
//
// Started without arguments (a double click) it opens an installer page in the browser. With --yes it installs
// right away on the command line.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strings"
	"time"

	"golang.org/x/term"

	"github.com/kewe87/phone-tentacle/cli/internal/flash"
	"github.com/kewe87/phone-tentacle/cli/internal/flashui"
)

// version is set at build time (-ldflags "-X main.version=1.2.3").
var version = "dev"

func main() { os.Exit(run(os.Args[1:], os.Stdout, os.Stderr)) }

const usage = `tentacle-flash %s: installs the Tentacle firmware on a Seeed XIAO ESP32-S3 over USB.

  tentacle-flash                       open the installer in the browser
  tentacle-flash --yes [options]       install without questions, on the command line
  tentacle-flash --info [--port P]     ask a running board for its setup Wi-Fi
  tentacle-flash --chip [--port P]     show the chip, flash and PSRAM of the board
  tentacle-flash --list                list the serial ports

Options:
`

func run(args []string, out, errOut io.Writer) int {
	fs := flag.NewFlagSet("tentacle-flash", flag.ContinueOnError)
	fs.SetOutput(errOut)
	yes := fs.Bool("yes", false, "install right away, without the browser page")
	port := fs.String("port", "", "the board's serial port (COM5, /dev/cu.usbmodem1101, /dev/ttyACM0); default: find it")
	image := fs.String("image", "", "firmware image to install (a *.merged.bin, signed with tentacle sign); default: the built-in one")
	erase := fs.Bool("erase", false, "erase the whole flash first (removes the stored Wi-Fi and the pairing)")
	info := fs.Bool("info", false, "ask the board for its setup Wi-Fi and exit")
	chip := fs.Bool("chip", false, "show the chip info and exit")
	list := fs.Bool("list", false, "list the serial ports and exit")
	noBrowser := fs.Bool("no-browser", false, "print the page's address instead of opening the browser")
	langFlag := fs.String("lang", "", "language of the messages: de or en (default: the system's)")
	showVersion := fs.Bool("version", false, "print the version")
	fs.Usage = func() {
		fmt.Fprintf(errOut, usage, version)
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() > 0 {
		fs.Usage()
		return 2
	}
	lang := *langFlag
	if lang == "" {
		lang = systemLang()
	}
	if lang != "de" {
		lang = "en"
	}
	c := &cli{out: out, err: errOut, lang: lang, tty: isTTY(out)}
	enableVT()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	sys := flash.Serial{}

	var src flash.Source = flash.Embedded{}
	if *image != "" {
		src = flash.File{Path: *image}
	}
	switch {
	case *showVersion:
		fmt.Fprintln(out, "tentacle-flash", version)
		return 0
	case *list:
		return c.list(sys)
	case *info:
		return c.info(ctx, sys, *port)
	case *chip:
		return c.chip(ctx, sys, *port)
	case *yes:
		return c.install(ctx, sys, src, *port, *erase)
	}
	return c.gui(ctx, sys, src, *port, *erase, *noBrowser)
}

type cli struct {
	out, err io.Writer
	lang     string
	tty      bool
	stage    flash.Stage
	lastPct  int
}

var words = map[string][2]string{
	"connect":   {"Looking for the board…", "Suche das Board…"},
	"check":     {"Checking the chip…", "Prüfe den Chip…"},
	"erase":     {"Erasing the whole flash…", "Lösche den ganzen Flash…"},
	"write":     {"Writing the firmware…", "Schreibe die Firmware…"},
	"verify":    {"Checking what was written…", "Prüfe das Geschriebene…"},
	"restart":   {"Restarting the board…", "Starte das Board neu…"},
	"setup":     {"Asking the board for its setup Wi-Fi (up to 20 s)…", "Frage das Board nach seinem Einrichtungs-WLAN (bis zu 20 s)…"},
	"image":     {"Firmware: %s", "Firmware: %s"},
	"signed":    {"signed", "signiert"},
	"unsigned":  {"NOT signed", "NICHT signiert"},
	"chip":      {"Board: %s %s on %s, %s flash, PSRAM %s, MAC %s", "Board: %s %s an %s, %s Flash, PSRAM %s, MAC %s"},
	"none":      {"none", "keins"},
	"done":      {"Done: Tentacle is installed.", "Fertig: Tentacle ist installiert."},
	"warning":   {"Warning: %s", "Achtung: %s"},
	"setupWifi": {"Setup Wi-Fi : %s", "Einrichtungs-WLAN : %s"},
	"setupPass": {"Password    : %s", "Passwort          : %s"},
	"scan":      {"Scan this code with the phone's camera to join without typing the password.", "Diesen Code mit der Handykamera scannen, um ohne Passworteingabe beizutreten."},
	"how":       {"Join that Wi-Fi with a phone or laptop; the setup page opens by itself (or open %s).\nChoose your home Wi-Fi, enter its password, then press the BOOT button (B) on the board within 30 s.", "Tritt diesem WLAN mit Handy oder Laptop bei; die Einrichtungsseite öffnet sich von selbst (sonst %s öffnen).\nDas eigene WLAN wählen, Passwort eingeben, dann innerhalb von 30 s die BOOT-Taste (B) am Board drücken."},
	"stored":    {"The board already has a Wi-Fi stored: '%s' (address %s). To set it up again, hold BOOT for 10 s.", "Auf dem Board ist schon ein WLAN gespeichert: '%s' (Adresse %s). Zum Neu-Einrichten BOOT 10 s halten."},
	"page":      {"The installer is open in your browser: %s\nKeep this window open while it runs; closing it ends the installer.", "Der Installer ist im Browser geöffnet: %s\nDieses Fenster offen lassen, solange er läuft; Schließen beendet den Installer."},
	"ipPending": {"still connecting", "verbindet sich noch"},
	"pageNo":    {"Open this address in a browser: %s", "Diese Adresse im Browser öffnen: %s"},
	"noPorts":   {"No serial ports.", "Keine seriellen Anschlüsse."},
	"next":      {"Next: the manual, chapter 3 (Wi-Fi) and 5 (pairing).", "Weiter im Handbuch, Kapitel 3 (WLAN) und 5 (Koppeln)."},
}

func (c *cli) w(key string, args ...any) string {
	t := words[key]
	f := t[0]
	if c.lang == "de" {
		f = t[1]
	}
	if len(args) == 0 {
		return f
	}
	return fmt.Sprintf(f, args...)
}

func (c *cli) text(err error) string {
	var m *flash.Msg
	if errors.As(err, &m) {
		return m.Text(c.lang)
	}
	return err.Error()
}

func (c *cli) fail(err error) int {
	if c.tty && c.stage == flash.StageWrite {
		fmt.Fprintln(c.out)
	}
	fmt.Fprintln(c.err, c.text(err))
	return 1
}

func (c *cli) progress(s flash.Stage, done, total int) {
	if s != c.stage {
		if c.tty && c.stage == flash.StageWrite {
			fmt.Fprintln(c.out)
		}
		c.stage, c.lastPct = s, -1
		if s != flash.StageWrite || !c.tty {
			fmt.Fprintln(c.out, c.w(string(s)))
		}
	}
	if s != flash.StageWrite || total == 0 {
		return
	}
	pct := done * 100 / total
	switch {
	case c.tty && pct != c.lastPct:
		fmt.Fprintf(c.out, "\r%s %3d %%", c.w("write"), pct)
	case !c.tty && pct/10 != c.lastPct/10:
		fmt.Fprintf(c.out, "  %3d %%\n", pct)
	default:
		return
	}
	c.lastPct = pct
}

func (c *cli) notes(ms []*flash.Msg) {
	for _, m := range ms {
		fmt.Fprintln(c.err, c.w("warning", m.Text(c.lang)))
	}
}

func (c *cli) flasher(sys flash.System) *flash.Flasher {
	f := &flash.Flasher{Sys: sys, Progress: c.progress}
	if os.Getenv("TENTACLE_FLASH_DEBUG") != "" {
		f.Log = func(s string) { fmt.Fprintln(c.err, "  ·", s) }
	}
	return f
}

func (c *cli) install(ctx context.Context, sys flash.System, src flash.Source, port string, erase bool) int {
	im, err := flash.Open(ctx, src)
	if err != nil {
		return c.fail(err)
	}
	sig := c.w("unsigned")
	if im.Signed {
		sig = c.w("signed")
	}
	desc := im.Name
	if im.Version != "" {
		desc += " (" + im.Version + ")"
	}
	fmt.Fprintln(c.out, c.w("image", desc+", "+sig))
	c.notes(im.Notes)
	res, err := c.flasher(sys).Install(ctx, flash.Options{Port: port, Image: im, Erase: erase})
	if err != nil {
		return c.fail(err)
	}
	ch := res.Chip
	psram := ch.PSRAM
	if psram == "" {
		psram = c.w("none")
	}
	fmt.Fprintln(c.out, c.w("chip", ch.Name, ch.Revision, res.Port, ch.FlashSizeText(), psram, ch.MAC))
	c.notes(res.Notes)
	fmt.Fprintln(c.out, c.w("done"))
	if res.Setup == nil {
		fmt.Fprintln(c.out, (&flash.Msg{Code: "no_setup"}).Text(c.lang))
	} else {
		c.showSetup(res.Setup)
	}
	fmt.Fprintln(c.out, c.w("next"))
	return 0
}

func (c *cli) showSetup(s *flash.SetupInfo) {
	if !s.Setup {
		ip := s.IP
		if ip == "" {
			ip = c.w("ipPending")
		}
		fmt.Fprintln(c.out, c.w("stored", s.SSID, ip))
		return
	}
	fmt.Fprintln(c.out)
	fmt.Fprintln(c.out, "  "+c.w("setupWifi", s.SSID))
	fmt.Fprintln(c.out, "  "+c.w("setupPass", s.Pass))
	fmt.Fprintln(c.out)
	if m := flash.QRModules(s.QR); m != nil {
		printQR(c.out, m)
		fmt.Fprintln(c.out)
		fmt.Fprintln(c.out, c.w("scan"))
	}
	url := s.URL
	if url == "" {
		url = "http://192.168.4.1/"
	}
	fmt.Fprintln(c.out, c.w("how", url))
}

// printQR prints a QR code with half blocks, dark modules on a white background with a 4-module quiet zone, so a
// phone camera reads it from a dark or a light terminal (as hardware/flash.sh does).
func printQR(w io.Writer, m [][]bool) {
	n := len(m)
	dark := func(x, y int) bool { return x >= 0 && y >= 0 && x < n && y < n && m[y][x] }
	for y := -4; y < n+4; y += 2 {
		var b strings.Builder
		for x := -4; x < n+4; x++ {
			t, u := dark(x, y), dark(x, y+1)
			switch {
			case t && u:
				b.WriteString("█")
			case t:
				b.WriteString("▀")
			case u:
				b.WriteString("▄")
			default:
				b.WriteString(" ")
			}
		}
		fmt.Fprintf(w, "  \x1b[30;107m%s\x1b[0m\n", b.String())
	}
}

func (c *cli) info(ctx context.Context, sys flash.System, port string) int {
	s, err := c.flasher(sys).AskSetup(ctx, port, 8*time.Second)
	if err != nil {
		return c.fail(err)
	}
	c.showSetup(s)
	return 0
}

func (c *cli) chip(ctx context.Context, sys flash.System, port string) int {
	ch, notes, err := c.flasher(sys).ChipInfo(ctx, port)
	if err != nil {
		return c.fail(err)
	}
	fmt.Fprintf(c.out, "chip      %s %s\nmac       %s\nflash     %s (JEDEC id %06x)\npsram     %s\nusb-jtag  %v\n",
		ch.Name, ch.Revision, ch.MAC, ch.FlashSizeText(), ch.FlashID, orNone(ch.PSRAM, c.w("none")), ch.USBJTAG)
	if ch.EmbeddedFlash != "" {
		fmt.Fprintf(c.out, "embedded  flash %s\n", ch.EmbeddedFlash)
	}
	c.notes(notes)
	return 0
}

func orNone(s, none string) string {
	if s == "" {
		return none
	}
	return s
}

func (c *cli) list(sys flash.System) int {
	ps, err := sys.Ports()
	if err != nil {
		return c.fail(err)
	}
	if len(ps) == 0 {
		fmt.Fprintln(c.out, c.w("noPorts"))
	}
	for _, p := range ps {
		fmt.Fprintf(c.out, "%-24s %-10s %s:%s %q %s\n", p.Name, p.Kind, p.VID, p.PID, p.Product, p.Serial)
	}
	return 0
}

func (c *cli) gui(ctx context.Context, sys flash.System, src flash.Source, port string, erase, noBrowser bool) int {
	s := &flashui.Server{Sys: sys, Version: version, Default: src, Port: port, Erase: erase, Idle: 3 * time.Minute}
	ln, url, err := s.Listen()
	if err != nil {
		return c.fail(err)
	}
	if noBrowser || openBrowser(url) != nil {
		fmt.Fprintln(c.out, c.w("pageNo", url))
	} else {
		fmt.Fprintln(c.out, c.w("page", url))
	}
	if err := s.Serve(ctx, ln); err != nil && !errors.Is(err, context.Canceled) {
		return c.fail(err)
	}
	return 0
}

func openBrowser(url string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	return cmd.Start()
}

func isTTY(w io.Writer) bool {
	f, ok := w.(*os.File)
	return ok && term.IsTerminal(int(f.Fd()))
}
