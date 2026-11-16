// The status OLED's texts, see oled_text.h.
#include "oled_text.h"
#include <stdint.h>
#include <string.h>
#include "../setup/board_opts.h"

static const char *const EN[OT_COUNT] = {
  [OT_CONFIRM] = "PRESS BOOT TO CONFIRM",
  [OT_LEFT] = "%lu s left",
  [OT_PAIR_OPEN] = "PAIRING OPEN",
  [OT_PAIR_RUN] = "On the computer run:",
  [OT_QR_TITLE] = "Wi-Fi setup",
  [OT_QR_PASS] = "Password:",
  [OT_SETUP_JOIN] = "Wi-Fi setup: join",
  [OT_PASS] = "PW %s",
  [OT_NET] = "net %s %s",
  [OT_NO_WIFI] = "no Wi-Fi: BOOT 10s",
  [OT_PEERS] = "peers %d  cli %d",
  [OT_STOPPED] = "STOPPED",
  [OT_RUNNING] = "running",
  [OT_BAT] = "%s  bat %d%%",
};

static const char *const DE[OT_COUNT] = {
  [OT_CONFIRM] = "ZUM BESTAETIGEN: BOOT",
  [OT_LEFT] = "noch %lu s",
  [OT_PAIR_OPEN] = "KOPPELN OFFEN",
  [OT_PAIR_RUN] = "Am Computer eingeben:",
  [OT_QR_TITLE] = "WLAN-Setup",
  [OT_QR_PASS] = "Passwort:",
  [OT_SETUP_JOIN] = "Setup-WLAN beitreten:",
  [OT_PASS] = "PW %s",
  [OT_NET] = "WLAN %s %s",
  [OT_NO_WIFI] = "kein WLAN: BOOT 10s",
  [OT_PEERS] = "Peers %d  CLI %d",
  [OT_STOPPED] = "GESTOPPT",
  [OT_RUNNING] = "laeuft",
  [OT_BAT] = "%s  Akku %d%%",
};

const char *oled_text(int lang, int id) {
  if (id < 0 || id >= OT_COUNT) return "";
  return (lang == BO_LANG_DE ? DE : EN)[id];
}

// What the firmware asks to be confirmed (netcfg.cpp, web_server.cpp), and its German.
static const char *const REQUESTS[][2] = {
  {"Join this Wi-Fi?", "WLAN beitreten?"},
  {"New web password?", "Neues Web-Passwort?"},
  {"from the web page", "von der Webseite"},
  {"from a computer", "von einem Computer"},
};

const char *oled_request(int lang, const char *en) {
  if (!en) return "";
  if (lang != BO_LANG_DE) return en;
  for (size_t i = 0; i < sizeof REQUESTS / sizeof REQUESTS[0]; i++)
    if (!strcmp(en, REQUESTS[i][0])) return REQUESTS[i][1];
  return en;
}

// One code point from UTF-8; a broken or cut sequence is one U+FFFD and skips one byte.
static uint32_t next(const unsigned char **p) {
  const unsigned char *s = *p;
  uint32_t c = *s;
  int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
  if (n < 0) {
    *p = s + 1;
    return 0xfffd;
  }
  c = n ? c & (0x3f >> n) : c;
  for (int i = 1; i <= n; i++) {
    if ((s[i] & 0xc0) != 0x80) {
      *p = s + 1;
      return 0xfffd;
    }
    c = c << 6 | (s[i] & 0x3f);
  }
  *p = s + n + 1;
  return c;
}

size_t oled_ascii(const char *in, char *out, size_t cap) {
  if (!cap) return 0;
  size_t o = 0;
  const unsigned char *s = (const unsigned char *)(in ? in : "");
  while (*s) {
    uint32_t c = next(&s);
    const char *rep;
    char one[2] = {'?', 0};
    switch (c) {
      case 0xe4: rep = "ae"; break;
      case 0xf6: rep = "oe"; break;
      case 0xfc: rep = "ue"; break;
      case 0xc4: rep = "Ae"; break;
      case 0xd6: rep = "Oe"; break;
      case 0xdc: rep = "Ue"; break;
      case 0xdf: rep = "ss"; break;
      case 0x20ac: rep = "EUR"; break;
      default:
        if (c >= 0x20 && c < 0x7f) one[0] = (char)c;
        rep = one;
    }
    size_t n = strlen(rep);
    if (o + n >= cap) break;  // never half a replacement
    memcpy(out + o, rep, n);
    o += n;
  }
  out[o] = 0;
  return o;
}
