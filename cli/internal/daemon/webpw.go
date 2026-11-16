package daemon

import (
	"context"
	"errors"
	"strings"
)

// web runs `web password`: a new password for the board's web interface, sent over the session (sealed, never
// logged) as `webpw PASSWORD`; the board stores it once its BOOT button confirms it (firmware 0.3.0 or later).
func (s *Server) web(ctx context.Context, c *Conn, text string) Response {
	pw, found := strings.CutPrefix(text, "password\x00")
	if !found {
		return fail(errors.New("usage: tentacle web password"))
	}
	if strings.ContainsAny(pw, "\r\n\x00") {
		return fail(errors.New("the password cannot hold line breaks"))
	}
	reply, err := c.Send(ctx, "webpw "+pw)
	if err != nil {
		if strings.HasPrefix(err.Error(), "unknown command") {
			err = errors.New("this board's firmware has no web interface: update it (0.3.0 or later)")
		}
		return fail(err)
	}
	return ok(map[string]any{"message": "press BOOT on the board within 30 s; the password counts only after that (" +
		strings.TrimSpace(reply) + ")"})
}
