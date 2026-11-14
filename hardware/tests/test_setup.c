// Host tests for the Wi-Fi setup page's text helpers and the BOOT button / confirmation state machines.
// Part of `make -C hardware/tests`; alone (no mbedtls needed):
//   gcc -std=c11 -Wall -Wextra -DTEST_SETUP_MAIN -o test_setup test_setup.c ../hid_bridge/src/setup/setup_text.c ../hid_bridge/src/ui/confirm.c
#include <stdio.h>
#include <string.h>
#include "../hid_bridge/src/setup/setup_text.h"
#include "../hid_bridge/src/ui/confirm.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_html_esc(void) {
  char o[64];
  CHECK(setup_html_esc("Home", o, sizeof o) == 4 && !strcmp(o, "Home"));
  setup_html_esc("<script>alert('x')</script>", o, sizeof o);
  CHECK(!strcmp(o, "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;"));
  setup_html_esc("a\"b&c", o, sizeof o);
  CHECK(!strcmp(o, "a&quot;b&amp;c"));
  setup_html_esc("a\nb\x7f" "c", o, sizeof o);
  CHECK(!strcmp(o, "a?b?c"));
  setup_html_esc("Caf\xc3\xa9", o, sizeof o);  // UTF-8 passes
  CHECK(!strcmp(o, "Caf\xc3\xa9"));
  // too small: stops before the entity that does not fit, still terminated
  char s[8];
  CHECK(setup_html_esc("ab<<", s, sizeof s) == 6 && !strcmp(s, "ab&lt;"));
  CHECK(setup_html_esc("abcdefghij", s, sizeof s) == 7 && !strcmp(s, "abcdefg"));
  CHECK(setup_html_esc("x", s, 1) == 0 && !s[0]);
  // the longest name, all quotes
  char in[33], big[6 * 32 + 1];
  memset(in, '\'', 32);
  in[32] = 0;
  CHECK(setup_html_esc(in, big, sizeof big) == 5 * 32);
  // nothing that could open a tag or leave an attribute survives
  setup_html_esc("\"><img src=x onerror=alert(1)>", o, sizeof o);
  CHECK(!strpbrk(o, "<>\"'"));
}

static void test_json_esc(void) {
  char o[64];
  setup_json_esc("a\"b\\c", o, sizeof o);
  CHECK(!strcmp(o, "a\\\"b\\\\c"));
  setup_json_esc("</script>&\n", o, sizeof o);
  CHECK(!strcmp(o, "\\u003c/script\\u003e\\u0026\\u000a"));
  char s[6];
  CHECK(setup_json_esc("ab<", s, sizeof s) == 2 && !strcmp(s, "ab"));  // no half escape
}

static void test_hex(void) {
  char h[2 * 32 + 2], o[33];
  CHECK(setup_hex_enc("Hi <b>", h, sizeof h) == 13 && !strcmp(h, "x4869203c623e"));
  CHECK(setup_hex_dec(h, o, sizeof o) && !strcmp(o, "Hi <b>"));
  char full[33];
  memset(full, 0xfe, 32);
  full[32] = 0;
  CHECK(setup_hex_enc(full, h, sizeof h) == 65);
  CHECK(setup_hex_dec(h, o, sizeof o) && !memcmp(o, full, 33));
  CHECK(setup_hex_dec("x4A6b", o, sizeof o) && !strcmp(o, "Jk"));
  CHECK(!setup_hex_dec("", o, sizeof o));
  CHECK(!setup_hex_dec("x", o, sizeof o));
  CHECK(!setup_hex_dec("4869", o, sizeof o));      // no prefix
  CHECK(!setup_hex_dec("x486", o, sizeof o));      // odd
  CHECK(!setup_hex_dec("x48zz", o, sizeof o));     // not hex
  CHECK(!setup_hex_dec("x480069", o, sizeof o));   // NUL inside
  char longer[2 * 33 + 2] = "x";
  for (int i = 0; i < 33; i++) strcat(longer, "41");
  CHECK(!setup_hex_dec(longer, o, sizeof o));      // 33 bytes
  CHECK(!setup_hex_dec("x4869", o, 2));            // no room
}

static void test_check(void) {
  CHECK(setup_check("Home", "12345678") == SETUP_OK);
  CHECK(setup_check("Home", "") == SETUP_OK);  // open network
  CHECK(setup_check("", "12345678") == SETUP_BAD_SSID);
  CHECK(setup_check(NULL, NULL) == SETUP_BAD_SSID);
  char s33[34], p64[65];
  memset(s33, 'a', 33);
  s33[33] = 0;
  memset(p64, 'p', 64);
  p64[64] = 0;
  CHECK(setup_check(s33, "") == SETUP_BAD_SSID);
  CHECK(setup_check(s33 + 1, "") == SETUP_OK);  // 32
  CHECK(setup_check("Home", "1234567") == SETUP_BAD_PASS);
  CHECK(setup_check("Home", p64) == SETUP_BAD_PASS);
  CHECK(setup_check("Home", p64 + 1) == SETUP_OK);  // 63
  CHECK(setup_check("Ho\nme", "") == SETUP_BAD_SSID);
  CHECK(setup_check("Home", "1234\r5678") == SETUP_BAD_PASS);
  CHECK(setup_check("Caf\xc3\xa9 \"x\"", "p\xc3\xa4ssw\xc3\xb6rt") == SETUP_OK);
}

static void test_lang(void) {
  CHECK(setup_lang_de("de-DE,de;q=0.9,en-US;q=0.8,en;q=0.7"));
  CHECK(setup_lang_de("de"));
  CHECK(setup_lang_de("DE-at"));
  CHECK(!setup_lang_de("en-US,en;q=0.9,de;q=0.8"));
  CHECK(!setup_lang_de("en-GB,en;q=0.9"));
  CHECK(!setup_lang_de("fr-FR,fr;q=0.9"));
  CHECK(setup_lang_de("fr-FR,fr;q=0.9,de;q=0.5"));  // German before the English default
  CHECK(setup_lang_de("fr, en;q=0.3, de;q=0.4"));
  CHECK(!setup_lang_de("fr, en;q=0.4, de;q=0.4"));  // tie: the first listed
  CHECK(setup_lang_de("de;q=0.4, en;q=0.4"));
  CHECK(!setup_lang_de("de;q=0, en"));
  CHECK(!setup_lang_de("de;q=0"));
  CHECK(!setup_lang_de(""));
  CHECK(!setup_lang_de(NULL));
  CHECK(!setup_lang_de("*"));
  CHECK(!setup_lang_de("den,dev"));   // not the language "de"
  CHECK(!setup_lang_de(",,;;q=,de;q=x"));  // garbage: q without digits is 0
  CHECK(setup_lang_de("de;q=1.000"));
  CHECK(setup_lang_de("en;q=0.999,de;q=1"));
}

static void test_button(void) {
  btn_state b;
  btn_init(&b);
  uint32_t t = 1000;
  CHECK(btn_step(&b, false, t) == BTN_NONE);
  // a glitch shorter than the debounce time is no press
  CHECK(btn_step(&b, true, t += 10) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 10) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 100) == BTN_NONE);
  // a press: once, after the debounce time
  CHECK(btn_step(&b, true, t += 10) == BTN_NONE);
  uint32_t down = t;
  CHECK(btn_step(&b, true, t += 10) == BTN_NONE);
  CHECK(btn_step(&b, true, t += 20) == BTN_PRESS);
  CHECK(btn_step(&b, true, t += 10) == BTN_NONE);
  CHECK(btn_held_ms(&b, t) == t - down);
  // bounce while held changes nothing
  CHECK(btn_step(&b, false, t += 5) == BTN_NONE);
  CHECK(btn_step(&b, true, t += 5) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 10) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 30) == BTN_NONE);  // released
  CHECK(btn_held_ms(&b, t) == 0);
  // short press again, then a long one: BTN_LONG once, 10 s after it went down
  CHECK(btn_step(&b, true, t += 100) == BTN_NONE);
  down = t;
  CHECK(btn_step(&b, true, t += 30) == BTN_PRESS);
  CHECK(btn_step(&b, true, down + BTN_LONG_MS - 1) == BTN_NONE);
  CHECK(btn_step(&b, true, down + BTN_LONG_MS) == BTN_LONG);
  CHECK(btn_step(&b, true, down + BTN_LONG_MS + 5000) == BTN_NONE);
  CHECK(btn_step(&b, false, t = down + BTN_LONG_MS + 5010) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 30) == BTN_NONE);
  // released at 9 s: no long press, and the next press starts from zero
  CHECK(btn_step(&b, true, t += 100) == BTN_NONE);
  CHECK(btn_step(&b, true, t += 30) == BTN_PRESS);
  CHECK(btn_step(&b, true, t += 9000) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 10) == BTN_NONE);
  CHECK(btn_step(&b, false, t += 30) == BTN_NONE);
  CHECK(btn_step(&b, true, t += 2000) == BTN_NONE);
  CHECK(btn_step(&b, true, t += 30) == BTN_PRESS);
  CHECK(btn_step(&b, true, t += 5000) == BTN_NONE);
  // across the millis() wrap
  btn_init(&b);
  t = 0xffffff00u;
  CHECK(btn_step(&b, true, t) == BTN_NONE);
  CHECK(btn_step(&b, true, t + 40) == BTN_PRESS);
  CHECK(btn_step(&b, true, t + BTN_LONG_MS - 1) == BTN_NONE);
  CHECK(btn_step(&b, true, t + BTN_LONG_MS) == BTN_LONG);  // wrapped past zero
}

static void test_confirm(void) {
  confirm_slot c;
  cf_init(&c);
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_IDLE);
  CHECK(!cf_press(&c, 10));  // nothing open: the press is free for the stop toggle
  CHECK(!cf_request(&c, CONFIRM_NONE, "a", "b", 1000, 10));

  // accepted in time
  CHECK(cf_request(&c, CONFIRM_WIFI, "Join this Wi-Fi?", "Home", 30000, 100));
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_WAITING && cf_state(&c, CONFIRM_PAIR) == CF_IDLE);
  CHECK(cf_left_ms(&c, 100) == 30000 && cf_left_ms(&c, 10100) == 20000);
  CHECK(!strcmp(c.l1, "Join this Wi-Fi?") && !strcmp(c.l2, "Home"));
  CHECK(!cf_request(&c, CONFIRM_PAIR, "x", "y", 30000, 200));  // one at a time
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_WAITING);
  CHECK(!cf_press(&c, 100 + CF_ARM_MS - 1));  // too soon to be an answer: a normal press, the request still waits
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_WAITING);
  CHECK(cf_press(&c, 30099));
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_ACCEPTED && cf_left_ms(&c, 30099) == 0);
  CHECK(!cf_press(&c, 30100));  // the second press is a normal one again
  cf_tick(&c, 99999);
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_ACCEPTED);  // stays until read
  cf_clear(&c, CONFIRM_PAIR);  // not the asker: no effect
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_ACCEPTED);
  cf_clear(&c, CONFIRM_WIFI);
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_IDLE);

  // not pressed: expired, and a late press does not accept
  CHECK(cf_request(&c, CONFIRM_WIFI, "a", "b", 30000, 1000));
  cf_tick(&c, 30999);
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_WAITING);
  CHECK(!cf_press(&c, 31000));
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_EXPIRED && cf_left_ms(&c, 31000) == 0);
  // an unread result does not block the other kind
  CHECK(cf_request(&c, CONFIRM_PAIR, "Pair with", "PC 123456", 30000, 40000));
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_IDLE && cf_state(&c, CONFIRM_PAIR) == CF_WAITING);
  // the same kind asks again: new text, new time
  CHECK(cf_request(&c, CONFIRM_PAIR, "Pair with", "PC 654321", 30000, 50000));
  CHECK(!strcmp(c.l2, "PC 654321") && cf_left_ms(&c, 50000) == 30000);
  cf_clear(&c, CONFIRM_PAIR);  // withdrawn
  CHECK(!cf_press(&c, 50001));

  // long text is cut to a display line; NULL is empty
  CHECK(cf_request(&c, CONFIRM_WIFI, "0123456789012345678901234567890123456789", NULL, 1000, 0));
  CHECK(strlen(c.l1) == CF_TEXT - 1 && !c.l2[0]);
  // across the millis() wrap
  CHECK(cf_request(&c, CONFIRM_WIFI, "a", "b", 30000, 0xfffffff0u));
  cf_tick(&c, 100);
  CHECK(cf_state(&c, CONFIRM_WIFI) == CF_WAITING);
  CHECK(cf_press(&c, 29000));
}

int test_setup(void) {
  test_html_esc();
  test_json_esc();
  test_hex();
  test_check();
  test_lang();
  test_button();
  test_confirm();
  return fails;
}

#ifdef TEST_SETUP_MAIN
int main(void) {
  test_setup();
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
#endif
