// Host tests for the board's hardware settings (src/setup/board_opts.c) and the OLED's texts (src/ui/oled_text.c):
// what each key takes, the defaults of a board without them, that every text fits its line, and the ASCII
// transliteration of the display.
// Alone: gcc -DTEST_OPTS_MAIN -o test_opts test_opts.c ../hid_bridge/src/setup/board_opts.c ../hid_bridge/src/ui/oled_text.c && ./test_opts
#include <stdio.h>
#include <string.h>
#include "../hid_bridge/src/setup/board_opts.h"
#include "../hid_bridge/src/ui/oled_text.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_defaults(void) {
  board_opts o;
  bo_defaults(&o);
  // a board without the keys: the release behaviour (OLED if one answers, absolute pointer, no battery)
  CHECK(o.display == BO_DISPLAY_AUTO && o.abs == 1 && o.bat_pin == BO_BAT_NONE && o.bat_div == 200 && o.lang == BO_LANG_AUTO);
  char v[16];
  CHECK(bo_get(&o, "display", v, sizeof v) && !strcmp(v, "auto"));
  CHECK(bo_get(&o, "abs", v, sizeof v) && !strcmp(v, "on"));
  CHECK(bo_get(&o, "battery", v, sizeof v) && !strcmp(v, "none"));
  CHECK(bo_get(&o, "battery_div", v, sizeof v) && !strcmp(v, "2.00"));
  CHECK(bo_get(&o, "lang", v, sizeof v) && !strcmp(v, "auto"));
  CHECK(bo_get(&o, "sdlog", v, sizeof v) && !strcmp(v, "off") && o.sdlog == 0);
  CHECK(!bo_get(&o, "name", v, sizeof v));
  int n = 0;
  for (const char *const *k = BO_KEYS; *k; k++, n++) CHECK(bo_values(*k) && bo_get(&o, *k, v, sizeof v));
  CHECK(n == 6);
}

static void test_set(void) {
  board_opts o;
  bo_defaults(&o);
  CHECK(bo_set(&o, "display", "off") == BO_OK && o.display == BO_DISPLAY_OFF);
  CHECK(bo_set(&o, "display", "on") == BO_OK && o.display == BO_DISPLAY_ON);
  CHECK(bo_set(&o, "display", "yes") == BO_BAD_VALUE && o.display == BO_DISPLAY_ON);
  CHECK(bo_set(&o, "abs", "off") == BO_OK && o.abs == 0);
  CHECK(bo_set(&o, "abs", "1") == BO_OK && o.abs == 1);
  CHECK(bo_set(&o, "sdlog", "on") == BO_OK && o.sdlog == BO_SD_VIDEO);  // REC means video
  char v[16];
  CHECK(bo_get(&o, "sdlog", v, sizeof v) && !strcmp(v, "video"));
  CHECK(bo_set(&o, "sdlog", "log") == BO_OK && o.sdlog == BO_SD_LOG);
  CHECK(bo_get(&o, "sdlog", v, sizeof v) && !strcmp(v, "log"));
  CHECK(bo_set(&o, "sdlog", "yes") == BO_BAD_VALUE && o.sdlog == BO_SD_LOG);
  CHECK(bo_set(&o, "sdlog", "0") == BO_OK && o.sdlog == 0);
  CHECK(bo_set(&o, "abs", "") == BO_BAD_VALUE && bo_set(&o, "abs", NULL) == BO_BAD_VALUE && o.abs == 1);
  // ADC1 pins only, not the OLED's I2C pins, not the BOOT button (0) or the LED (21)
  CHECK(bo_set(&o, "battery", "1") == BO_OK && o.bat_pin == 1);
  CHECK(bo_set(&o, "battery", "10") == BO_OK && o.bat_pin == 10);
  CHECK(bo_set(&o, "battery", "5") == BO_BAD_VALUE && bo_set(&o, "battery", "6") == BO_BAD_VALUE);
  CHECK(bo_set(&o, "battery", "0") == BO_BAD_VALUE && bo_set(&o, "battery", "21") == BO_BAD_VALUE);
  CHECK(bo_set(&o, "battery", "11") == BO_BAD_VALUE && bo_set(&o, "battery", "-1") == BO_BAD_VALUE);
  CHECK(bo_set(&o, "battery", "1x") == BO_BAD_VALUE && bo_set(&o, "battery", "00001") == BO_BAD_VALUE && o.bat_pin == 10);
  CHECK(bo_set(&o, "battery", "none") == BO_OK && o.bat_pin == BO_BAT_NONE);
  CHECK(bo_set(&o, "battery", "3") == BO_OK && bo_set(&o, "battery", "off") == BO_OK && o.bat_pin == BO_BAT_NONE);
  CHECK(bo_set(&o, "battery_div", "2.5") == BO_OK && o.bat_div == 250);
  CHECK(bo_set(&o, "battery_div", "3.13") == BO_OK && o.bat_div == 313);
  CHECK(bo_set(&o, "battery_div", "1") == BO_OK && o.bat_div == 100);
  CHECK(bo_set(&o, "battery_div", "10.00") == BO_OK && o.bat_div == 1000);
  CHECK(bo_set(&o, "battery_div", "0.99") == BO_BAD_VALUE && bo_set(&o, "battery_div", "10.01") == BO_BAD_VALUE);
  CHECK(bo_set(&o, "battery_div", "2.") == BO_BAD_VALUE && bo_set(&o, "battery_div", ".") == BO_BAD_VALUE);
  CHECK(bo_set(&o, "battery_div", "2.125") == BO_BAD_VALUE && bo_set(&o, "battery_div", "2,5") == BO_BAD_VALUE);
  CHECK(bo_set(&o, "battery_div", "1.2.3") == BO_BAD_VALUE && o.bat_div == 1000);
  CHECK(bo_set(&o, "battery_div", ".5") == BO_BAD_VALUE);  // below 1
  CHECK(bo_set(&o, "battery_div", "2.05") == BO_OK && bo_get(&o, "battery_div", v, sizeof v) && !strcmp(v, "2.05"));
  CHECK(bo_set(&o, "lang", "de") == BO_OK && o.lang == BO_LANG_DE);
  CHECK(bo_set(&o, "lang", "fr") == BO_BAD_VALUE && o.lang == BO_LANG_DE);
  CHECK(bo_set(&o, "nope", "1") == BO_UNKNOWN_KEY && bo_set(&o, NULL, "1") == BO_UNKNOWN_KEY);
  CHECK(bo_values("nope") == NULL);
}

static void test_lang(void) {
  CHECK(bo_oled_lang(BO_LANG_AUTO, "de") == BO_LANG_DE);
  CHECK(bo_oled_lang(BO_LANG_AUTO, "en") == BO_LANG_EN);
  CHECK(bo_oled_lang(BO_LANG_AUTO, "") == BO_LANG_EN && bo_oled_lang(BO_LANG_AUTO, NULL) == BO_LANG_EN);
  CHECK(bo_oled_lang(BO_LANG_EN, "de") == BO_LANG_EN && bo_oled_lang(BO_LANG_DE, "en") == BO_LANG_DE);
}

// Every line, with its widest values, fits the display (21 columns; the QR screen's 12).
static void test_fit(void) {
  int langs[] = {BO_LANG_EN, BO_LANG_DE};
  char b[64];
  for (int i = 0; i < 2; i++) {
    int l = langs[i];
    for (int id = 0; id < OT_COUNT; id++) {
      const char *t = oled_text(l, id);
      CHECK(t && *t);
      for (const char *p = t; *p; p++) CHECK(*p >= 0x20 && *p < 0x7f);  // the font is ASCII
    }
    CHECK(strlen(oled_text(l, OT_CONFIRM)) <= OLED_COLS);
    CHECK(snprintf(b, sizeof b, oled_text(l, OT_LEFT), 999UL) <= OLED_COLS);
    CHECK(strlen(oled_text(l, OT_PAIR_OPEN)) <= OLED_COLS && strlen(oled_text(l, OT_PAIR_RUN)) <= OLED_COLS);
    CHECK(strlen(oled_text(l, OT_QR_TITLE)) <= OLED_QR_COLS && strlen(oled_text(l, OT_QR_PASS)) <= OLED_QR_COLS);
    CHECK(strlen(oled_text(l, OT_SETUP_JOIN)) <= OLED_COLS);
    CHECK(snprintf(b, sizeof b, oled_text(l, OT_PASS), "abcdefghijkm") <= OLED_COLS);  // the hotspot's 12
    CHECK(snprintf(b, sizeof b, oled_text(l, OT_NET), "sta", "10.0.0.123") <= OLED_COLS);
    CHECK(strlen(oled_text(l, OT_NO_WIFI)) <= OLED_COLS);
    CHECK(snprintf(b, sizeof b, oled_text(l, OT_PEERS), 4, 1) <= OLED_COLS);
    CHECK(snprintf(b, sizeof b, oled_text(l, OT_BAT), oled_text(l, OT_STOPPED), 100) <= OLED_COLS);
    CHECK(snprintf(b, sizeof b, oled_text(l, OT_BAT), oled_text(l, OT_RUNNING), 100) <= OLED_COLS);
  }
  CHECK(!strcmp(oled_text(BO_LANG_DE, OT_RUNNING), "laeuft") && !strcmp(oled_text(BO_LANG_EN, OT_STOPPED), "STOPPED"));
  CHECK(!strcmp(oled_text(BO_LANG_AUTO, OT_PAIR_OPEN), "PAIRING OPEN"));  // anything but de: English
  CHECK(!strcmp(oled_text(BO_LANG_DE, OT_COUNT), ""));
  // the requests: known ones translated and short enough, anything else as it is
  const char *req[] = {"Join this Wi-Fi?", "New web password?", "from the web page", "from a computer"};
  for (int i = 0; i < 4; i++) {
    CHECK(strcmp(oled_request(BO_LANG_DE, req[i]), req[i]) && strlen(oled_request(BO_LANG_DE, req[i])) <= OLED_COLS);
    CHECK(!strcmp(oled_request(BO_LANG_EN, req[i]), req[i]));
  }
  CHECK(!strcmp(oled_request(BO_LANG_DE, "Code 123456"), "Code 123456") && !strcmp(oled_request(BO_LANG_DE, NULL), ""));
}

static void test_ascii(void) {
  char o[32];
  CHECK(oled_ascii("Gr\xc3\xbc\xc3\x9f" "e \xc3\x84rger", o, sizeof o) == 14 && !strcmp(o, "Gruesse Aerger"));
  CHECK(oled_ascii("\xc3\xb6\xc3\x96\xc3\xbc\xc3\x9c\xc3\xa4", o, sizeof o) && !strcmp(o, "oeOeueUeae"));
  CHECK(oled_ascii("5 \xe2\x82\xac", o, sizeof o) && !strcmp(o, "5 EUR"));
  CHECK(oled_ascii("caf\xc3\xa9 \xf0\x9f\x98\x80!", o, sizeof o) && !strcmp(o, "caf? ?!"));
  CHECK(oled_ascii("a\tb\xff" "c\xc3", o, sizeof o) && !strcmp(o, "a?b?c?"));  // control, broken, cut
  CHECK(oled_ascii("\xc3\xa4\xc3\xa4\xc3\xa4", o, 6) == 4 && !strcmp(o, "aeae"));  // never half an "ae"
  CHECK(oled_ascii(NULL, o, sizeof o) == 0 && !*o);
  CHECK(oled_ascii("x", o, 1) == 0 && !*o);
}

int test_opts(void) {
  fails = 0;
  test_defaults();
  test_set();
  test_lang();
  test_fit();
  test_ascii();
  return fails;
}

#ifdef TEST_OPTS_MAIN
int main(void) {
  int f = test_opts();
  printf(f ? "%d FAILED\n" : "all tests passed\n", f);
  return f != 0;
}
#endif
