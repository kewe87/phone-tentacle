package app

import (
	"bytes"
	"strings"
	"testing"
)

// --help prints the command's usage, never a JSON error ("flag: help requested").
func TestHelpPrintsUsage(t *testing.T) {
	for _, args := range [][]string{{"pair", "--help"}, {"io", "tap", "-h"}, {"screenshot", "-help"}, {"help", "pair"}} {
		var out, errOut bytes.Buffer
		a := &App{Out: &out, Err: &errOut}
		cmd := args[0]
		if cmd == "help" {
			cmd = args[1]
		}
		if rc := a.Run(args); rc != 0 || out.Len() != 0 || !strings.Contains(errOut.String(), "  "+cmd+" ") {
			t.Fatalf("%v: rc %d, out %q, err %q", args, rc, out.String(), errOut.String())
		}
	}
}

// Every command answers -h and --help with its usage on stderr and exit 0, without reaching the daemon (no Client).
func TestEveryCommandHasHelp(t *testing.T) {
	cmds := []string{"version", "daemon", "mcp", "history", "intent", "intents", "memory", "logs", "update", "debug",
		"keygen", "sign", "skill", "pair", "unpair", "devices", "calibrate", "device", "net", "web", "listen",
		"keyboard", "video", "screenshot", "screencapture", "io"}
	for _, cmd := range cmds {
		for _, flag := range []string{"-h", "--help"} {
			var out, errOut bytes.Buffer
			a := &App{Out: &out, Err: &errOut}
			name := cmd
			if name == "intents" {
				name = "intent"
			}
			if rc := a.Run([]string{cmd, flag}); rc != 0 || out.Len() != 0 || !strings.Contains(errOut.String(), name) {
				t.Errorf("%s %s: rc %d, out %q, err %q", cmd, flag, rc, out.String(), errOut.String())
			}
		}
	}
}
