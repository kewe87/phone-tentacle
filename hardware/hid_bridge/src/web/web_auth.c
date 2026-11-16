// Password record, login rate limit, token compare: see web_auth.h.
#include "web_auth.h"
#include <string.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

int wa_check_new(const char *pw) {
  size_t n = strlen(pw);
  if (n < WA_PW_MIN) return WA_TOO_SHORT;
  if (n > WA_PW_MAX) return WA_TOO_LONG;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)pw[i] < 0x20 || pw[i] == 0x7f) return WA_BAD_CHAR;
  return WA_OK;
}

static bool derive(const char *pw, const uint8_t *salt, uint32_t iterations, uint8_t out[WA_HASH_LEN]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw), salt, WA_SALT_LEN,
                                       iterations, WA_HASH_LEN, out) == 0;
}

bool wa_make(const char *pw, const uint8_t salt[WA_SALT_LEN], uint32_t iterations, uint8_t rec[WA_REC_LEN]) {
  if (!iterations) return false;
  rec[0] = WA_VERSION;
  for (int i = 0; i < 4; i++) rec[1 + i] = (uint8_t)(iterations >> (8 * i));
  memcpy(rec + 5, salt, WA_SALT_LEN);
  return derive(pw, salt, iterations, rec + 5 + WA_SALT_LEN);
}

bool wa_verify(const char *pw, const uint8_t *rec, size_t rec_len) {
  if (!rec || rec_len != WA_REC_LEN || rec[0] != WA_VERSION) return false;
  uint32_t it = rec[1] | rec[2] << 8 | rec[3] << 16 | (uint32_t)rec[4] << 24;
  if (!it || it > 10000000u) return false;
  uint8_t h[WA_HASH_LEN];
  if (!derive(pw, rec + 5, it, h)) return false;
  uint8_t d = 0;
  for (int i = 0; i < WA_HASH_LEN; i++) d |= h[i] ^ rec[5 + WA_SALT_LEN + i];
  memset(h, 0, sizeof h);
  return d == 0;
}

bool wa_same(const char *a, const char *b) {
  size_t n = strlen(a);
  if (!n || n != strlen(b)) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

void wa_limit_init(wa_limit *l) { memset(l, 0, sizeof *l); }

uint32_t wa_limit_wait(wa_limit *l, uint32_t now) {
  if (!l->locked) return 0;
  int32_t left = (int32_t)(l->until - now);
  if (left <= 0) {
    l->locked = false;  // the lock ran out; the count stays, so the next wrong one locks again (longer)
    return 0;
  }
  return (uint32_t)left;
}

void wa_limit_fail(wa_limit *l, uint32_t now) {
  l->fails++;
  if (l->fails <= WA_FREE_FAILS) return;
  uint32_t k = l->fails - WA_FREE_FAILS - 1, ms = WA_LOCK_MS;
  while (k-- && ms < WA_LOCK_MAX_MS) ms *= 2;
  if (ms > WA_LOCK_MAX_MS) ms = WA_LOCK_MAX_MS;
  l->locked = true;
  l->until = now + ms;
}

void wa_limit_ok(wa_limit *l) { wa_limit_init(l); }
