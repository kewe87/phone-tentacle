package daemon

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

// net: the board's Wi-Fi. arg: "" (show), "join SSID\x00PASS", "hotspot". docs/cli-protocol.md, "Wi-Fi link".
func (s *Server) net(ctx context.Context, c *Conn, arg string) Response {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	verb, rest, _ := strings.Cut(strings.TrimSpace(arg), " ")
	switch verb {
	case "":
	case "join", "hotspot":
		var req map[string]string
		if verb == "hotspot" {
			req = map[string]string{"mode": "ap"}
		} else {
			ssid, pass, _ := strings.Cut(rest, "\x00")
			if ssid == "" {
				return fail(errors.New("usage: net join SSID [PASSWORD]"))
			}
			req = map[string]string{"mode": "sta", "ssid": ssid, "pass": pass}
		}
		b, _ := json.Marshal(req)
		if _, err := c.Send(ctx, "net "+string(b)); err != nil {
			return fail(err)
		}
		// The board answered, and now leaves the network this session runs on. It is found again (mDNS) on the
		// new one by the next request; its old address is no use.
		return ok(map[string]any{"sent": verb, "message": "sent; the board changes its Wi-Fi now and this link drops"})
	default:
		return fail(errors.New("usage: net [join SSID [PASSWORD] | hotspot]"))
	}
	reply, err := c.Send(ctx, "net")
	if err != nil {
		return fail(err)
	}
	var board map[string]any
	if json.Unmarshal([]byte(reply), &board) != nil {
		return fail(errors.New("unexpected answer from the board to net: " + reply))
	}
	if board["mode"] == "sta" {
		delete(board, "pass") // your Wi-Fi's password stays out of logs; the hotspot's is needed to join it
	}
	return ok(map[string]any{"board": board})
}
