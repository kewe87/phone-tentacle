// The web interface's own TLS certificate (web_server.cpp): an ECDSA P-256 key and a self-signed certificate for
// it, made once on the board and kept in NVS. Plain C on mbedtls for the host tests.
//
// The browser warns about it the first time (nobody vouches for it); the page's settings show its SHA-256
// fingerprint to compare with the browser's. Names: the mDNS name (tentacle.local) and the board's address when it
// was made. Valid from the later of 2026-01-01 and the firmware's build date for WC_DAYS days (Apple takes no
// server certificate valid longer than 825 days); the board makes a new one when a firmware's build date comes
// within WC_RENEW_DAYS of its end (the board has no clock of its own).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC_DAYS 824
#define WC_RENEW_DAYS 60
#define WC_KEY_PEM_MAX 512
#define WC_CRT_PEM_MAX 1536
#define WC_FP_LEN 96  // "AB:CD:...": 32 bytes as hex pairs with colons, NUL

typedef int (*wc_rng)(void *ctx, unsigned char *out, size_t len);  // 0 on success (mbedtls' f_rng)

// Days since 1970-01-01 of a calendar date, and back (proleptic Gregorian).
int32_t wc_days(int y, int m, int d);
void wc_date(int32_t days, int *y, int *m, int *d);
// The build date from __DATE__ ("Nov 16 2026"); -1 if it does not parse.
int32_t wc_build_days(const char *date);

// Makes the key and the certificate as PEM (NUL-terminated). dns: the host name; ipv4: an address in network
// order (as lwIP keeps it; 0: none); serial: 16 random bytes. 0 on success, else an mbedtls error.
int wc_make(const char *dns, uint32_t ipv4, int32_t from_days, wc_rng rng, void *rng_ctx, char key_pem[WC_KEY_PEM_MAX],
            char crt_pem[WC_CRT_PEM_MAX]);
// A stored certificate: its SHA-256 fingerprint and the day it ends. false if it does not parse.
bool wc_info(const char *crt_pem, char fp[WC_FP_LEN], int32_t *until_days);
// Whether a stored certificate does for a firmware built on build_days (it parses, and it ends well after).
bool wc_good(const char *crt_pem, int32_t build_days);

#ifdef __cplusplus
}
#endif
