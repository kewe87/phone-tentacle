// The web interface's password and login rules (web_server.cpp), as plain C so they can be tested on the host:
// the stored password record (salted PBKDF2-HMAC-SHA256), the login rate limit with its lockout, and the session
// token compare. Times are millis(); they may wrap.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Password record, as stored in NVS: version (1), iterations (u32 LE), salt, PBKDF2-HMAC-SHA256(password).
#define WA_SALT_LEN 16
#define WA_HASH_LEN 32
#define WA_REC_LEN (1 + 4 + WA_SALT_LEN + WA_HASH_LEN)
#define WA_VERSION 1
#ifndef WA_ITERATIONS
#define WA_ITERATIONS 10000  // ~0.2 s on the ESP32-S3 (SHA in hardware): slow for guessing, quick enough to log in
#endif
#define WA_PW_MIN 8
#define WA_PW_MAX 64

enum { WA_OK = 0, WA_TOO_SHORT, WA_TOO_LONG, WA_BAD_CHAR };
// A new password: WA_PW_MIN..WA_PW_MAX bytes, no control characters (UTF-8 above 0x7f is fine).
int wa_check_new(const char *pw);
// Builds the record for pw with this salt (random, from the caller) and iteration count. False: mbedtls failed.
bool wa_make(const char *pw, const uint8_t salt[WA_SALT_LEN], uint32_t iterations, uint8_t rec[WA_REC_LEN]);
// True if pw matches the record (constant-time compare). A record of the wrong size or version never matches.
bool wa_verify(const char *pw, const uint8_t *rec, size_t rec_len);
// Constant-time compare of two NUL-terminated strings of the same length (session tokens).
bool wa_same(const char *a, const char *b);

// ---- Login rate limit, one for the whole board (a guesser that changes its address gains nothing): the first
// WA_FREE_FAILS wrong passwords cost nothing, then each one locks logins for WA_LOCK_MS, doubled with every further
// one up to WA_LOCK_MAX_MS. A right password clears it.
#define WA_FREE_FAILS 5
#define WA_LOCK_MS 30000u
#define WA_LOCK_MAX_MS (15u * 60u * 1000u)

typedef struct {
  uint32_t fails;       // wrong passwords in a row
  uint32_t until;       // locked until then (when locked)
  bool locked;
} wa_limit;

void wa_limit_init(wa_limit *l);
uint32_t wa_limit_wait(wa_limit *l, uint32_t now);  // 0: a login may be tried now; else ms until it may
void wa_limit_fail(wa_limit *l, uint32_t now);      // a wrong password
void wa_limit_ok(wa_limit *l);                      // a right one

#ifdef __cplusplus
}
#endif
