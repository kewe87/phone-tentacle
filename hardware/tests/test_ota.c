// Host tests for the firmware update (src/ota/): `update begin`'s arguments, the order and size of the image's
// chunks, the stall deadline, and which images the board takes (Ed25519 signature with the release key; unsigned
// ones only in a debug build).
// Alone: gcc -DTEST_OTA_MAIN -o test_ota test_ota.c ../hid_bridge/src/ota/ota_gate.c ../hid_bridge/src/ota/ota_sig.c -lsodium && ./test_ota
#include <stdio.h>
#include <string.h>
#include <sodium.h>
#include "../hid_bridge/src/ota/ota_gate.h"
#include "../hid_bridge/src/ota/ota_sig.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static const char *SHA = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static void test_parse(void) {
  ota_begin_req r;
  char line[400];
  snprintf(line, sizeof line, "1400000 %s - 0.2.0", SHA);
  CHECK(og_parse_begin(line, &r) && r.size == 1400000 && !r.has_sig && r.sha[31] == 0x1f && !strcmp(r.version, "0.2.0"));
  snprintf(line, sizeof line, "5 %s -", SHA);
  CHECK(og_parse_begin(line, &r) && r.size == 5 && !r.version[0]);
  char sig[129];
  uint8_t s[64];
  for (int i = 0; i < 64; i++) s[i] = (uint8_t)(0xA0 + i);
  hex(s, 64, sig);
  snprintf(line, sizeof line, "7 %s %s 1.0", SHA, sig);
  CHECK(og_parse_begin(line, &r) && r.has_sig && !memcmp(r.sig, s, 64) && !strcmp(r.version, "1.0"));
  snprintf(line, sizeof line, "7 %s %.127s", SHA, sig);  // a signature one digit short
  CHECK(!og_parse_begin(line, &r));
  snprintf(line, sizeof line, "7 %s", SHA);  // no signature field
  CHECK(!og_parse_begin(line, &r));
  snprintf(line, sizeof line, "7 %.63s -", SHA);
  CHECK(!og_parse_begin(line, &r));
  snprintf(line, sizeof line, "0 %s -", SHA);
  CHECK(!og_parse_begin(line, &r));
  snprintf(line, sizeof line, "99999999999 %s -", SHA);
  CHECK(!og_parse_begin(line, &r));
  CHECK(!og_parse_begin("", &r) && !og_parse_begin("x", &r));
  snprintf(line, sizeof line, "7 %s - a\x01" "b", SHA);
  CHECK(og_parse_begin(line, &r) && !strcmp(r.version, "a?b"));
}

static void test_gate(void) {
  ota_gate g;
  ota_begin_req r;
  char line[200];
  snprintf(line, sizeof line, "10000 %s - 0.2.0", SHA);
  og_parse_begin(line, &r);
  og_init(&g);
  CHECK(!og_chunk(&g, 0, 100, 0));            // nothing is being received
  CHECK(og_begin(&g, &r, 5000, 0) != NULL);   // too big for the slot
  CHECK(og_begin(&g, &r, 1 << 20, 0) == NULL && og_busy(&g));
  CHECK(og_begin(&g, &r, 1 << 20, 0) != NULL);  // one at a time
  CHECK(og_chunk(&g, 0, OTA_CHUNK_MAX, 1));
  og_written(&g, OTA_CHUNK_MAX);
  CHECK(og_chunk(&g, 4096, 4096, 2));
  og_written(&g, 4096);
  CHECK(!og_complete(&g));
  CHECK(!og_stalled(&g, 2 + OTA_STALL_MS - 1) && og_stalled(&g, 2 + OTA_STALL_MS));
  CHECK(og_chunk(&g, 8192, 10000 - 8192, 3));
  og_written(&g, 10000 - 8192);
  CHECK(og_complete(&g));
  og_done(&g);
  CHECK(!og_busy(&g) && og_begin(&g, &r, 1 << 20, 4) != NULL);  // restarting

  og_init(&g);
  og_begin(&g, &r, 1 << 20, 0);
  CHECK(!og_chunk(&g, 4096, 100, 1) && !og_busy(&g) && strstr(g.why, "order"));  // a lost chunk
  CHECK(og_begin(&g, &r, 1 << 20, 0) == NULL && !g.why[0]);  // a new begin after a failure
  CHECK(!og_chunk(&g, 0, OTA_CHUNK_MAX + 1, 1) && strstr(g.why, "size"));
  og_begin(&g, &r, 1 << 20, 0);
  CHECK(og_chunk(&g, 0, 4096, 1));
  og_written(&g, 4096);
  CHECK(!og_chunk(&g, 0, 4096, 1) && strstr(g.why, "order"));  // repeated
  og_begin(&g, &r, 1 << 20, 0);
  for (uint32_t off = 0; off < 8192; off += 4096) {
    og_chunk(&g, off, 4096, 1);
    og_written(&g, 4096);
  }
  CHECK(!og_chunk(&g, 8192, 4096, 1) && strstr(g.why, "more data"));  // past the announced size
  og_begin(&g, &r, 1 << 20, 0);
  og_fail(&g, "the session ended");
  CHECK(!og_busy(&g) && !strcmp(g.why, "the session ended"));
}

static void test_signature(void) {
  CHECK(sodium_init() >= 0);
  uint8_t pk[32], sk[64], pk2[32], sk2[64];
  crypto_sign_keypair(pk, sk);
  crypto_sign_keypair(pk2, sk2);
  const char image[] = "pretend this is a firmware image";
  uint8_t sha[32], msg[OTA_SIG_MSG_LEN];
  crypto_hash_sha256(sha, (const uint8_t *)image, sizeof image);
  ota_sig_message(sha, msg);
  CHECK(!memcmp(msg, "tentacle-fw-v1", 14) && !memcmp(msg + 14, sha, 32));

  ota_begin_req r;
  memset(&r, 0, sizeof r);
  r.size = sizeof image;
  memcpy(r.sha, sha, 32);
  r.has_sig = true;
  crypto_sign_detached(r.sig, NULL, msg, sizeof msg, sk);
  // good signature: taken by a release and a debug build
  CHECK(ota_sig_check(&r, pk, false) == NULL);
  CHECK(ota_sig_check(&r, pk, true) == NULL);
  // signed with another key: refused, also by a debug build
  CHECK(ota_sig_check(&r, pk2, false) != NULL);
  CHECK(ota_sig_check(&r, pk2, true) != NULL);
  // a bad signature (one bit) or another image under a good signature
  r.sig[10] ^= 1;
  CHECK(ota_sig_check(&r, pk, false) != NULL && ota_sig_check(&r, pk, true) != NULL);
  r.sig[10] ^= 1;
  r.sha[0] ^= 1;
  CHECK(ota_sig_check(&r, pk, false) != NULL);
  r.sha[0] ^= 1;
  // the label is part of what is signed: a signature over the bare SHA-256 does not count
  crypto_sign_detached(r.sig, NULL, sha, 32, sk);
  CHECK(ota_sig_check(&r, pk, false) != NULL);
  // unsigned: only a debug build takes it
  r.has_sig = false;
  const char *why = ota_sig_check(&r, pk, false);
  CHECK(why && strstr(why, "unsigned"));
  CHECK(ota_sig_check(&r, pk, true) == NULL);
}

int test_ota(void) {
  test_parse();
  test_gate();
  test_signature();
  return fails;
}

#ifdef TEST_OTA_MAIN
int main(void) {
  test_ota();
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
#endif
