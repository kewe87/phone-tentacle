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

void wv_init(wv_gate *g) { memset(g, 0, sizeof *g); }

bool wv_send(wv_gate *g, uint32_t gen, uint32_t ts, bool catching_up, uint32_t now, uint32_t max_lag) {
  if (g->skipping) {
    if (gen == g->skip_gen) return false;
    g->skipping = false;  // a new GOP: start over from its IDR
  }
  if (catching_up || !max_lag || (int32_t)(now - ts) <= (int32_t)max_lag) return true;
  g->skipping = true;
  g->skip_gen = gen;
  g->since = now;
  return false;
}

bool wv_waiting(wv_gate *g, uint32_t gen) {
  if (g->skipping && gen != g->skip_gen) g->skipping = false;
  return g->skipping;
}

uint32_t wv_waited(const wv_gate *g, uint32_t now) { return g->skipping ? now - g->since : 0; }
