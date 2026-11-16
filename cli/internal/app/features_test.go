package app

import (
	"bytes"
	"image"
	"image/jpeg"
	"io"
	"path/filepath"
	"strings"
	"testing"

	"github.com/kewe87/phone-tentacle/cli/internal/history"
)

func jpegOf(w, h int) []byte {
	var buf bytes.Buffer
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i := range img.Pix {
		img.Pix[i] = 255
	}
	jpeg.Encode(&buf, img, nil)
	return buf.Bytes()
}

// withIntent: a config dir with one run (screen, tap, type) saved as intent "Alarm".
func withIntent(t *testing.T) *App {
	a := &App{Err: io.Discard, Dir: t.TempDir()}
	r := &history.Recorder{Dir: history.Root(a.Dir)}
	r.Write("llm", map[string]any{"role": "screenshot"}, jpegOf(960, 2080))
	r.Write("step", map[string]any{"action": "tap", "x": 500, "y": 814}, nil)
	r.Write("step", map[string]any{"action": "type", "text": "secret 192.168.4.1"}, nil)
	if o := run(t, a, "intent", "save", "--name", "Alarm", "--prompt", "Set an alarm on 10.0.0.1", "--hint", "Type the time",
		"--from-run", "latest"); o.Status != "ok" {
		t.Fatalf("save: %+v", o)
	}
	return a
}

func TestHistoryAndIntentCommands(t *testing.T) {
	a := withIntent(t)
	if o := run(t, a, "history", "list"); o.Status != "ok" || len(o.Data["runs"].([]any)) != 1 {
		t.Fatalf("list: %+v", o)
	}
	if o := run(t, a, "history", "show", "nope"); o.Status != "error" {
		t.Fatalf("show nope: %+v", o)
	}
	if o := run(t, a, "history", "new"); o.Status != "ok" {
		t.Fatalf("new: %+v", o)
	}
	o := run(t, a, "intent", "export", "Alarm", "--output", filepath.Join(a.Dir, "a.tentacle-intent"))
	if o.Status != "ok" || o.Data["screenshots"] != false {
		t.Fatalf("export: %+v", o)
	}
	if o := run(t, a, "intent", "import", filepath.Join(a.Dir, "a.tentacle-intent")); o.Status != "ok" || o.Data["skipped"] != 1.0 {
		t.Fatalf("import of a saved one: %+v", o)
	}

	// the MCP tools work without a daemon
	var buf bytes.Buffer
	a.Out = &buf
	a.mcp(strings.NewReader(`{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"intent_show","arguments":{"name":"alarm"}}}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"history","arguments":{"action":"list"}}}
{"jsonrpc":"2.0","id":3,"method":"tools/list"}
`))
	out := buf.String()
	if !strings.Contains(out, `Set an alarm on 10.0.0.1`) || !strings.Contains(out, `20`) || strings.Contains(out, `"isError":true`) ||
		!strings.Contains(out, `"intent_save"`) || strings.Contains(out, "report") || strings.Contains(out, "export") {
		t.Fatalf("mcp:\n%s", out)
	}
}
