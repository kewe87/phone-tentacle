// Host tests for the keyboard layouts (src/hid/kbd_map.c): the keys of a character on US and German, the dead
// keys, UTF-8, and the list of what a layout cannot type.
// Alone: gcc -DTEST_KBD_MAIN -o test_kbd test_kbd.c ../hid_bridge/src/hid/kbd_map.c && ./test_kbd
#include <stdio.h>
#include <string.h>
#include "../hid_bridge/src/hid/kbd_map.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int key1(int layout, uint32_t cp, uint8_t mods, uint8_t usage) {
  kbd_key_t k[2];
  return kbd_map_keys(layout, cp, k) == 1 && k[0].mods == mods && k[0].usage == usage;
}

static void test_find(void) {
  CHECK(kbd_map_find("us") == KBD_US && kbd_map_find("en-US") == KBD_US && kbd_map_find("DE") == KBD_DE);
  CHECK(kbd_map_find("de-DE") == KBD_DE && kbd_map_find("fr") == -1 && kbd_map_find("") == -1);
  CHECK(kbd_map_find("a-very-long-code") == -1 && kbd_map_find(NULL) == -1);
  CHECK(!strcmp(kbd_map_code(KBD_DE), "de") && !strcmp(kbd_map_code(KBD_US), "us"));
}

static void test_us(void) {
  kbd_key_t k[2];
  for (uint32_t c = 0x20; c < 0x7f; c++) CHECK(kbd_map_keys(KBD_US, c, k) == 1);  // all of ASCII, one key each
  CHECK(key1(KBD_US, 'z', 0, 0x1d) && key1(KBD_US, 'Y', KBD_SHIFT, 0x1c));
  CHECK(key1(KBD_US, '_', KBD_SHIFT, 0x2d) && key1(KBD_US, '@', KBD_SHIFT, 0x1f) && key1(KBD_US, '?', KBD_SHIFT, 0x38));
  CHECK(kbd_map_keys(KBD_US, 0xe4, k) == 0);  // ä: not on US
}

static void test_de(void) {
  kbd_key_t k[2];
  for (uint32_t c = 0x20; c < 0x7f; c++) CHECK(kbd_map_keys(KBD_DE, c, k) >= 1);  // all of ASCII
  // the bug of 2026-11-16: `_` came out `?`, Y and Z swapped
  CHECK(key1(KBD_DE, '_', KBD_SHIFT, 0x38) && key1(KBD_DE, '?', KBD_SHIFT, 0x2d));
  CHECK(key1(KBD_DE, 'z', 0, 0x1c) && key1(KBD_DE, 'y', 0, 0x1d) && key1(KBD_DE, 'Z', KBD_SHIFT, 0x1c));
  CHECK(key1(KBD_DE, '-', 0, 0x38) && key1(KBD_DE, '/', KBD_SHIFT, 0x24) && key1(KBD_DE, '<', 0, 0x64));
  // Apple's Option layer
  CHECK(key1(KBD_DE, '@', KBD_ALT, 0x0f) && key1(KBD_DE, 0x20ac, KBD_ALT, 0x08));
  CHECK(key1(KBD_DE, '\\', KBD_ALT | KBD_SHIFT, 0x24) && key1(KBD_DE, '{', KBD_ALT, 0x25));
  // umlauts and ß
  CHECK(key1(KBD_DE, 0xe4, 0, 0x34) && key1(KBD_DE, 0xd6, KBD_SHIFT, 0x33) && key1(KBD_DE, 0xfc, 0, 0x2f));
  CHECK(key1(KBD_DE, 0xdf, 0, 0x2d));
  // dead keys: ^ alone is ^ then space; é is ´ then e; È is ` then Shift+E
  CHECK(kbd_map_keys(KBD_DE, '^', k) == 2 && k[0].usage == 0x35 && k[1].usage == 0x2c);
  CHECK(kbd_map_keys(KBD_DE, 0xe9, k) == 2 && k[0].usage == 0x2e && !k[0].mods && k[1].usage == 0x08 && !k[1].mods);
  CHECK(kbd_map_keys(KBD_DE, 0xc8, k) == 2 && k[0].mods == KBD_SHIFT && k[1].mods == KBD_SHIFT && k[1].usage == 0x08);
  CHECK(kbd_map_keys(KBD_DE, 0xf1, k) == 2 && k[0].mods == KBD_ALT && k[0].usage == 0x11 && k[1].usage == 0x11);
  CHECK(kbd_map_keys(KBD_DE, 0x4e00, k) == 0);
}

static void test_utf8(void) {
  const char *s = "a\xc3\xa4\xe2\x82\xac\xf0\x9f\x98\x80\xff";  // a ä € 😀 broken
  CHECK(kbd_utf8_next(&s) == 'a' && kbd_utf8_next(&s) == 0xe4 && kbd_utf8_next(&s) == 0x20ac);
  CHECK(kbd_utf8_next(&s) == 0x1f600 && kbd_utf8_next(&s) == 0xfffd && kbd_utf8_next(&s) == 0);
  const char *cut = "\xc3";
  CHECK(kbd_utf8_next(&cut) == 0xfffd && *cut == 0);
}

static void test_check(void) {
  char bad[16];
  CHECK(kbd_map_check(KBD_DE, "Gr\xc3\xbc\xc3\x9f" "e_zu Hause @ 5 \xe2\x82\xac", bad, sizeof bad) == 0 && !bad[0]);
  CHECK(kbd_map_check(KBD_US, "Gr\xc3\xbc\xc3\x9f" "e \xc3\xbc", bad, sizeof bad) == 2 && !strcmp(bad, "\xc3\xbc\xc3\x9f"));
  CHECK(kbd_map_check(KBD_US, "abc", bad, sizeof bad) == 0);
  char tiny[3];  // ü does not fit with its terminator after ä: counted, not written
  CHECK(kbd_map_check(KBD_US, "\xc3\xa4\xc3\xbc", tiny, sizeof tiny) == 2 && !strcmp(tiny, "\xc3\xa4"));
}

int test_kbd(void) {
  test_find();
  test_us();
  test_de();
  test_utf8();
  test_check();
  return fails;
}

#ifdef TEST_KBD_MAIN
int main(void) {
  test_kbd();
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
#endif
