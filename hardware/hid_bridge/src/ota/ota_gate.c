// Firmware update state machine, see ota_gate.h.
#include "ota_gate.h"
#include <string.h>

void og_init(ota_gate *g) { memset(g, 0, sizeof *g); }

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void og_clean_version(const char *in, char out[OTA_VERSION_MAX]) {
  size_t n = 0;
  for (; in && in[n] && n < OTA_VERSION_MAX - 1; n++) out[n] = in[n] > ' ' && in[n] < 0x7f ? in[n] : '?';
  out[n] = 0;
}

bool og_parse_begin(const char *arg, ota_begin_req *r) {
  memset(r, 0, sizeof *r);
  const char *p = arg;
  if (!p || *p < '0' || *p > '9') return false;
  uint64_t size = 0;
  for (; *p >= '0' && *p <= '9'; p++) {
    size = size * 10 + (uint64_t)(*p - '0');
    if (size > 0xffffffffu) return false;
  }
  if (*p++ != ' ' || !size) return false;
  for (int i = 0; i < 32; i++) {
    int hi = hexval(p[2 * i]), lo = hi < 0 ? -1 : hexval(p[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    r->sha[i] = (uint8_t)(hi << 4 | lo);
  }
  p += 64;
  if (*p++ != ' ') return false;
  if (*p == '-') {
    p++;
  } else {
    for (int i = 0; i < 64; i++) {
      int hi = hexval(p[2 * i]), lo = hi < 0 ? -1 : hexval(p[2 * i + 1]);
      if (hi < 0 || lo < 0) return false;
      r->sig[i] = (uint8_t)(hi << 4 | lo);
    }
    p += 128;
    r->has_sig = true;
  }
  if (*p == ' ') og_clean_version(p + 1, r->version);
  else if (*p) return false;
  r->size = (uint32_t)size;
  return true;
}

const char *og_begin(ota_gate *g, const ota_begin_req *r, uint32_t max_size, uint32_t now) {
  if (og_busy(g)) return "busy: an update is already running";
  if (g->state == OG_DONE) return "busy: the board is restarting into the new firmware";
  if (!r->size) return "empty image";
  if (r->size > max_size) return "image too big for the update partition";
  og_init(g);
  g->state = OG_RECEIVING;
  g->size = r->size;
  g->since = now;
  memcpy(g->sha, r->sha, sizeof g->sha);
  memcpy(g->version, r->version, sizeof g->version);
  return NULL;
}

static void setWhy(ota_gate *g, const char *why) {
  size_t n = strlen(why);
  if (n >= sizeof g->why) n = sizeof g->why - 1;
  memcpy(g->why, why, n);
  g->why[n] = 0;
}

void og_fail(ota_gate *g, const char *why) {
  og_init(g);
  setWhy(g, why ? why : "failed");
}

bool og_chunk(ota_gate *g, uint32_t off, size_t n, uint32_t now) {
  if (g->state != OG_RECEIVING) return false;
  if (!n || n > OTA_CHUNK_MAX) { og_fail(g, "bad chunk size"); return false; }
  if (off != g->got) { og_fail(g, "chunk out of order (lost or repeated)"); return false; }
  if ((uint64_t)off + n > g->size) { og_fail(g, "more data than the announced size"); return false; }
  g->since = now;
  return true;
}

void og_written(ota_gate *g, size_t n) {
  if (g->state == OG_RECEIVING) g->got += (uint32_t)n;
}

bool og_complete(const ota_gate *g) { return g->state == OG_RECEIVING && g->got == g->size; }

bool og_stalled(const ota_gate *g, uint32_t now) { return g->state == OG_RECEIVING && now - g->since >= OTA_STALL_MS; }

void og_done(ota_gate *g) { g->state = OG_DONE; }

bool og_busy(const ota_gate *g) { return g->state == OG_RECEIVING; }
