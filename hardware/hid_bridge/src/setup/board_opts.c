// The board's hardware settings, see board_opts.h.
#include "board_opts.h"
#include <stdio.h>
#include <string.h>

const char *const BO_KEYS[] = {"display", "abs", "battery", "battery_div", "lang", "sdlog", NULL};

void bo_defaults(board_opts *o) {
  o->display = BO_DISPLAY_AUTO;
  o->abs = 1;
  o->bat_pin = BO_BAT_NONE;
  o->bat_div = BO_DIV_DEFAULT;
  o->lang = BO_LANG_AUTO;
  o->sdlog = 0;
}

bool bo_bat_pin_ok(int pin) { return pin >= 1 && pin <= 10 && pin != BO_OLED_SDA && pin != BO_OLED_SCL; }

const char *bo_display_name(int d) { return d == BO_DISPLAY_ON ? "on" : d == BO_DISPLAY_OFF ? "off" : "auto"; }
const char *bo_lang_name(int l) { return l == BO_LANG_EN ? "en" : l == BO_LANG_DE ? "de" : "auto"; }

// A whole number in [lo, hi] of digits only.
static bool number(const char *s, int lo, int hi, int *out) {
  if (!s || !*s || strlen(s) > 4) return false;
  int n = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    n = n * 10 + (*s - '0');
  }
  if (n < lo || n > hi) return false;
  *out = n;
  return true;
}

// "2", "2.5", "2.25" -> hundredths; at most two decimals.
static bool hundredths(const char *s, int *out) {
  if (!s || !*s) return false;
  int whole = 0, frac = 0, digits = 0, places = -1;
  for (; *s; s++) {
    if (*s == '.' && places < 0) {
      places = 0;
      continue;
    }
    if (*s < '0' || *s > '9' || ++digits > 6) return false;
    if (places < 0) whole = whole * 10 + (*s - '0');
    else if (++places > 2) return false;
    else frac = frac * 10 + (*s - '0');
  }
  if (!digits || places == 0) return false;  // "." or "2."
  if (places == 1) frac *= 10;
  int v = whole * 100 + frac;
  if (v < BO_DIV_MIN || v > BO_DIV_MAX) return false;
  *out = v;
  return true;
}

const char *bo_values(const char *key) {
  if (!key) return NULL;
  if (!strcmp(key, "display")) return "auto|on|off";
  if (!strcmp(key, "abs")) return "on|off";
  if (!strcmp(key, "battery")) return "none|1-10 (a GPIO; not 5, 6)";
  if (!strcmp(key, "battery_div")) return "1.00-10.00";
  if (!strcmp(key, "lang")) return "auto|en|de";
  if (!strcmp(key, "sdlog")) return "off|log|video";
  return NULL;
}

int bo_set(board_opts *o, const char *key, const char *v) {
  if (!bo_values(key)) return BO_UNKNOWN_KEY;
  if (!v) return BO_BAD_VALUE;
  int n;
  if (!strcmp(key, "display")) {
    if (!strcmp(v, "auto")) o->display = BO_DISPLAY_AUTO;
    else if (!strcmp(v, "on")) o->display = BO_DISPLAY_ON;
    else if (!strcmp(v, "off")) o->display = BO_DISPLAY_OFF;
    else return BO_BAD_VALUE;
  } else if (!strcmp(key, "abs")) {
    if (!strcmp(v, "on") || !strcmp(v, "1")) o->abs = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0")) o->abs = 0;
    else return BO_BAD_VALUE;
  } else if (!strcmp(key, "battery")) {
    if (!strcmp(v, "none") || !strcmp(v, "off")) o->bat_pin = BO_BAT_NONE;
    else if (number(v, 0, 99, &n) && bo_bat_pin_ok(n)) o->bat_pin = n;
    else return BO_BAD_VALUE;
  } else if (!strcmp(key, "battery_div")) {
    if (!hundredths(v, &n)) return BO_BAD_VALUE;
    o->bat_div = n;
  } else if (!strcmp(key, "sdlog")) {
    if (!strcmp(v, "log")) o->sdlog = BO_SD_LOG;
    else if (!strcmp(v, "video") || !strcmp(v, "on") || !strcmp(v, "1")) o->sdlog = BO_SD_VIDEO;  // REC is video
    else if (!strcmp(v, "off") || !strcmp(v, "0")) o->sdlog = BO_SD_OFF;
    else return BO_BAD_VALUE;
  } else {  // lang
    if (!strcmp(v, "auto")) o->lang = BO_LANG_AUTO;
    else if (!strcmp(v, "en")) o->lang = BO_LANG_EN;
    else if (!strcmp(v, "de")) o->lang = BO_LANG_DE;
    else return BO_BAD_VALUE;
  }
  return BO_OK;
}

bool bo_get(const board_opts *o, const char *key, char *out, size_t cap) {
  if (!key || !cap) return false;
  if (!strcmp(key, "display")) snprintf(out, cap, "%s", bo_display_name(o->display));
  else if (!strcmp(key, "abs")) snprintf(out, cap, "%s", o->abs ? "on" : "off");
  else if (!strcmp(key, "battery")) {
    if (o->bat_pin == BO_BAT_NONE) snprintf(out, cap, "none");
    else snprintf(out, cap, "%d", o->bat_pin);
  } else if (!strcmp(key, "battery_div")) snprintf(out, cap, "%d.%02d", o->bat_div / 100, o->bat_div % 100);
  else if (!strcmp(key, "lang")) snprintf(out, cap, "%s", bo_lang_name(o->lang));
  else if (!strcmp(key, "sdlog")) snprintf(out, cap, "%s", o->sdlog == BO_SD_VIDEO ? "video" : o->sdlog == BO_SD_LOG ? "log" : "off");
  else return false;
  return true;
}

int bo_oled_lang(int lang, const char *web) {
  if (lang == BO_LANG_EN || lang == BO_LANG_DE) return lang;
  return web && !strcmp(web, "de") ? BO_LANG_DE : BO_LANG_EN;
}
