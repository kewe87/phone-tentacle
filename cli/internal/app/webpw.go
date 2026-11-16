package app

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"

	"golang.org/x/term"

	"github.com/kewe87/phone-tentacle/cli/internal/daemon"
)

// webPassword is `web password`: ask twice for a new password of the board's web interface (not echoed, not in
// the shell history) and send it over the paired session (sealed). The board keeps it only once someone presses
// its BOOT button within 30 s, as for a change from the page itself.
func (a *App) webPassword(dev string) int {
	in := a.In
	if in == nil {
		in = os.Stdin
	}
	r := bufio.NewReader(in)
	read := func(prompt string) (string, error) {
		fmt.Fprint(a.Err, prompt)
		if f, ok := in.(*os.File); ok && term.IsTerminal(int(f.Fd())) {
			b, err := term.ReadPassword(int(f.Fd()))
			fmt.Fprintln(a.Err)
			return string(b), err
		}
		s, err := r.ReadString('\n')
		if err != nil && s == "" {
			return "", err
		}
		return strings.TrimRight(s, "\r\n"), nil
	}
	pw, err := read("New web password (8-64 characters): ")
	if err != nil {
		return a.fail(err)
	}
	again, err := read("Again: ")
	if err != nil {
		return a.fail(err)
	}
	if pw != again {
		return a.fail(errors.New("the two passwords differ"))
	}
	if n := len([]rune(pw)); n < 8 || n > 64 {
		return a.fail(errors.New("the password needs 8 to 64 characters"))
	}
	fmt.Fprintln(a.Err, "Now press the BOOT button on the board once (within 30 s) to confirm.")
	return a.call(daemon.Request{Cmd: "web", Device: dev, Text: "password\x00" + pw})
}
