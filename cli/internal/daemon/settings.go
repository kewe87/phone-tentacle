package daemon

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// settingKeys are the board's settings (`config`, docs/cli-protocol.md), in the order `device settings` shows them.
var settingKeys = []string{"name", "display", "abs", "battery", "battery_div", "lang", "sdlog"}

// settings shows the board's settings (text "") or stores one ("KEY VALUE"): the status OLED, the pointer, the
// battery pin and its divider, the OLED's language, the name. Firmware 0.4.0 or later.
func (s *Server) settings(ctx context.Context, c *Conn, text string) Response {
	line := "config"
	key, value, set := strings.Cut(strings.TrimSpace(text), " ")
	if key != "" {
		value = strings.TrimSpace(value)
		if !set || value == "" {
			return fail(fmt.Errorf("usage: tentacle device set %s VALUE", key))
		}
		known := false
		for _, k := range settingKeys {
			known = known || k == key
		}
		if !known {
			return fail(fmt.Errorf("unknown setting %q (known: %s)", key, strings.Join(settingKeys, ", ")))
		}
		line += " " + key + " " + value
	}
	reply, err := c.Send(ctx, line)
	if err != nil {
		if strings.HasPrefix(err.Error(), "unknown command") {
			err = errors.New("this board's firmware has no settings: update it (0.4.0 or later)")
		}
		return fail(err)
	}
	out := map[string]any{}
	if err := json.Unmarshal([]byte(strings.TrimSpace(reply)), &out); err != nil {
		return fail(fmt.Errorf("the board's settings: %v", err))
	}
	if num(out["restart"]) == 1 {
		out["message"] = "a changed name or pointer (abs) takes effect when the board restarts (unplug it, or the web page's restart)"
	} else if key == "abs" {
		out["message"] = "the pointer is already as set"
	}
	if key == "sdlog" && out["sdlog"] != "off" {
		switch out["sd"] {
		case "none":
			out["message"] = "this board has no microSD slot"
		case "nocard":
			out["message"] = "no card in the slot (or not FAT32)"
		case "err":
			out["message"] = "the card cannot be written"
		}
	}
	if key == "display" && out["display"] != "off" && num(out["oled"]) != 1 {
		out["message"] = "no display answers on I2C (GPIO5 SDA, GPIO6 SCL at 0x3C/0x3D)"
	}
	return ok(out)
}
