package app

import (
	"bytes"
	"strings"
	"testing"
)

// --help prints the command's usage, never a JSON error ("flag: help requested").
func TestHelpPrintsUsage(t *testing.T) {
	for _, args := range [][]string{{"pair", "--help"}, {"io", "tap", "-h"}, {"screenshot", "-help"}} {
		var out, errOut bytes.Buffer
		a := &App{Out: &out, Err: &errOut}
		if rc := a.Run(args); rc != 0 || out.Len() != 0 || !strings.Contains(errOut.String(), "  "+args[0]+" ") {
			t.Fatalf("%v: rc %d, out %q, err %q", args, rc, out.String(), errOut.String())
		}
	}
}
