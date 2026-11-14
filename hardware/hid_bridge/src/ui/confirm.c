// Button, confirmation and pairing window state machines, see confirm.h.
#include "confirm.h"
#include <string.h>

void btn_init(btn_state *b) { memset(b, 0, sizeof *b); }

int btn_step(btn_state *b, bool pressed, uint32_t now) {
  if (pressed != b->raw) {
    b->raw = pressed;
    b->raw_at = now;
  }
  if (b->raw != b->down && now - b->raw_at >= BTN_DEBOUNCE_MS) {
    b->down = b->raw;
    if (!b->down) return BTN_NONE;
    b->down_at = b->raw_at;
    b->long_fired = false;
    return BTN_PRESS;
  }
  if (b->down && !b->long_fired && now - b->down_at >= BTN_LONG_MS) {
    b->long_fired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

uint32_t btn_held_ms(const btn_state *b, uint32_t now) { return b->down ? now - b->down_at : 0; }

void cf_init(confirm_slot *c) { memset(c, 0, sizeof *c); }

static void copyLine(char *dst, const char *src) {
  size_t n = src ? strlen(src) : 0;
  if (n >= CF_TEXT) n = CF_TEXT - 1;
  if (n) memcpy(dst, src, n);
  dst[n] = 0;
}

bool cf_request(confirm_slot *c, int kind, const char *l1, const char *l2, uint32_t timeout_ms, uint32_t now) {
  if (kind == CONFIRM_NONE) return false;
  cf_tick(c, now);
  if (c->state == CF_WAITING && c->kind != kind) return false;
  c->kind = kind;
  c->state = CF_WAITING;
  c->start = now;
  c->timeout = timeout_ms;
  copyLine(c->l1, l1);
  copyLine(c->l2, l2);
  return true;
}

void cf_tick(confirm_slot *c, uint32_t now) {
  if (c->state == CF_WAITING && now - c->start >= c->timeout) c->state = CF_EXPIRED;
}

bool cf_press(confirm_slot *c, uint32_t now) {
  cf_tick(c, now);
  if (c->state != CF_WAITING || now - c->start < CF_ARM_MS) return false;
  c->state = CF_ACCEPTED;
  return true;
}

int cf_state(const confirm_slot *c, int kind) { return c->kind == kind ? c->state : CF_IDLE; }

void cf_clear(confirm_slot *c, int kind) {
  if (c->kind != kind) return;
  cf_init(c);
}

uint32_t cf_left_ms(const confirm_slot *c, uint32_t now) {
  if (c->state != CF_WAITING) return 0;
  uint32_t gone = now - c->start;
  return gone >= c->timeout ? 0 : c->timeout - gone;
}

void pw_init(pair_window *w) { memset(w, 0, sizeof *w); }

int pw_press(pair_window *w, bool allowed, uint32_t now) {
  if (w->first && allowed && now - w->first_at <= PW_DOUBLE_MS) {
    w->first = false;  // a third press starts over
    w->open = true;
    w->open_at = now;
    return PW_DOUBLE;
  }
  w->first = true;
  w->first_at = now;
  return PW_SINGLE;
}

void pw_forget(pair_window *w) { w->first = false; }

void pw_tick(pair_window *w, uint32_t now) {
  if (w->first && now - w->first_at > PW_DOUBLE_MS) w->first = false;
  if (w->open && now - w->open_at >= PW_OPEN_MS) w->open = false;
}

bool pw_is_open(const pair_window *w) { return w->open; }

uint32_t pw_left_ms(const pair_window *w, uint32_t now) {
  if (!w->open) return 0;
  uint32_t gone = now - w->open_at;
  return gone >= PW_OPEN_MS ? 0 : PW_OPEN_MS - gone;
}

void pw_close(pair_window *w) { w->open = false; }
