// Control between the session and the web interface, and the web video's lag gate: see web_ctl.h.
#include "web_ctl.h"
#include <string.h>

void wc_init(wc_state *c) { memset(c, 0, sizeof *c); }

void wc_connect(wc_state *c, uint32_t now) {
  c->connected = true;
  c->control = false;
  c->watching = false;
  c->web_at = now;
}

void wc_disconnect(wc_state *c) {
  c->connected = false;
  c->control = false;
  c->watching = false;
}

void wc_take(wc_state *c, uint32_t now) {
  if (!c->connected) return;
  c->control = true;
  c->watching = false;
  c->web_at = now;
}

void wc_release(wc_state *c) {
  c->control = false;
  c->watching = c->connected;
}

void wc_web_input(wc_state *c, uint32_t now) {
  if (c->control) c->web_at = now;
}

void wc_cli_input(wc_state *c, uint32_t now) {
  c->cli_at = now;
  c->cli_seen = true;
}

bool wc_cli_active(const wc_state *c, bool cli_up, uint32_t now) {
  return cli_up && c->cli_seen && now - c->cli_at < WC_CLI_IDLE_MS;
}

bool wc_tick(wc_state *c, bool cli_up, uint32_t now) {
  bool was = c->control;
  if (!c->connected) c->control = false;
  else if (c->control && now - c->web_at >= WC_WEB_IDLE_MS) {
    c->control = false;
    c->watching = true;
  } else if (!c->control && !c->watching && !wc_cli_active(c, cli_up, now)) {
    c->control = true;
    c->web_at = now;
  }
  return was != c->control;
}

void wv_init(wv_watch *w) { memset(w, 0, sizeof *w); }

int wv_update(wv_watch *w, bool following, bool wait_idr, bool whole, uint32_t now) {
  int s = following || (!wait_idr && whole) ? VW_OK : wait_idr ? VW_KEYFRAME : VW_FROZEN;
  if (s != w->state) {
    w->state = s;
    w->since = now;
  }
  return s;
}

uint32_t wv_waited(const wv_watch *w, uint32_t now) { return w->state == VW_OK ? 0 : now - w->since; }
