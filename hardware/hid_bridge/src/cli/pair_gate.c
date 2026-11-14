// Pairing over Wi-Fi: frame layout, code text and the one-request gate, see pair_gate.h.
#include "pair_gate.h"
#include <string.h>

bool pair_parse(const uint8_t *body, size_t n, pair_req *r) {
  const size_t base = PAIR_PUB_LEN + PAIR_ID_LEN;
  memset(r, 0, sizeof *r);
  if (n != base && (n < base + PAIR_COMMIT_LEN || n > base + PAIR_COMMIT_LEN + PAIR_NAME_MAX)) return false;
  r->c_pub = body;
  r->client_id = body + PAIR_PUB_LEN;
  if (n == base) return true;
  r->commit = body + base;
  size_t len = n - base - PAIR_COMMIT_LEN;
  memcpy(r->name, body + base + PAIR_COMMIT_LEN, len);
  r->name[len] = 0;  // a NUL inside the name ends it
  return true;
}

void pair_code_text(const uint8_t mac[4], char out[7]) {
  uint32_t v = ((uint32_t)mac[0] << 24 | (uint32_t)mac[1] << 16 | (uint32_t)mac[2] << 8 | mac[3]) % 1000000u;
  for (int i = 5; i >= 0; i--) {
    out[i] = (char)('0' + v % 10);
    v /= 10;
  }
  out[6] = 0;
}

void pair_show_name(const char *name, char *out, size_t cap) {
  size_t n = 0;
  if (!cap) return;
  for (; name && name[n] && n < cap - 1; n++) out[n] = name[n] >= 0x20 && name[n] < 0x7f ? name[n] : '?';
  out[n] = 0;
  if (!n) {
    strncpy(out, "computer", cap - 1);
    out[cap - 1] = 0;
  }
}

void pg_init(pair_gate *g) { memset(g, 0, sizeof *g); }

uint32_t pg_lock_left(pair_gate *g, uint32_t now) {
  if (!g->locked) return 0;
  uint32_t gone = now - g->lock_at;
  if (gone >= g->lock_ms) {
    g->locked = false;
    g->lock_at += g->lock_ms;  // when it ended: the quiet time towards PAIR_FORGIVE_MS counts from here
    return 0;
  }
  return g->lock_ms - gone;
}

int pg_begin(pair_gate *g, int owner, bool in_use, uint32_t now, uint32_t *wait_ms) {
  if (wait_ms) *wait_ms = 0;
  if (g->state != PG_IDLE) return PG_OPEN;  // also the owner's own second PAIR: the open request stays as it is
  if (in_use) return PG_IN_USE;
  uint32_t left = pg_lock_left(g, now);
  if (left) {
    if (wait_ms) *wait_ms = left;
    return PG_LOCKED;
  }
  if (g->fails && now - g->lock_at >= PAIR_FORGIVE_MS) g->fails = 0;
  g->state = PG_KEYED;
  g->owner = owner;
  g->at = now;
  return PG_OK;
}

bool pg_is(const pair_gate *g, int owner, int state) { return g->state == state && g->owner == owner; }

bool pg_keyed_late(const pair_gate *g, uint32_t now) { return g->state == PG_KEYED && now - g->at >= PAIR_KEYED_MS; }

bool pg_wait(pair_gate *g, int owner, uint32_t now) {
  if (!pg_is(g, owner, PG_KEYED) || pg_keyed_late(g, now)) return false;
  g->state = PG_WAITING;
  g->at = now;
  return true;
}

void pg_end(pair_gate *g, bool paired, uint32_t now) {
  if (g->state == PG_IDLE) return;
  g->state = PG_IDLE;
  if (paired) {
    g->fails = 0;
    g->locked = false;
    return;
  }
  if (g->fails < 8) g->fails++;
  uint32_t ms = PAIR_LOCK_MIN_MS;
  for (int i = 1; i < g->fails && ms < PAIR_LOCK_MAX_MS; i++) ms *= 2;
  g->lock_ms = ms > PAIR_LOCK_MAX_MS ? PAIR_LOCK_MAX_MS : ms;
  g->lock_at = now;
  g->locked = true;
}
