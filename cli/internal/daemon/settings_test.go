package daemon

import (
	"strings"
	"testing"
)

const settingsJSON = `{"display":"auto","oled":1,"abs":"on","abs_active":1,"battery":"none","battery_div":"2.00","lang":"auto","name":"Tentacle","restart":0}`

// `device settings` asks the board for `config` and passes its JSON on.
func TestSettingsShow(t *testing.T) {
	b, c, _ := absBoard(t)
	b.Handle = func(cmd string) string {
		if cmd == "config" {
			return "ok " + settingsJSON
		}
		return "err unknown command"
	}
	r, data := call(t, c, Request{Cmd: "settings"})
	if r.Status != "ok" || data["display"] != "auto" || data["abs"] != "on" || data["oled"] != 1.0 || data["name"] != "Tentacle" {
		t.Fatalf("settings: %+v", r)
	}
	if got := sent(b); got != "config" {
		t.Fatalf("sent %s", got)
	}
}

// `device set KEY VALUE` sends `config KEY VALUE`; a pointer change waits for a restart, and says so.
func TestSettingsSetAbsWantsRestart(t *testing.T) {
	b, c, _ := absBoard(t)
	b.Handle = func(cmd string) string {
		if cmd == "config abs off" {
			return "ok " + strings.NewReplacer(`"abs":"on"`, `"abs":"off"`, `"restart":0`, `"restart":1`).Replace(settingsJSON)
		}
		return "err unknown command"
	}
	r, data := call(t, c, Request{Cmd: "settings", Text: "abs off"})
	if r.Status != "ok" || data["abs"] != "off" || data["abs_active"] != 1.0 || !strings.Contains(data["message"].(string), "restarts") {
		t.Fatalf("set abs: %+v", r)
	}
	if got := sent(b); got != "config abs off" {
		t.Fatalf("sent %s", got)
	}
}

// A name keeps its spaces; the board's own refusal comes back as the error.
func TestSettingsSetNameAndRefusal(t *testing.T) {
	b, c, _ := absBoard(t)
	b.Handle = func(cmd string) string {
		switch cmd {
		case "config name Desk Phone":
			return "ok " + strings.Replace(settingsJSON, `"name":"Tentacle"`, `"name":"Desk Phone"`, 1)
		case "config battery 5":
			return "err bad value for battery: none|GPIO 1-10 except 5 and 6 (the OLED's)"
		}
		return "err unknown command"
	}
	if r, data := call(t, c, Request{Cmd: "settings", Text: "name Desk Phone"}); r.Status != "ok" || data["name"] != "Desk Phone" {
		t.Fatalf("set name: %+v", r)
	}
	if r, _ := call(t, c, Request{Cmd: "settings", Text: "battery 5"}); r.Status != "error" || !strings.Contains(r.Error, "bad value for battery") {
		t.Fatalf("set battery 5: %+v", r)
	}
}

// Unknown keys and a missing value are refused before anything is sent; old firmware gets a hint.
func TestSettingsUsageAndOldFirmware(t *testing.T) {
	b, c, _ := absBoard(t)
	b.Handle = func(string) string { return "err unknown command" }
	if r, _ := call(t, c, Request{Cmd: "settings", Text: "colour red"}); r.Status != "error" || !strings.Contains(r.Error, "unknown setting") {
		t.Fatalf("unknown key: %+v", r)
	}
	if r, _ := call(t, c, Request{Cmd: "settings", Text: "display"}); r.Status != "error" || !strings.Contains(r.Error, "usage") {
		t.Fatalf("no value: %+v", r)
	}
	if len(b.Commands) != 0 {
		t.Fatalf("sent %s", sent(b))
	}
	if r, _ := call(t, c, Request{Cmd: "settings"}); r.Status != "error" || !strings.Contains(r.Error, "0.4.0") {
		t.Fatalf("old firmware: %+v", r)
	}
}
