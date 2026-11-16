// GOP buffer and video chunker, see gop.h.
#include "gop.h"
#include <string.h>

void gop_init(gop_buf *g, uint8_t *arena, size_t cap, gop_au *aus, int max) {
  memset(g, 0, sizeof *g);
  g->arena = arena;
  g->cap = cap;
  g->aus = aus;
  g->max = max;
  g->wait_idr = true;
}

void gop_set_config(gop_buf *g, const uint8_t *annexb, size_t len, uint32_t ts) {
  if (len > GOP_CFG_MAX) len = 0;  // never seen in practice; treat as missing
  memcpy(g->cfg, annexb, len);
  g->cfg_len = len;
  g->cfg_ts = ts;
}

static gop_au *slot(const gop_buf *g, uint32_t k) { return &g->aus[k % (uint32_t)g->max]; }

static void drop_oldest(gop_buf *g) {
  g->used -= slot(g, g->first)->len;
  if (g->first++ == 0) g->overflows++;  // the chain lost its start: no new viewer until the next IDR
  g->n--;
}

// Where an AU of len bytes goes without overwriting a kept one. The AUs lie in one piece [head, wr) or, wrapped,
// in [head, end of the arena) and [0, wr); an AU never wraps itself (the end of the arena stays unused then).
// False: the oldest has to go first.
static bool place(const gop_buf *g, size_t len, size_t *pos) {
  if (!g->n) {
    *pos = 0;
    return true;
  }
  if (g->n == g->max) return false;
  const gop_au *w = slot(g, gop_end(g) - 1);
  size_t head = slot(g, g->first)->off, wr = w->off + w->len;
  if (w->off >= head) {
    if (g->cap - wr >= len) *pos = wr;
    else if (head >= len) *pos = 0;  // wraps around
    else return false;
    return true;
  }
  if (head - wr < len) return false;
  *pos = wr;
  return true;
}

static void empty(gop_buf *g) {
  g->n = 0;
  g->first = 0;
  g->used = 0;
  g->total = 0;
}

int gop_push(gop_buf *g, const uint8_t *annexb, size_t len, bool idr, uint32_t ts) {
  if (idr) {
    empty(g);
    g->gen++;
    g->wait_idr = false;
    g->idr_ts = ts;
  } else if (g->wait_idr) {
    return -1;
  }
  if (len > g->cap || g->max < 1) {
    // Nothing after it decodes without it: wait for the next IDR. A new generation, so a stream on this chain stops.
    empty(g);
    g->gen++;
    g->wait_idr = true;
    g->overflows++;
    return -1;
  }
  size_t pos;
  while (!place(g, len, &pos)) drop_oldest(g);
  uint32_t k = gop_end(g);
  memcpy(g->arena + pos, annexb, len);
  *slot(g, k) = (gop_au){(uint32_t)pos, (uint32_t)len, ts, g->total};
  g->used += len;
  g->total += (uint32_t)len;
  g->n++;
  return (int)k;
}

const gop_au *gop_at(const gop_buf *g, uint32_t k) {
  if (k < g->first || k >= gop_end(g)) return NULL;
  return slot(g, k);
}

int gop_resume_at(const gop_buf *g, uint32_t idr_ts, uint32_t n) {
  if (g->wait_idr || !g->n || g->idr_ts != idr_ts || n < 1 || n < g->first || n > gop_end(g)) return -1;
  return (int)n;
}

size_t gop_bytes_from(const gop_buf *g, uint32_t k) {
  if (!g->n || k >= gop_end(g)) return 0;
  if (k <= g->first) return g->used;
  return g->total - slot(g, k)->at;
}

bool annexb_has_idr(const uint8_t *p, size_t len) {
  for (size_t i = 0; i + 4 < len; i++)
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 5) return true;
  return false;
}

bool avcc_to_annexb(uint8_t *p, size_t len) {
  size_t i = 0;
  while (i + 4 <= len) {
    uint32_t n = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 | (uint32_t)p[i + 2] << 8 | p[i + 3];
    if (n > len - i - 4) return false;
    p[i] = p[i + 1] = p[i + 2] = 0;
    p[i + 3] = 1;
    i += 4 + n;
  }
  return i == len;
}

void vchunk_begin(vchunk *c, const uint8_t *data, size_t len, uint8_t flags, uint32_t ts) {
  *c = (vchunk){data, len, 0, ts, (uint8_t)(flags & ~(VF_START | VF_END)), false};
}

size_t vchunk_next(vchunk *c, uint8_t *out, size_t max_payload, uint16_t seq) {
  if (max_payload < 5 || (c->started && c->pos >= c->len)) return 0;
  uint8_t flags = c->flags;
  size_t o = VCHUNK_HDR, room = max_payload;
  if (!c->started) {
    flags |= VF_START;
    for (int i = 0; i < 4; i++) out[o++] = (uint8_t)(c->ts >> (8 * i));
    room -= 4;
    c->started = true;
  }
  size_t n = c->len - c->pos < room ? c->len - c->pos : room;
  memcpy(out + o, c->data + c->pos, n);
  c->pos += n;
  if (c->pos >= c->len) flags |= VF_END;
  out[0] = (uint8_t)seq;
  out[1] = (uint8_t)(seq >> 8);
  out[2] = flags;
  return o + n;
}
