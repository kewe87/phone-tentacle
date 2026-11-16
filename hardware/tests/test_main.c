// Host unit tests for the board's pure-C parts: bplist codec, the GOP buffer / video chunker, and the
// mirroring keystream, the AirPlay audio packets and sound gate, the CLI link crypto, the RAM log ring, mouse move pacing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../hid_bridge/src/bplist/bplist.h"
#include "../hid_bridge/src/video/gop.h"
#include "../hid_bridge/src/airplay/keystream.h"
#include "../hid_bridge/src/airplay/audio.h"
#include "mbedtls/aes.h"
#include "../hid_bridge/src/cli/cli_proto.h"
#include "../hid_bridge/src/hid/pace.h"
#include "../hid_bridge/src/log/logring.h"
#include "mdns.h"  // stubs/: the host stand-in for ESP-IDF mdns
#include "../hid_bridge/src/airplay/ap_internal.h"

static int fails;
int test_setup(void);  // test_setup.c: the Wi-Fi setup page's text helpers, the BOOT button and its confirmation
int test_pair(void);   // test_pair.c: the PAIR frame, the pairing code's text, the one-request gate
int test_ota(void);    // test_ota.c: `update begin`, the chunks' order, the firmware signature
int test_kbd(void);    // test_kbd.c: the keyboard layouts (US, German), dead keys, what a layout cannot type
int test_web(void);    // test_web.c: the web interface: password, login lock, input, control, video gate, certificate
int test_opts(void);   // test_opts.c: the board's hardware settings, the OLED's texts in English and German
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_bplist_roundtrip(void) {
  bp_writer w;
  bp_writer_init(&w);
  int root = bp_w_dict(&w);
  bp_w_set(&w, root, "features", bp_w_uint(&w, 0x5A7FFEE6ull));
  bp_w_set(&w, root, "big", bp_w_uint(&w, 0x123456789ull));
  bp_w_set(&w, root, "name", bp_w_string(&w, "Tentacle with a long name"));
  bp_w_set(&w, root, "ok", bp_w_bool(&w, true));
  bp_w_set(&w, root, "rate", bp_w_real(&w, 1.0 / 60));
  uint8_t pk[32];
  for (int i = 0; i < 32; i++) pk[i] = (uint8_t)i;
  bp_w_set(&w, root, "pk", bp_w_data(&w, pk, 32));
  int arr = bp_w_array(&w);
  bp_w_set(&w, root, "displays", arr);
  int d0 = bp_w_dict(&w);
  bp_w_append(&w, arr, d0);
  bp_w_set(&w, d0, "width", bp_w_uint(&w, 480));
  size_t len = 0;
  uint8_t *buf = bp_w_finish(&w, &len);
  bp_writer_free(&w);
  CHECK(buf != NULL);

  bp_reader r;
  int top = bp_open(&r, buf, len);
  CHECK(top >= 0 && bp_type(&r, top) == BP_DICT && bp_count(&r, top) == 7);
  uint64_t u = 0;
  CHECK(bp_get_uint(&r, bp_dict_get(&r, top, "features"), &u) && u == 0x5A7FFEE6ull);
  CHECK(bp_get_uint(&r, bp_dict_get(&r, top, "big"), &u) && u == 0x123456789ull);
  char s[64];
  CHECK(bp_get_string(&r, bp_dict_get(&r, top, "name"), s, sizeof s) && !strcmp(s, "Tentacle with a long name"));
  bool b = false;
  CHECK(bp_get_bool(&r, bp_dict_get(&r, top, "ok"), &b) && b);
  double d = 0;
  CHECK(bp_get_real(&r, bp_dict_get(&r, top, "rate"), &d) && d == 1.0 / 60);
  const uint8_t *p;
  size_t n;
  CHECK(bp_get_data(&r, bp_dict_get(&r, top, "pk"), &p, &n) && n == 32 && !memcmp(p, pk, 32));
  int a = bp_dict_get(&r, top, "displays");
  CHECK(bp_type(&r, a) == BP_ARRAY && bp_count(&r, a) == 1);
  CHECK(bp_get_uint(&r, bp_dict_get(&r, bp_array_get(&r, a, 0), "width"), &u) && u == 480);
  CHECK(bp_dict_get(&r, top, "missing") < 0);
  CHECK(bp_array_get(&r, a, 5) < 0);

  // truncation and garbage never crash
  for (size_t cut = 0; cut < len; cut++) {
    bp_reader t;
    int tt = bp_open(&t, buf, cut);
    if (tt >= 0) bp_dict_get(&t, tt, "pk");
  }
  uint8_t junk[200];
  memcpy(junk, buf, len < 200 ? len : 200);
  for (int i = 8; i < 200 && (size_t)i < len - 32; i += 3) junk[i] ^= 0xA5;
  bp_reader t;
  int tt = bp_open(&t, junk, len < 200 ? len : 200);
  if (tt >= 0) { bp_dict_get(&t, tt, "pk"); bp_count(&t, tt); }
  free(buf);
}

// Hand-made plist as Apple writes it: UTF-16 string value and 1-byte refs.
static void test_bplist_utf16(void) {
  static const uint8_t pl[] = {
    'b','p','l','i','s','t','0','0',
    0xD1, 0x01, 0x02,                // dict {1: 2}         @8
    0x54, 'n','a','m','e',           // "name"              @11
    0x63, 0x00,'H', 0x20,0x19, 0x00,'s',  // u"H’s"        @16
    8, 11, 16,                       // offsets            @23
    0,0,0,0,0,0, 1, 1,
    0,0,0,0,0,0,0,3, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,23};
  bp_reader r;
  int top = bp_open(&r, pl, sizeof pl);
  char s[32];
  CHECK(top == 0);
  CHECK(bp_get_string(&r, bp_dict_get(&r, top, "name"), s, sizeof s) && !strcmp(s, "H\xE2\x80\x99s"));
}

// Every kept AU of the chain still holds its own bytes (each AU k was filled with k & 0xff), and used adds up.
static bool gop_intact(const gop_buf *g) {
  size_t used = 0;
  for (uint32_t k = g->first; k < gop_end(g); k++) {
    const gop_au *a = gop_at(g, k);
    if (!a || a->off + a->len > g->cap) return false;
    for (uint32_t i = 0; i < a->len; i++)
      if (g->arena[a->off + i] != (uint8_t)k) return false;
    used += a->len;
  }
  return used == g->used;
}

static int push_k(gop_buf *g, size_t len, bool idr, uint32_t ts) {
  static uint8_t au[128];
  memset(au, (uint8_t)gop_end(g) * !idr, sizeof au);
  return gop_push(g, au, len, idr, ts);
}

static void test_gop(void) {
  static uint8_t arena[100];
  static gop_au aus[8];
  gop_buf g;
  gop_init(&g, arena, sizeof arena, aus, 4);
  CHECK(push_k(&g, 10, false, 1) == -1 && !gop_whole(&g));  // waits for the first IDR
  CHECK(push_k(&g, 30, true, 2) == 0);
  CHECK(push_k(&g, 30, false, 3) == 1);
  CHECK(push_k(&g, 30, false, 4) == 2 && g.used == 90 && gop_whole(&g) && gop_end(&g) == 3);
  // a stream whose decoder has the first n AUs of this chain (IDR stamped 2) goes on at n; another chain's: from scratch
  CHECK(gop_resume_at(&g, 2, 1) == 1 && gop_resume_at(&g, 2, 3) == 3);
  CHECK(gop_resume_at(&g, 2, 4) == -1 && gop_resume_at(&g, 2, 0) == -1 && gop_resume_at(&g, 3, 2) == -1);
  CHECK(gop_bytes_from(&g, 0) == 90 && gop_bytes_from(&g, 1) == 60 && gop_bytes_from(&g, 3) == 0);
  CHECK(gop_at(&g, 2) && gop_at(&g, 2)->ts == 4 && !gop_at(&g, 3));
  uint32_t gen = g.gen;

  // Full: the oldest goes, never the newest (2026-11-16: the GOP froze instead, a page that joined or fell behind
  // after that stayed black until mirroring restarted). The chain lost its start: no new viewer, but a stream that
  // has AU 0 goes on.
  CHECK(push_k(&g, 30, false, 5) == 3 && g.first == 1 && g.n == 3 && !gop_whole(&g) && !g.wait_idr);
  CHECK(g.overflows == 1 && g.gen == gen && gop_at(&g, 3)->off == 0 && gop_intact(&g));  // wrapped to the front
  CHECK(!gop_at(&g, 0) && gop_at(&g, 1) && gop_at(&g, 3)->ts == 5);
  CHECK(gop_resume_at(&g, 2, 1) == 1 && gop_resume_at(&g, 2, 4) == 4);  // AU 1 is still there; at the end: waits
  CHECK(gop_bytes_from(&g, 0) == 90 && gop_bytes_from(&g, 2) == 60 && gop_bytes_from(&g, 3) == 30);
  CHECK(push_k(&g, 30, false, 6) == 4 && g.first == 2 && g.overflows == 1 && gop_intact(&g));  // counted once a chain
  CHECK(gop_resume_at(&g, 2, 1) == -1 && gop_resume_at(&g, 2, 2) == 2);  // AU 1 was dropped: that decoder starts over

  // A new IDR: a new chain, whole again; a decoder of the old one starts over.
  CHECK(push_k(&g, 10, true, 7) == 0 && gop_whole(&g) && g.gen != gen && g.idr_ts == 7 && g.used == 10);
  CHECK(gop_resume_at(&g, 2, 2) == -1 && gop_resume_at(&g, 7, 1) == 1);

  // The AU list fills before the arena (a still screen, tiny AUs): the same.
  gop_init(&g, arena, sizeof arena, aus, 4);
  for (uint32_t i = 0; i < 4; i++) CHECK(push_k(&g, 5, i == 0, 10 + i) == (int)i);
  CHECK(push_k(&g, 5, false, 14) == 4 && g.first == 1 && g.n == 4 && gop_intact(&g));
  CHECK(gop_at(&g, 4) == &aus[0]);  // AU k in aus[k % max]

  // Any mix of sizes: the ring never overwrites an AU it keeps, and always keeps the newest.
  gop_init(&g, arena, sizeof arena, aus, 8);
  CHECK(push_k(&g, 17, true, 1) == 0);
  uint32_t seed = 1;
  for (int i = 0; i < 5000; i++) {
    seed = seed * 1103515245u + 12345u;
    size_t len = 1 + (seed >> 16) % 50;
    int k = push_k(&g, len, false, (uint32_t)i);
    if (k < 0 || (uint32_t)k + 1 != gop_end(&g) || gop_at(&g, (uint32_t)k)->len != len || !gop_intact(&g)) {
      CHECK(!"ring");
      break;
    }
  }
  CHECK(g.first > 0 && g.overflows == 1 && g.total >= g.used);
  uint32_t last = gop_end(&g) - 1;
  CHECK(gop_bytes_from(&g, last) == gop_at(&g, last)->len);

  // An AU larger than the whole arena: nothing after it decodes; a new generation waits for the next IDR.
  gen = g.gen;
  uint8_t big[101] = {0};
  CHECK(gop_push(&g, big, sizeof big, false, 20) == -1 && g.wait_idr && g.n == 0 && g.gen != gen && !gop_whole(&g));
  CHECK(push_k(&g, 5, false, 21) == -1 && g.n == 0);
  CHECK(gop_push(&g, big, sizeof big, true, 22) == -1 && g.wait_idr);  // an IDR too big as well
  CHECK(push_k(&g, 100, true, 23) == 0 && !g.wait_idr && gop_whole(&g) && g.used == 100);  // exactly the arena
  CHECK(push_k(&g, 1, false, 24) == 1 && g.first == 1 && g.n == 1 && gop_at(&g, 1)->off == 0 && gop_intact(&g));

  uint8_t idr[] = {0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x65, 9};
  CHECK(annexb_has_idr(idr, sizeof idr));
  CHECK(!annexb_has_idr(idr, 6));

  uint8_t avcc[] = {0, 0, 0, 2, 0x65, 1, 0, 0, 0, 1, 0x41};
  uint8_t want[] = {0, 0, 0, 1, 0x65, 1, 0, 0, 0, 1, 0x41};
  CHECK(avcc_to_annexb(avcc, sizeof avcc) && !memcmp(avcc, want, sizeof want));
  uint8_t bad[] = {0, 0, 0, 9, 0x65};
  CHECK(!avcc_to_annexb(bad, sizeof bad));
}

static void test_chunker(void) {
  uint8_t data[100], out[64], got[100];
  for (int i = 0; i < 100; i++) data[i] = (uint8_t)i;
  vchunk c;
  vchunk_begin(&c, data, sizeof data, VF_IDR | VF_SNAP, 0xAABBCCDD);
  size_t n, got_len = 0;
  int chunks = 0;
  uint16_t seq = 65534;
  while ((n = vchunk_next(&c, out, 20, seq)) > 0) {
    CHECK(n <= VCHUNK_HDR + 20);
    CHECK(out[0] == (uint8_t)seq && out[1] == (uint8_t)(seq >> 8));
    CHECK(out[2] & VF_IDR && out[2] & VF_SNAP);
    size_t off = VCHUNK_HDR;
    if (chunks == 0) {
      CHECK(out[2] & VF_START);
      CHECK(out[3] == 0xDD && out[6] == 0xAA);
      off += 4;
    } else {
      CHECK(!(out[2] & VF_START));
    }
    memcpy(got + got_len, out + off, n - off);
    got_len += n - off;
    chunks++;
    seq++;
    if (got_len < 100) CHECK(!(out[2] & VF_END));
  }
  CHECK(out[2] & VF_END);
  CHECK(got_len == 100 && !memcmp(got, data, 100));
  CHECK(chunks == 6);  // 16 + 5 * 20 >= 100
  CHECK(seq == 4);     // wrapped

  vchunk_begin(&c, data, 3, VF_CONFIG, 1);  // tiny AU: one chunk is START and END
  n = vchunk_next(&c, out, 20, 0);
  CHECK(n == VCHUNK_HDR + 4 + 3 && (out[2] & (VF_START | VF_END | VF_CONFIG)) == (VF_START | VF_END | VF_CONFIG));
  CHECK(vchunk_next(&c, out, 20, 1) == 0);
}


// An AVCC frame of n bytes: one slice NAL (type 1) filling it, with random content.
static void make_frame(uint8_t *f, size_t n) {
  uint32_t len = (uint32_t)(n - 4);
  f[0] = len >> 24, f[1] = len >> 16, f[2] = len >> 8, f[3] = len;
  f[4] = 0x41;
  for (size_t i = 5; i < n; i++) f[i] = (uint8_t)rand();
}

// The sender encrypts every frame on one continuous CTR keystream but drops some before sending; the
// receiver must find where to pick up again, and then decode every following frame in step.
static void test_keystream_resync(void) {
  static const uint8_t k16[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, k16, 128);
  ks_key key = {.aes = &aes};
  memset(key.iv0, 0xfe, 16);  // near a carry across all counter bytes
  key.iv0[0] = 0x11;

  // ks_at(pos) is where mbedtls leaves the keystream after pos bytes
  for (uint64_t pos = 0; pos < 70; pos += 7) {
    uint8_t iv[16], stream[16] = {0}, buf[80] = {0};
    size_t off = 0;
    memcpy(iv, key.iv0, 16);
    mbedtls_aes_crypt_ctr(&aes, pos, &off, iv, stream, buf, buf);
    keystream at;
    ks_at(&key, &at, pos);
    CHECK(at.off == off && !memcmp(at.iv, iv, 16) && (!off || !memcmp(at.stream, stream, 16)));
  }

  uint8_t snd_iv[16], snd_stream[16] = {0};
  size_t snd_off = 0;
  memcpy(snd_iv, key.iv0, 16);
  keystream rx = {.off = 0};
  memcpy(rx.iv, key.iv0, 16);
  static uint8_t frame[40000], cipher[40000], plain[40000];
  int decoded = 0, resynced = 0;
  for (int i = 0; i < 60; i++) {
    size_t n = 100 + (size_t)(rand() % (i % 9 == 0 ? 35000 : 3000));  // now and then a big keyframe
    make_frame(frame, n);
    mbedtls_aes_crypt_ctr(&aes, n, &snd_off, snd_iv, snd_stream, frame, cipher);
    bool dropped = i == 20 || i == 21 || i == 40;  // encrypted, never sent
    if (dropped) continue;
    keystream next;
    if (ks_try(&key, &rx, &next, cipher, plain, n) && avcc_to_annexb(plain, n)) {
      rx = next, decoded++;
    } else if (ks_resync(&key, &rx, &next, cipher, plain, n, 512 * 1024, NULL)) {
      CHECK(!memcmp(plain, frame, n));
      rx = next, decoded++, resynced++;
    }
  }
  CHECK(decoded == 57);
  CHECK(resynced == 2);
  mbedtls_aes_free(&aes);
}

static void hex(const char *s, uint8_t *out) {
  for (size_t i = 0; s[2 * i]; i++) sscanf(s + 2 * i, "%2hhx", &out[i]);
}

void cli_random(uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rand();
}

// docs/cli-protocol.md vectors (cli/testdata/vectors.json, made with Python's cryptography).
static void test_cli_proto(void) {
  uint8_t b_priv[32], c_pub[32], b_pub[32], k[32], want[32], id[16], mac[16], cn[16], bn[16], c2b[16], b2c[16];
  hex("65666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f8081828384", b_priv);
  hex("07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c", c_pub);
  CHECK(cli_pair(b_priv, c_pub, b_pub, k));
  hex("5714769d116bf76436ae74bc793d2c30ad1903c59ac5273805c7e2698b410c36", want);
  CHECK(!memcmp(b_pub, want, 32));
  hex("c07dccb75b1bc3461ff97978252c817f17838929f5ee8e46853bf60960ac1b4b", want);
  CHECK(!memcmp(k, want, 32));
  hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", id);
  cli_mac(k, "paired", id, 16, NULL, 0, mac);
  hex("7798e5494589af0a3a0693c8f69845c0", want);
  CHECK(!memcmp(mac, want, 16));
  uint8_t zero[32] = {0};
  CHECK(!cli_pair(b_priv, zero, b_pub, k));  // low-order point: refused

  // Pairing over Wi-Fi: the computer's commitment to its nonce, and the 6-digit code both sides show.
  uint8_t nc[16], commit[32];
  char code[7];
  hex("c07dccb75b1bc3461ff97978252c817f17838929f5ee8e46853bf60960ac1b4b", k);
  hex("303132333435363738393a3b3c3d3e3f", nc);
  cli_pair_commit(nc, commit);
  hex("816b9e7c25d559c5766755b3bbb36654ad451e080ffa93694a793d6eed41f40a", want);
  CHECK(!memcmp(commit, want, 32));
  cli_pair_code(k, nc, code);
  CHECK(!strcmp(code, "736717"));
  nc[0] ^= 1;  // another nonce, another code
  cli_pair_code(k, nc, code);
  CHECK(strcmp(code, "736717") && strlen(code) == 6);

  hex("c07dccb75b1bc3461ff97978252c817f17838929f5ee8e46853bf60960ac1b4b", k);
  hex("101112131415161718191a1b1c1d1e1f", cn);
  hex("202122232425262728292a2b2c2d2e2f", bn);
  CHECK(cli_session(k, cn, bn, c2b, b2c));
  hex("f7aa391511dd90a81e1c1a428156d7c8", want);
  CHECK(!memcmp(c2b, want, 16));
  hex("fcd94939cf057d1e0211098b17d96f7a", want);
  CHECK(!memcmp(b2c, want, 16));
  cli_mac(k, "hello", cn, 16, bn, 16, mac);
  hex("f56acc021dbffef560a8711bb7bb34bd", want);
  CHECK(!memcmp(mac, want, 16));

  uint8_t frame[64], plain[64], expect[64];
  uint32_t ctr = 0;
  hex("1001000000f408fb72dbfb180661172c15ebf19d92f98017", expect);
  int n = cli_open(c2b, 'C', expect, 24, &ctr, plain);
  CHECK(n == 7 && ctr == 1 && !memcmp(plain, "c7 ping", 7));
  CHECK(cli_open(c2b, 'B', expect, 24, &ctr, plain) == -1);  // wrong stream: other nonce
  expect[7] ^= 1;
  CHECK(cli_open(c2b, 'C', expect, 24, &ctr, plain) == -1);  // tampered
  CHECK(cli_seal(b2c, CLI_DATA, 'B', 2, (const uint8_t *)"r7 ok", 5, frame) == 22);
  hex("10020000000b79c3cb3399dad58409d400ed0ce6dbf8", expect);
  CHECK(!memcmp(frame, expect, 22));
  uint8_t v[8];
  hex("0100050000000001", v);
  CHECK(cli_seal(b2c, CLI_VIDEO, 'V', 1, v, 8, frame) == 25);
  hex("110100000091c4931fb64b82f0eed8ab2aac6d31f5faf1c93d", expect);
  CHECK(!memcmp(frame, expect, 25));
}

static void test_pace(void) {
  // the move -158 -295 of 2026-11-06: every report the same speed, the sum exact, never above the step
  long moves[][2] = {{-158, -295}, {150, 0}, {8, 295}, {12, 0}, {1, 0}, {-3, 7}, {466, 1316}};
  for (unsigned m = 0; m < sizeof moves / sizeof moves[0]; m++) {
    pace_t p;
    pace_start(&p, moves[m][0], moves[m][1], 10);
    long x = 0, y = 0, n = 0;
    double lo = 1e9, hi = 0;
    int sx, sy;
    while (pace_next(&p, &sx, &sy)) {
      double v = sqrt((double)sx * sx + (double)sy * sy);
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
      x += sx, y += sy, n++;
    }
    CHECK(x == moves[m][0] && y == moves[m][1]);
    CHECK(hi <= 11);  // each axis rounds on its own: at most a unit over
    CHECK(n < 2 || hi - lo <= 1.5);  // one speed, give or take a unit of rounding
  }
  pace_t p;
  pace_start(&p, 0, 0, 10);
  int sx, sy;
  CHECK(!pace_next(&p, &sx, &sy));
}

static void test_logring(void) {
  char buf[32], out[64];
  logring r;
  logring_init(&r, buf, sizeof buf);
  CHECK(logring_read(&r, 0, out, sizeof out) == 0 && out[0] == 0);
  logring_add(&r, "boot a\\b");
  uint32_t next = logring_read(&r, 0, out, sizeof out);
  CHECK(next == 9 && strcmp(out, "boot a\\\\b\\n") == 0);
  logring_add(&r, "wifi: got ip");
  logring_add(&r, "ota: done");  // 9 + 13 + 10 = 32 bytes: still fits
  CHECK(r.start == 0 && r.end == 32);
  logring_add(&r, "cli up");  // wraps: the oldest line goes, whole
  logring_read(&r, 0, out, sizeof out);
  CHECK(strncmp(out, "wifi: got ip\\n", 14) == 0 && r.end - r.start <= sizeof buf);
  uint32_t from = r.start, pages = 0;  // paging: small pages join up to the same text
  char all[128] = "", page[6];
  while (from < r.end && pages++ < 40) { from = logring_read(&r, from, page, sizeof page); strcat(all, page); }
  CHECK(strcmp(all, out) == 0);
  CHECK(logring_read(&r, r.end, out, sizeof out) == r.end && out[0] == 0);  // caught up
  logring_add(&r, "0123456789012345678901234567890123456789");  // longer than the ring: its tail is kept
  logring_read(&r, 0, out, sizeof out);
  CHECK(strlen(out) > 0 && strstr(out, "789\\n") != NULL);
}

// What the iPhone sees in Screen Mirroring: the Bonjour services and TXT records (airplay/mdns.c).
mdns_stub_service mdns_stub[4];
int mdns_stub_count;
char mdns_stub_host[32];
esp_err_t mdns_init(void) { mdns_stub_count = 0; return ESP_OK; }
esp_err_t mdns_hostname_set(const char *name) { snprintf(mdns_stub_host, sizeof mdns_stub_host, "%s", name); return ESP_OK; }
esp_err_t mdns_instance_name_set(const char *name) { (void)name; return ESP_OK; }
esp_err_t mdns_service_add(const char *instance, const char *type, const char *proto, uint16_t port,
                           mdns_txt_item_t *txt, size_t n) {
  (void)proto;
  mdns_stub_service *s = &mdns_stub[mdns_stub_count++];
  snprintf(s->instance, sizeof s->instance, "%s", instance);
  snprintf(s->type, sizeof s->type, "%s", type);
  s->port = port;
  s->n = n;
  for (size_t i = 0; i < n; i++) {
    snprintf(s->keys[i], sizeof s->keys[i], "%s", txt[i].key);
    snprintf(s->values[i], sizeof s->values[i], "%s", txt[i].value);
  }
  return ESP_OK;
}
airplay_cfg ap_cfg;
uint8_t ap_pk[32];
void ap_log(const char *fmt, ...) { (void)fmt; }

static const char *txt_value(const mdns_stub_service *s, const char *key) {
  for (size_t i = 0; i < s->n; i++) if (!strcmp(s->keys[i], key)) return s->values[i];
  return NULL;
}

static void test_mdns(void) {
  ap_cfg.name = "Tentacle";
  uint8_t mac[6] = {0x7C, 0x3E, 0x01, 0x02, 0xAB, 0xCD};
  memcpy(ap_cfg.hwaddr, mac, 6);
  for (int i = 0; i < 32; i++) ap_pk[i] = (uint8_t)i;
  CHECK(ap_mdns_start() && mdns_stub_count == 2);
  const mdns_stub_service *a = &mdns_stub[0], *r = &mdns_stub[1];
  CHECK(!strcmp(a->type, "_airplay") && !strcmp(a->instance, "Tentacle"));
  CHECK(!strcmp(r->type, "_raop") && !strcmp(r->instance, "7C3E0102ABCD@Tentacle"));
  CHECK(a->port == r->port && a->port > 0);
  CHECK(txt_value(a, "deviceid") && !strcmp(txt_value(a, "deviceid"), "7C:3E:01:02:AB:CD"));
  const char *pk = txt_value(a, "pk");
  CHECK(pk && strlen(pk) == 64 && !strncmp(pk, "000102", 6) && !strcmp(pk, txt_value(r, "pk")));
  CHECK(txt_value(a, "features") && !strcmp(txt_value(a, "features"), txt_value(r, "ft")));
  CHECK(txt_value(a, "pw") && !strcmp(txt_value(a, "pw"), "false"));  // no PIN prompt on the iPhone

  // The raw TXT record: length-prefixed "k=v" entries, whole entries only, never past cap.
  uint8_t rec[512];
  size_t len = ap_txt_record(true, rec, sizeof rec), o = 0, entries = 0;
  while (o < len) { o += 1 + rec[o]; entries++; }
  CHECK(o == len && entries == a->n && !memcmp(rec + 1, "deviceid=7C:3E:01:02:AB:CD", 26));
  size_t cut = ap_txt_record(true, rec, 40);
  CHECK(cut <= 40 && cut == 1u + rec[0]);  // only the first entry (28 bytes) fits
}

// Encrypt like the sender (CBC over the whole blocks, fresh IV per packet, tail in the clear) and decrypt back.
static void test_audio_packets(void) {
  uint8_t key[16], iv[16], plain[45], pkt[12 + 45];
  for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 7), iv[i] = (uint8_t)(0xa0 + i);
  for (int i = 0; i < 45; i++) plain[i] = (uint8_t)(i * 3 + 1);
  pkt[0] = 0x80, pkt[1] = 0x60, pkt[2] = 0x12, pkt[3] = 0x34;
  pkt[4] = 0x00, pkt[5] = 0x01, pkt[6] = 0xe2, pkt[7] = 0x40;
  memset(pkt + 8, 0, 4);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 128);
  uint8_t v[16];
  for (int round = 0; round < 2; round++) {  // the same IV again: nothing carries over between packets
    memcpy(v, iv, 16);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, 32, v, plain, pkt + 12);
    memcpy(pkt + 12 + 32, plain + 32, 13);
    ap_rtp r;
    CHECK(ap_rtp_parse(pkt, sizeof pkt, &r));
    CHECK(r.seq == 0x1234 && r.ts == 123456 && r.len == 45 && r.payload == pkt + 12);
    CHECK(!ap_audio_no_data(r.payload, r.len));
    uint8_t out[45];
    ap_audio_decrypt(key, iv, r.payload, r.len, out);
    CHECK(!memcmp(out, plain, 45));
    ap_audio_decrypt(key, iv, pkt + 12, 45, pkt + 12);  // in place
    CHECK(!memcmp(pkt + 12, plain, 45));
  }
  mbedtls_aes_free(&aes);
  ap_rtp r;
  CHECK(!ap_rtp_parse(pkt, 11, &r));
  pkt[0] = 0x40;
  CHECK(!ap_rtp_parse(pkt, sizeof pkt, &r));  // RTP version 1
  const uint8_t marker[4] = {0x00, 0x68, 0x34, 0x00};
  CHECK(ap_audio_no_data(marker, 4) && ap_audio_no_data(NULL, 0) && !ap_audio_no_data(plain, 4));
  uint8_t short_tail[7] = {1, 2, 3, 4, 5, 6, 7}, out[7];
  ap_audio_decrypt(key, iv, short_tail, 7, out);  // under one block: all clear
  CHECK(!memcmp(out, short_tail, 7));
}

static void test_audio_gate(void) {
  ap_audio_gate g = {0};
  CHECK(ap_audio_gate_packet(&g, 0, true) == AP_GATE_SKIP && !g.active);   // no-data packets start nothing
  CHECK(ap_audio_gate_packet(&g, 10, false) == AP_GATE_START && g.active && g.segments == 1);
  CHECK(ap_audio_gate_packet(&g, 20, false) == AP_GATE_FRAME);
  CHECK(ap_audio_gate_packet(&g, 30, true) == AP_GATE_SKIP);               // silence inside a segment
  CHECK(!ap_audio_gate_idle(&g, 20 + AP_AUDIO_IDLE_MS - 1));
  CHECK(ap_audio_gate_idle(&g, 20 + AP_AUDIO_IDLE_MS) && !g.active);      // ends once, timed from the last sound
  CHECK(!ap_audio_gate_idle(&g, 5000));
  CHECK(ap_audio_gate_packet(&g, 6000, false) == AP_GATE_START && g.segments == 2 && g.frames == 3);
}

int main(void) {
  test_audio_packets();
  test_audio_gate();
  test_mdns();
  test_logring();
  test_bplist_roundtrip();
  test_bplist_utf16();
  test_gop();
  test_chunker();
  test_keystream_resync();
  test_cli_proto();
  test_pace();
  fails += test_setup();
  fails += test_pair();
  fails += test_ota();
  fails += test_kbd();
  fails += test_web();
  fails += test_opts();
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
