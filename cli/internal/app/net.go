package app

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"golang.org/x/term"

	"github.com/onehumanbeing/esp32-iphone-agent/cli/internal/daemon"
)

// netJoin is `net join` without arguments: ask for the Wi-Fi's name and password (not echoed, not in the shell
// history), send them to the board over the current session (sealed), and wait until the board is found again on
// that Wi-Fi. The session drops in between: the board leaves the network it ran on.
func (a *App) netJoin(dev string) int {
	in := a.In
	if in == nil {
		in = os.Stdin
	}
	r := bufio.NewReader(in)
	if _, err := a.Client.Call(daemon.Request{Cmd: "net", Device: dev}); err != nil { // a session first
		return a.fail(err)
	}
	fmt.Fprint(a.Err, "Wi-Fi name (SSID): ")
	ssid, _ := r.ReadString('\n')
	ssid = strings.TrimRight(ssid, "\r\n")
	if ssid == "" {
		return a.fail(errors.New("no Wi-Fi name given"))
	}
	fmt.Fprint(a.Err, "Password (empty for an open network): ")
	var pass string
	if f, ok := in.(*os.File); ok && term.IsTerminal(int(f.Fd())) {
		b, err := term.ReadPassword(int(f.Fd()))
		fmt.Fprintln(a.Err)
		if err != nil {
			return a.fail(err)
		}
		pass = string(b)
	} else {
		pass, _ = r.ReadString('\n')
		pass = strings.TrimRight(pass, "\r\n")
	}
	if _, err := a.Client.Call(daemon.Request{Cmd: "net", Device: dev, Text: "join " + ssid + "\x00" + pass}); err != nil {
		return a.fail(err)
	}
	fmt.Fprintf(a.Err, "Sent. The board is joining %q; this computer and the iPhone have to be on that Wi-Fi too.\n", ssid)
	var last []byte
	for deadline := time.Now().Add(45 * time.Second); time.Now().Before(deadline); time.Sleep(2 * time.Second) {
		line, err := a.Client.Call(daemon.Request{Cmd: "net", Device: dev})
		if err != nil {
			continue
		}
		last = line
		var resp struct {
			Data struct {
				Board struct {
					Mode string `json:"mode"`
					IP   string `json:"ip"`
				} `json:"board"`
			} `json:"data"`
		}
		json.Unmarshal(line, &resp)
		if d := resp.Data; d.Board.Mode == "sta" && d.Board.IP != "" && d.Board.IP != "0.0.0.0" {
			fmt.Fprintf(a.Err, "Done: the board is on that Wi-Fi (%s).\n", d.Board.IP)
			a.waitMirroring(dev)
			a.Out.Write(line)
			return 0
		}
	}
	if last != nil {
		a.Out.Write(last)
	}
	return a.fail(errors.New("the board was not found on that Wi-Fi in 45 s: is this computer on the same Wi-Fi, and the password right? " +
		"A board that cannot join keeps trying and is not reachable meanwhile: hold its BOOT button 10 s for the setup hotspot"))
}

// waitMirroring walks the user through the step after the network: the iPhone mirroring to the board.
func (a *App) waitMirroring(dev string) {
	state := func() (ap, name string) {
		line, err := a.Client.Call(daemon.Request{Cmd: "info", Device: dev})
		if err != nil {
			return "", ""
		}
		var r struct {
			Data struct {
				AP   string `json:"ap"`
				Name string `json:"name"`
			} `json:"data"`
		}
		json.Unmarshal(line, &r)
		return r.Data.AP, r.Data.Name
	}
	ap, name := state()
	if ap == "streaming" {
		fmt.Fprintln(a.Err, "Screen Mirroring is on. Ready.")
		return
	}
	if name == "" {
		name = "ESP32 Agent"
	}
	fmt.Fprintf(a.Err, "Next, on the iPhone (on the same Wi-Fi): Control Center → Screen Mirroring → %q.\nWaiting for it (Ctrl+C to skip)...\n", name)
	for deadline := time.Now().Add(2 * time.Minute); time.Now().Before(deadline); time.Sleep(2 * time.Second) {
		if ap, _ = state(); ap == "streaming" {
			fmt.Fprintln(a.Err, "Screen Mirroring is on. Ready: try esp32agent screenshot.")
			return
		}
	}
	fmt.Fprintln(a.Err, "No mirroring yet. Is the iPhone on the same Wi-Fi as the board? Start it later the same way.")
}
