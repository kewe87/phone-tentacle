// Host tests for the web interface's plain-C parts (src/web/): the password record and the login lock, the cookie
// and Host helpers, where a point of the picture lands on the phone (letterbox bars as in the CLI's area.go), what
// the page's mouse and keyboard messages become, the move-coalescing queue, who has control (session or page),
// the video gate that skips to the next IDR when a browser falls behind, and the board's own TLS certificate.
// Alone: gcc -DTEST_WEB_MAIN -I<mbedtls>/include -o test_web test_web.c ../hid_bridge/src/web/*.c -lmbedx509
//        -lmbedcrypto -lm
#include <stdio.h>
#include <string.h>
#include "mbedtls/oid.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "../hid_bridge/src/web/web_auth.h"
#include "../hid_bridge/src/web/web_cert.h"
#include "../hid_bridge/src/web/web_ctl.h"
#include "../hid_bridge/src/web/web_http.h"
#include "../hid_bridge/src/web/web_input.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_password(void) {
  CHECK(wa_check_new("short") == WA_TOO_SHORT);
  CHECK(wa_check_new("12345678") == WA_OK);
  CHECK(wa_check_new("with\nnewline") == WA_BAD_CHAR);
  CHECK(wa_check_new("Grüße an alle") == WA_OK);  // UTF-8 is fine
  char big[WA_PW_MAX + 2];
  memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = 0;
  CHECK(wa_check_new(big) == WA_TOO_LONG);

  uint8_t salt[WA_SALT_LEN], rec[WA_REC_LEN];
  for (int i = 0; i < WA_SALT_LEN; i++) salt[i] = (uint8_t)i;
  CHECK(wa_make("correct horse", salt, 1000, rec));
  // PBKDF2-HMAC-SHA256("correct horse", 00..0f, 1000, 32), from Python's hashlib
  static const uint8_t want[32] = {0xc9, 0x14, 0xcc, 0x4f, 0x06, 0xcc, 0x6e, 0x8f, 0x46, 0xd1, 0x57,
                                   0xe3, 0xa1, 0xb5, 0xaa, 0x7a, 0xbc, 0xee, 0xbb, 0x17, 0xbb, 0x04,
                                   0x44, 0xcd, 0x4c, 0x4a, 0xc1, 0x6c, 0xa2, 0xae, 0x98, 0x64};
  CHECK(rec[0] == WA_VERSION && rec[1] == 0xe8 && rec[2] == 0x03 && !memcmp(rec + 5, salt, WA_SALT_LEN));
  CHECK(!memcmp(rec + 5 + WA_SALT_LEN, want, 32));
  CHECK(wa_verify("correct horse", rec, sizeof rec));
  CHECK(!wa_verify("correct horsf", rec, sizeof rec));
  CHECK(!wa_verify("", rec, sizeof rec));
  CHECK(!wa_verify("correct horse", rec, sizeof rec - 1));  // wrong size
  rec[0] = 2;
  CHECK(!wa_verify("correct horse", rec, sizeof rec));  // unknown version
  rec[0] = WA_VERSION;
  rec[1] = rec[2] = rec[3] = rec[4] = 0;
  CHECK(!wa_verify("correct horse", rec, sizeof rec));  // zero iterations
  CHECK(!wa_make("x", salt, 0, rec));

  CHECK(wa_same("abcd", "abcd") && !wa_same("abcd", "abce") && !wa_same("abc", "abcd") && !wa_same("", ""));
}

static void test_limit(void) {
  wa_limit l;
  wa_limit_init(&l);
  uint32_t t = 0xFFFFF000u;  // across the wrap
  for (int i = 0; i < WA_FREE_FAILS; i++) {
    CHECK(wa_limit_wait(&l, t) == 0);
    wa_limit_fail(&l, t);
  }
  CHECK(wa_limit_wait(&l, t) == 0);  // five wrong ones are free
  wa_limit_fail(&l, t);              // the sixth locks
  CHECK(wa_limit_wait(&l, t) == WA_LOCK_MS);
  CHECK(wa_limit_wait(&l, t + WA_LOCK_MS - 1) == 1);
  CHECK(wa_limit_wait(&l, t + WA_LOCK_MS) == 0);
  wa_limit_fail(&l, t + WA_LOCK_MS);  // the next one locks twice as long
  CHECK(wa_limit_wait(&l, t + WA_LOCK_MS) == 2 * WA_LOCK_MS);
  for (int i = 0; i < 20; i++) wa_limit_fail(&l, t);
  CHECK(wa_limit_wait(&l, t) == WA_LOCK_MAX_MS);  // never longer than the cap
  wa_limit_ok(&l);
  CHECK(wa_limit_wait(&l, t) == 0 && l.fails == 0);
}

static void test_http(void) {
  char v[40];
  CHECK(wh_cookie("a=1; tsid=abc123; b=2", "tsid", v, sizeof v) && !strcmp(v, "abc123"));
  CHECK(wh_cookie("tsid=x", "tsid", v, sizeof v) && !strcmp(v, "x"));
  CHECK(!wh_cookie("xtsid=1; tsidx=2", "tsid", v, sizeof v));
  CHECK(!wh_cookie("tsid=0123456789", "tsid", v, 5));  // too long for the buffer
  CHECK(!wh_cookie(NULL, "tsid", v, sizeof v));
  CHECK(wh_host("tentacle.local", v, sizeof v) && !strcmp(v, "tentacle.local"));
  CHECK(wh_host("192.168.1.20:443", v, sizeof v) && !strcmp(v, "192.168.1.20"));
  CHECK(wh_host("[fe80::1]:80", v, sizeof v) && !strcmp(v, "[fe80::1]"));
  CHECK(!wh_host("evil.com\r\nSet-Cookie: x", v, sizeof v) && !v[0]);
  CHECK(!wh_host("a b", v, sizeof v) && !wh_host("", v, sizeof v) && !wh_host("host:8x", v, sizeof v));
  CHECK(wh_same_origin("https://192.168.1.20", "192.168.1.20"));
  CHECK(!wh_same_origin("http://192.168.1.20", "192.168.1.20"));
  CHECK(!wh_same_origin("https://evil.example", "192.168.1.20"));
  CHECK(!wh_same_origin(NULL, "192.168.1.20") && !wh_same_origin("https://", ""));
}

static void test_screen(void) {
  int none[4] = {0, 0, 0, 0};
  wi_rect r = wi_screen(718, 1560, none);
  CHECK(r.x0 == 0 && r.y0 == 0 && r.x1 == 718 && r.y1 == 1560);
  // A phone picture (718x1560) pillarboxed in a 900x1560 frame: the bars are not screen.
  int pillar[4] = {91, 0, 91, 0};
  r = wi_screen(900, 1560, pillar);
  CHECK(r.x0 == 91 && r.x1 == 809 && r.y0 == 0 && r.y1 == 1560);
  int half[4] = {280, 0, 280, 0};  // bars that would leave less than half the frame: not bars
  r = wi_screen(1040, 1040, half);
  CHECK(r.x0 == 0 && r.x1 == 1040);
  // Black at the top and bottom of a dark app in a frame with a phone's aspect: not bars.
  int dark[4] = {0, 10, 0, 10};
  r = wi_screen(480, 1040, dark);
  CHECK(r.y0 == 0 && r.y1 == 1040);
  // Unequal bars: not centred, so not bars.
  int uneven[4] = {91, 0, 60, 0};
  r = wi_screen(900, 1560, uneven);
  CHECK(r.x0 == 0 && r.x1 == 900);
  int ax, ay;
  r = wi_screen(900, 1560, pillar);
  wi_to_abs(r, 91, 0, &ax, &ay);
  CHECK(ax == 0 && ay == 0);
  wi_to_abs(r, 809, 1560, &ax, &ay);
  CHECK(ax == WI_ABS_FULL && ay == WI_ABS_FULL);
  wi_to_abs(r, 450, 780, &ax, &ay);
  CHECK(ax == 16384 && ay == 16384);
  wi_to_abs(r, 10, -5, &ax, &ay);  // on a bar / outside: clamped to the screen's edge
  CHECK(ax == 0 && ay == 0);
}

static void test_messages(void) {
  wi_state s;
  wi_init(&s);
  char out[8][WI_CMD_MAX];
  int n;
  const char *why;
  CHECK(wi_message(&s, "down 10 10", out, 8, &n, &why) == WI_ERR && strstr(why, "no video"));
  CHECK(wi_message(&s, "size 480 1040", out, 8, &n, &why) == WI_NONE && s.w == 480);
  CHECK(wi_message(&s, "down 240 520", out, 8, &n, &why) == WI_INPUT && n == 2);
  CHECK(!strcmp(out[0], "abs 16384 16384") && !strcmp(out[1], "press") && s.down);
  CHECK(wi_message(&s, "move 240 520", out, 8, &n, &why) == WI_NONE);  // nowhere new
  CHECK(wi_message(&s, "move 240 1040", out, 8, &n, &why) == WI_INPUT && n == 1 && !strcmp(out[0], "abs 16384 32767 1600"));
  CHECK(wi_message(&s, "up 240 1040", out, 8, &n, &why) == WI_INPUT && n == 1 && !strcmp(out[0], "release") && !s.down);
  CHECK(wi_message(&s, "move 0 0", out, 8, &n, &why) == WI_NONE);  // hovering: nothing
  CHECK(wi_message(&s, "down 0 0", out, 8, &n, &why) == WI_INPUT);
  CHECK(wi_message(&s, "up 480 0", out, 8, &n, &why) == WI_INPUT && n == 2 && !strcmp(out[0], "abs 32767 0 1600"));
  CHECK(wi_message(&s, "long 0 1040", out, 8, &n, &why) == WI_INPUT && n == 2 && !strcmp(out[1], "press_for 900"));
  CHECK(wi_message(&s, "wheel 240 520 104", out, 8, &n, &why) == WI_INPUT && n == 5);
  CHECK(!strcmp(out[0], "abs 16384 16384") && !strcmp(out[2], "abs 16384 13107") && !strcmp(out[3], "wait 250") &&
        !strcmp(out[4], "release"));
  CHECK(wi_message(&s, "wheel 240 0 104", out, 8, &n, &why) == WI_NONE);  // already at the top edge
  CHECK(wi_message(&s, "down nan 3", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "down 1", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "key cmd+a", out, 8, &n, &why) == WI_INPUT && n == 1 && !strcmp(out[0], "key cmd+a"));
  CHECK(wi_message(&s, "key two words", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "key", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "text Hi there!\n", out, 8, &n, &why) == WI_INPUT && n == 4);
  CHECK(!strcmp(out[0], "type Hi") && !strcmp(out[1], "key space") && !strcmp(out[2], "type there!") &&
        !strcmp(out[3], "key enter"));
  CHECK(wi_message(&s, "text a\xc3\xa4" "b", out, 8, &n, &why) == WI_INPUT && n == 1 && !strcmp(out[0], "type ab"));
  s.layout = 1;  // German (KBD_DE): ä and € are typed, a character it has no keys for is still skipped
  CHECK(wi_message(&s, "text a\xc3\xa4" "b \xe2\x82\xac\xe4\xb8\xad", out, 8, &n, &why) == WI_INPUT && n == 3 &&
        !strcmp(out[0], "type a\xc3\xa4" "b") && !strcmp(out[2], "type \xe2\x82\xac"));
  {  // a long word is cut between characters, never inside one
    char msg[200] = "text ", *m = msg + 5;
    for (int i = 0; i < 60; i++) m += sprintf(m, "\xc3\xa4");
    CHECK(wi_message(&s, msg, out, 8, &n, &why) == WI_INPUT && n == 2 && strlen(out[0]) == 5 + 80 &&
          strlen(out[1]) == 5 + 40);
  }
  s.layout = 0;
  CHECK(wi_message(&s, "btn home", out, 8, &n, &why) == WI_INPUT && n == 1 && !strcmp(out[0], "homescreen"));
  CHECK(wi_message(&s, "btn control", out, 8, &n, &why) == WI_INPUT && n == 4 && !strcmp(out[2], "abs 30800 20000 4096"));
  CHECK(wi_message(&s, "btn switcher", out, 8, &n, &why) == WI_INPUT && n == 5);
  CHECK(wi_message(&s, "btn power", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "btn control", out, 3, &n, &why) == WI_ERR);  // does not fit: nothing half done
  CHECK(wi_message(&s, "bars 280 0 280", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "size 900 1560", out, 8, &n, &why) == WI_NONE);
  CHECK(wi_message(&s, "bars 91 0 91 0", out, 8, &n, &why) == WI_NONE);
  CHECK(wi_message(&s, "down 91 0", out, 8, &n, &why) == WI_INPUT && !strcmp(out[0], "abs 0 0"));
  CHECK(wi_message(&s, "sizes 1 2", out, 8, &n, &why) == WI_ERR);
  CHECK(wi_message(&s, "size 4 4", out, 8, &n, &why) == WI_ERR);
}

static void test_queue(void) {
  static wi_queue q;
  wi_queue_init(&q);
  char c[WI_CMD_MAX];
  CHECK(!wi_queue_pop(&q, c));
  CHECK(wi_queue_push(&q, "abs 1 1", false) && wi_queue_push(&q, "press", false));
  CHECK(wi_queue_push(&q, "abs 2 2 1600", true) && wi_queue_push(&q, "abs 3 3 1600", true));  // replaced
  CHECK(q.n == 3);
  CHECK(wi_queue_push(&q, "release", false) && wi_queue_push(&q, "abs 4 4 1600", true) && q.n == 5);
  CHECK(wi_queue_pop(&q, c) && !strcmp(c, "abs 1 1"));
  CHECK(wi_queue_pop(&q, c) && !strcmp(c, "press"));
  CHECK(wi_queue_pop(&q, c) && !strcmp(c, "abs 3 3 1600"));
  CHECK(wi_queue_pop(&q, c) && !strcmp(c, "release"));
  CHECK(wi_queue_pop(&q, c) && !strcmp(c, "abs 4 4 1600") && !wi_queue_pop(&q, c));
  for (int i = 0; i < WI_QUEUE; i++) CHECK(wi_queue_push(&q, "key a", false));
  CHECK(!wi_queue_push(&q, "key b", false));  // full
  wi_queue_clear(&q);
  CHECK(!wi_queue_pop(&q, c));
}

static void test_control(void) {
  wc_state c;
  wc_init(&c);
  uint32_t t = 1000;
  CHECK(!wc_tick(&c, false, t) && !c.control);  // no page
  wc_connect(&c, t);
  CHECK(wc_tick(&c, false, t) && c.control);  // no session: the page drives
  wc_disconnect(&c);
  CHECK(!c.control);
  // A session that is working: the page watches until it takes control.
  wc_cli_input(&c, t);
  wc_connect(&c, t + 10);
  CHECK(!wc_tick(&c, true, t + 10) && !c.control && wc_cli_active(&c, true, t + 10));
  wc_take(&c, t + 20);
  CHECK(c.control);
  // ... the session's input is refused from now on (the caller asks c.control); the page's keeps it alive.
  wc_web_input(&c, t + 1000);
  CHECK(!wc_tick(&c, true, t + 1000 + WC_WEB_IDLE_MS - 1) && c.control);
  CHECK(wc_tick(&c, true, t + 1000 + WC_WEB_IDLE_MS) && !c.control && c.watching);  // idle: given back
  CHECK(!wc_tick(&c, false, t + 2000 + WC_WEB_IDLE_MS) && !c.control);  // and not taken again by itself
  wc_take(&c, t + 5000);
  wc_release(&c);
  CHECK(!c.control && c.watching);
  // The session went quiet: a new page takes over by itself.
  wc_connect(&c, t + WC_CLI_IDLE_MS + 1);
  CHECK(!wc_cli_active(&c, true, t + WC_CLI_IDLE_MS + 1));
  CHECK(wc_tick(&c, true, t + WC_CLI_IDLE_MS + 1) && c.control);
  wc_disconnect(&c);
  wc_take(&c, t);
  CHECK(!c.control);  // nothing to take for
}

static void test_video_gate(void) {
  wv_gate g;
  wv_init(&g);
  CHECK(wv_send(&g, 1, 0, true, 60000, 5000));     // catching up: old is fine
  CHECK(wv_send(&g, 1, 1000, false, 5000, 5000));  // in step, 4 s behind
  CHECK(!wv_send(&g, 1, 1000, false, 6001, 5000)); // too far behind: skip ...
  CHECK(wv_waiting(&g, 1) && wv_waited(&g, 7001) == 1000);
  CHECK(!wv_send(&g, 1, 7000, false, 7001, 5000)); // ... the whole generation, even fresh access units
  CHECK(wv_send(&g, 2, 8000, true, 8001, 5000));   // the next IDR: going again
  CHECK(!wv_waiting(&g, 2) && wv_waited(&g, 9000) == 0);
  CHECK(wv_send(&g, 2, 0, false, 0xFFFFFFFFu, 0)); // no limit
  wv_init(&g);
  CHECK(wv_send(&g, 3, 0xFFFFFF00u, false, 100, 5000));  // across the wrap: 356 ms
}

// A deterministic stand-in for the board's random source: SHA-256 of a counter.
static int test_rng(void *ctx, unsigned char *out, size_t len) {
  uint32_t *n = ctx;
  while (len) {
    unsigned char b[32];
    (*n)++;
    mbedtls_sha256((const unsigned char *)n, sizeof *n, b, 0);
    size_t k = len < 32 ? len : 32;
    memcpy(out, b, k);
    out += k, len -= k;
  }
  return 0;
}

static void test_cert(void) {
  CHECK(wc_days(1970, 1, 1) == 0 && wc_days(2026, 1, 1) == 20454 && wc_days(2000, 3, 1) == 11017);
  int y, m, d;
  wc_date(wc_days(2028, 2, 29), &y, &m, &d);
  CHECK(y == 2028 && m == 2 && d == 29);
  CHECK(wc_build_days("Nov 16 2026") == wc_days(2026, 11, 16) && wc_build_days("Feb  3 2027") == wc_days(2027, 2, 3));
  CHECK(wc_build_days("Foo 1 2026") == -1 && wc_build_days("") == -1 && wc_build_days(NULL) == -1);

  static char key[WC_KEY_PEM_MAX], crt[WC_CRT_PEM_MAX];
  uint32_t n = 0;
  uint32_t ip = 20u << 24 | 1u << 16 | 168u << 8 | 192u;  // 192.168.1.20 as lwIP keeps it
  int32_t from = wc_days(2026, 11, 16);
  CHECK(wc_make("tentacle.local", ip, from, test_rng, &n, key, crt) == 0);
  CHECK(strstr(key, "BEGIN EC PRIVATE KEY") && strstr(crt, "BEGIN CERTIFICATE"));
  mbedtls_x509_crt c;
  mbedtls_x509_crt_init(&c);
  CHECK(mbedtls_x509_crt_parse(&c, (const unsigned char *)crt, strlen(crt) + 1) == 0);
  CHECK(c.version == 3 && c.valid_from.year == 2026 && c.valid_from.mon == 11 && c.valid_from.day == 16);
  CHECK(wc_days(c.valid_to.year, c.valid_to.mon, c.valid_to.day) - from == WC_DAYS && WC_DAYS <= 825);
  CHECK(mbedtls_pk_get_type(&c.pk) == MBEDTLS_PK_ECKEY && mbedtls_pk_get_bitlen(&c.pk) == 256);
  CHECK(mbedtls_x509_crt_check_extended_key_usage(&c, MBEDTLS_OID_SERVER_AUTH,
                                                  MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) == 0);
  CHECK(mbedtls_x509_crt_check_key_usage(&c, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0);
  CHECK(!c.MBEDTLS_PRIVATE(ca_istrue));
  bool dns = false, addr = false;
  for (const mbedtls_x509_sequence *s = &c.subject_alt_names; s && s->buf.p; s = s->next) {
    mbedtls_x509_subject_alternative_name san;
    if (mbedtls_x509_parse_subject_alt_name(&s->buf, &san)) continue;
    if (san.type == MBEDTLS_X509_SAN_DNS_NAME)
      dns |= san.san.unstructured_name.len == 14 && !memcmp(san.san.unstructured_name.p, "tentacle.local", 14);
    if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS)
      addr |= san.san.unstructured_name.len == 4 && !memcmp(san.san.unstructured_name.p, "\xc0\xa8\x01\x14", 4);
    mbedtls_x509_free_subject_alt_name(&san);
  }
  CHECK(dns && addr);
  // the key and the certificate belong together; the certificate signs itself
  mbedtls_pk_context k;
  mbedtls_pk_init(&k);
  CHECK(mbedtls_pk_parse_key(&k, (const unsigned char *)key, strlen(key) + 1, NULL, 0, test_rng, &n) == 0);
  CHECK(mbedtls_pk_check_pair(&c.pk, &k, test_rng, &n) == 0);
  uint32_t flags = 0;
  CHECK(mbedtls_x509_crt_verify(&c, &c, NULL, "tentacle.local", &flags, NULL, NULL) == 0 ||
        !(flags & ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)));
  mbedtls_pk_free(&k);

  char fp[WC_FP_LEN];
  int32_t until;
  CHECK(wc_info(crt, fp, &until) && strlen(fp) == 95 && fp[2] == ':' && until == from + WC_DAYS);
  unsigned char sum[32];
  mbedtls_sha256(c.raw.p, c.raw.len, sum, 0);
  char first[3];
  snprintf(first, sizeof first, "%02X", sum[0]);
  CHECK(!strncmp(fp, first, 2));
  mbedtls_x509_crt_free(&c);
  CHECK(wc_good(crt, from) && wc_good(crt, from + WC_DAYS - WC_RENEW_DAYS - 1));
  CHECK(!wc_good(crt, from + WC_DAYS - WC_RENEW_DAYS) && !wc_good("junk", from) && !wc_good("", from));
  uint32_t n2 = 0;
  static char crt2[WC_CRT_PEM_MAX], key2[WC_KEY_PEM_MAX];
  CHECK(wc_make("tentacle.local", 0, from, test_rng, &n2, key2, crt2) == 0 && strcmp(crt2, crt));  // no address
}

int test_web(void) {
  fails = 0;
  test_cert();
  test_password();
  test_limit();
  test_http();
  test_screen();
  test_messages();
  test_queue();
  test_control();
  test_video_gate();
  return fails;
}

#ifdef TEST_WEB_MAIN
int main(void) {
  int f = test_web();
  printf(f ? "%d FAILED\n" : "web: all tests passed\n", f);
  return f != 0;
}
#endif
