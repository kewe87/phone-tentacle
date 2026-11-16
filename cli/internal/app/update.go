package app

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/kewe87/phone-tentacle/cli/internal/daemon"
	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
)

// update runs `update FILE.bin`: the daemon sends the image and the signature in it (fwsign.Split) over the session,
// the board checks it and restarts into it, the daemon waits for it to come back.
func (a *App) update(args []string) int {
	dev := new(string)
	fs := newFS(dev)
	force := fs.Bool("force", false, "send an image without a Tentacle version marker")
	pos, err := parse(fs, args)
	if err != nil {
		return a.fail(err)
	}
	if len(pos) != 1 {
		return a.fail(errors.New("usage: tentacle update FILE.bin [--force]  (a signed image: tentacle sign)"))
	}
	path, err := filepath.Abs(pos[0])
	if err != nil {
		return a.fail(err)
	}
	if _, sig, err := fwsign.Load(path); err != nil {
		return a.fail(err)
	} else if sig == nil {
		fmt.Fprintf(a.Err, "%s is not signed: sending it unsigned (only a debug build takes that).\n", filepath.Base(path))
	}
	return a.call(daemon.Request{Cmd: "update", Device: *dev, Text: path, Force: *force})
}

// updateProgress shows the update's progress lines.
func (a *App) updateProgress(d map[string]any) {
	num := func(k string) int { f, _ := d[k].(float64); return int(f) }
	switch d["update"] {
	case "send":
		if from, ok := d["from"].(string); ok {
			to, _ := d["to"].(string)
			signed := "unsigned"
			if s, _ := d["signed"].(bool); s {
				signed = "signed"
			}
			fmt.Fprintf(a.Err, "Updating the board: %s -> %s (%d KB, %s).\n", from, to, num("total")/1024, signed)
			return
		}
		if t := num("total"); t > 0 {
			fmt.Fprintf(a.Err, "  sent %3d%% (%d KB)\n", num("done")*100/t, num("done")/1024)
		}
	case "restart":
		fmt.Fprintln(a.Err, "Verified. The board restarts into the new firmware; waiting for it to come back...")
		fmt.Fprintln(a.Err, "Afterwards start Screen Mirroring on the iPhone again (Control Center > Screen Mirroring > Tentacle).")
	}
}

// debugCmd runs `debug ARGS`: one of the board's dbg commands (debug builds), the answer as text.
func (a *App) debugCmd(args []string) int {
	dev := new(string)
	fs := newFS(dev)
	pos, err := parse(fs, args)
	if err != nil {
		return a.fail(err)
	}
	return a.call(daemon.Request{Cmd: "debug", Device: *dev, Text: strings.Join(pos, " ")})
}

// followBoardLog runs `logs --board --follow`: the board's new log lines on stdout until Ctrl+C (debug builds).
func (a *App) followBoardLog(dev string) int {
	line, err := a.Client.Stream(daemon.Request{Cmd: "logfollow", Device: dev}, a.Out)
	if err != nil {
		return a.fail(err)
	}
	var r daemon.Response
	if json.Unmarshal(line, &r) == nil && r.Status != "ok" {
		return a.print(r)
	}
	return 0
}

// keygen and sign: the release key (docs/cli-protocol.md, "Firmware update"). Not in the usage: for whoever
// builds releases.
func (a *App) keygen(args []string) int {
	if len(args) != 1 {
		return a.fail(errors.New("usage: tentacle keygen DIR   (writes DIR/tentacle-release.key, keep it out of any repository)"))
	}
	pub, keyPath, err := fwsign.Keygen(args[0])
	if err != nil {
		return a.fail(err)
	}
	return a.print(daemon.Response{Status: "ok", Data: map[string]string{"key": keyPath, "public": fmt.Sprintf("%x", []byte(pub)),
		"ota_pubkey_h": fwsign.CArray(pub)}})
}

func (a *App) sign(args []string) int {
	dev := new(string)
	fs := newFS(dev)
	key := fs.String("key", os.Getenv("TENTACLE_RELEASE_KEY"), "the private key file (tentacle keygen)")
	pos, err := parse(fs, args)
	if err != nil {
		return a.fail(err)
	}
	if len(pos) != 1 || *key == "" {
		return a.fail(errors.New("usage: tentacle sign FILE.bin --key KEYFILE   (puts the signature into FILE.bin)"))
	}
	priv, err := fwsign.LoadKey(*key)
	if err != nil {
		return a.fail(err)
	}
	img, err := fwsign.SignFile(priv, pos[0])
	if err != nil {
		return a.fail(err)
	}
	return a.print(daemon.Response{Status: "ok", Data: map[string]string{"signed": pos[0], "version": fwsign.Version(img)}})
}
