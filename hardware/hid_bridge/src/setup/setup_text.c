// Text helpers of the Wi-Fi setup page, see setup_text.h.
#include "setup_text.h"
#include <stdio.h>
#include <string.h>

static size_t put(char *out, size_t cap, size_t n, const char *s) {
  size_t l = strlen(s);
  if (n + l >= cap) return 0;  // no room: the caller stops, nothing is cut in half
  memcpy(out + n, s, l);
  return l;
}

size_t setup_html_esc(const char *in, char *out, size_t cap) {
  size_t n = 0;
  if (!cap) return 0;
  for (; in && *in; in++) {
    unsigned char ch = (unsigned char)*in;
    char one[2] = {(char)ch, 0};
    const char *s = one;
    if (ch == '&') s = "&amp;";
    else if (ch == '<') s = "&lt;";
    else if (ch == '>') s = "&gt;";
    else if (ch == '"') s = "&quot;";
    else if (ch == '\'') s = "&#39;";
    else if (ch < 0x20 || ch == 0x7f) s = "?";
    size_t l = put(out, cap, n, s);
    if (!l) break;
    n += l;
  }
  out[n] = 0;
  return n;
}

size_t setup_json_esc(const char *in, char *out, size_t cap) {
  size_t n = 0;
  if (!cap) return 0;
  for (; in && *in; in++) {
    unsigned char ch = (unsigned char)*in;
    char buf[8] = {(char)ch, 0};
    if (ch == '"' || ch == '\\') snprintf(buf, sizeof buf, "\\%c", ch);
    else if (ch < 0x20 || ch == 0x7f || ch == '<' || ch == '>' || ch == '&') snprintf(buf, sizeof buf, "\\u%04x", ch);
    size_t l = put(out, cap, n, buf);
    if (!l) break;
    n += l;
  }
  out[n] = 0;
  return n;
}

size_t setup_hex_enc(const char *in, char *out, size_t cap) {
  size_t n = 0;
  if (!cap) return 0;
  if (cap >= 2) out[n++] = 'x';
  for (; in && *in && n + 2 < cap; in++) n += (size_t)snprintf(out + n, 3, "%02x", (unsigned char)*in);
  out[n] = 0;
  return n;
}

static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool setup_hex_dec(const char *in, char *out, size_t cap) {
  if (!in || !cap || in[0] != 'x') return false;
  size_t l = strlen(++in), n = l / 2;
  if (!l || l % 2 || n > 32 || n >= cap) return false;
  for (size_t i = 0; i < n; i++) {
    int hi = nibble(in[2 * i]), lo = nibble(in[2 * i + 1]);
    if (hi < 0 || lo < 0 || !(hi | lo)) return false;
    out[i] = (char)(hi << 4 | lo);
  }
  out[n] = 0;
  return true;
}

static bool hasControl(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || *s == 0x7f) return true;
  return false;
}

int setup_check(const char *ssid, const char *pass) {
  size_t s = ssid ? strlen(ssid) : 0, p = pass ? strlen(pass) : 0;
  if (s < 1 || s > 32 || hasControl(ssid)) return SETUP_BAD_SSID;
  if (p && (p < 8 || p > 63 || hasControl(pass))) return SETUP_BAD_PASS;
  return SETUP_OK;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// "de-DE,de;q=0.9,en;q=0.8": each entry is a language range with an optional weight (q, 0-1, default 1).
bool setup_lang_de(const char *h) {
  int de = -1, en = -1;  // best weight in thousandths; on a tie the one listed first wins
  bool deFirst = false;
  while (h && *h) {
    while (*h == ' ' || *h == ',') h++;
    const char *tag = h;
    while (*h && *h != ',' && *h != ';') h++;
    size_t tagLen = (size_t)(h - tag);
    int q = 1000;
    if (*h == ';') {
      while (*h && *h != ',' && *h != '=') h++;
      if (*h == '=') {
        h++;
        q = 0;
        int scale = 1000;
        for (bool frac = false; *h && *h != ','; h++) {
          if (*h == '.') frac = true;
          else if (*h >= '0' && *h <= '9') {
            if (!frac) q = q * 10 + (*h - '0') * 1000;
            else if (scale > 1) q += (*h - '0') * (scale /= 10);
          }
        }
        if (q > 1000) q = 1000;
      }
      while (*h && *h != ',') h++;
    }
    if (tagLen >= 2 && (tagLen == 2 || tag[2] == '-' || tag[2] == ' ')) {
      char a = lower(tag[0]), b = lower(tag[1]);
      if (a == 'd' && b == 'e' && q > de) {
        de = q;
        if (en < 0) deFirst = true;
      }
      if (a == 'e' && b == 'n' && q > en) en = q;
    }
  }
  if (de <= 0) return false;
  return de > en || (de == en && deFirst);
}
