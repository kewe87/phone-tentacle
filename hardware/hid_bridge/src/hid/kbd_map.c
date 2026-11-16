#include "kbd_map.h"

#include <ctype.h>
#include <string.h>

#define S KBD_SHIFT
#define A KBD_ALT

static const char *const codes[KBD_LAYOUTS] = {"us", "de"};

int kbd_map_find(const char *code) {
  if (!code) return -1;
  char c[8];
  size_t n = strlen(code);
  if (n == 0 || n >= sizeof c) return -1;
  for (size_t i = 0; i <= n; i++) c[i] = (char)tolower((unsigned char)code[i]);
  if (!strcmp(c, "us") || !strcmp(c, "en") || !strcmp(c, "en-us") || !strcmp(c, "en_us")) return KBD_US;
  if (!strcmp(c, "de") || !strcmp(c, "de-de") || !strcmp(c, "de_de")) return KBD_DE;
  return -1;
}

const char *kbd_map_code(int layout) { return layout >= 0 && layout < KBD_LAYOUTS ? codes[layout] : "?"; }

const char *kbd_map_codes(void) { return "us de"; }

static int one(kbd_key_t out[2], uint8_t mods, uint8_t usage) {
  out[0].mods = mods;
  out[0].usage = usage;
  return 1;
}

// A dead key, then the key it applies to (space: the accent alone).
static int two(kbd_key_t out[2], kbd_key_t dead, kbd_key_t key) {
  out[0] = dead;
  out[1] = key;
  return 2;
}

// a-z on the layout: German swaps Y and Z.
static uint8_t letter(int layout, char c) {
  if (layout == KBD_DE && c == 'y') return 0x1d;
  if (layout == KBD_DE && c == 'z') return 0x1c;
  return (uint8_t)(0x04 + (c - 'a'));
}

static int us_punct(uint32_t c, kbd_key_t out[2]) {
  switch (c) {
    case '!': return one(out, S, 0x1e);
    case '"': return one(out, S, 0x34);
    case '#': return one(out, S, 0x20);
    case '$': return one(out, S, 0x21);
    case '%': return one(out, S, 0x22);
    case '&': return one(out, S, 0x24);
    case '\'': return one(out, 0, 0x34);
    case '(': return one(out, S, 0x26);
    case ')': return one(out, S, 0x27);
    case '*': return one(out, S, 0x25);
    case '+': return one(out, S, 0x2e);
    case ',': return one(out, 0, 0x36);
    case '-': return one(out, 0, 0x2d);
    case '.': return one(out, 0, 0x37);
    case '/': return one(out, 0, 0x38);
    case ':': return one(out, S, 0x33);
    case ';': return one(out, 0, 0x33);
    case '<': return one(out, S, 0x36);
    case '=': return one(out, 0, 0x2e);
    case '>': return one(out, S, 0x37);
    case '?': return one(out, S, 0x38);
    case '@': return one(out, S, 0x1f);
    case '[': return one(out, 0, 0x2f);
    case '\\': return one(out, 0, 0x31);
    case ']': return one(out, 0, 0x30);
    case '^': return one(out, S, 0x23);
    case '_': return one(out, S, 0x2d);
    case '`': return one(out, 0, 0x35);
    case '{': return one(out, S, 0x2f);
    case '|': return one(out, S, 0x31);
    case '}': return one(out, S, 0x30);
    case '~': return one(out, S, 0x35);
  }
  return 0;
}

// Apple's German layout (ISO): the dead keys ^ (left of 1), ´ and ` (right of ß), ~ (Option+N).
static const kbd_key_t DE_CIRC = {0, 0x35}, DE_ACUTE = {0, 0x2e}, DE_GRAVE = {S, 0x2e}, DE_TILDE = {A, 0x11},
                       SPACE = {0, 0x2c};

static int de_punct(uint32_t c, kbd_key_t out[2]) {
  switch (c) {
    case '!': return one(out, S, 0x1e);
    case '"': return one(out, S, 0x1f);
    case '#': return one(out, 0, 0x31);
    case '$': return one(out, S, 0x21);
    case '%': return one(out, S, 0x22);
    case '&': return one(out, S, 0x23);
    case '\'': return one(out, S, 0x31);
    case '(': return one(out, S, 0x25);
    case ')': return one(out, S, 0x26);
    case '*': return one(out, S, 0x30);
    case '+': return one(out, 0, 0x30);
    case ',': return one(out, 0, 0x36);
    case '-': return one(out, 0, 0x38);
    case '.': return one(out, 0, 0x37);
    case '/': return one(out, S, 0x24);
    case ':': return one(out, S, 0x37);
    case ';': return one(out, S, 0x36);
    case '<': return one(out, 0, 0x64);
    case '=': return one(out, S, 0x27);
    case '>': return one(out, S, 0x64);
    case '?': return one(out, S, 0x2d);
    case '@': return one(out, A, 0x0f);
    case '[': return one(out, A, 0x22);
    case '\\': return one(out, A | S, 0x24);
    case ']': return one(out, A, 0x23);
    case '^': return two(out, DE_CIRC, SPACE);
    case '_': return one(out, S, 0x38);
    case '`': return two(out, DE_GRAVE, SPACE);
    case '{': return one(out, A, 0x25);
    case '|': return one(out, A, 0x24);
    case '}': return one(out, A, 0x26);
    case '~': return two(out, DE_TILDE, SPACE);
    // beyond ASCII: keys of their own
    case 0xe4: return one(out, 0, 0x34);  // ä
    case 0xc4: return one(out, S, 0x34);  // Ä
    case 0xf6: return one(out, 0, 0x33);  // ö
    case 0xd6: return one(out, S, 0x33);  // Ö
    case 0xfc: return one(out, 0, 0x2f);  // ü
    case 0xdc: return one(out, S, 0x2f);  // Ü
    case 0xdf: return one(out, 0, 0x2d);  // ß
    case 0xa7: return one(out, S, 0x20);  // §
    case 0xb0: return one(out, S, 0x35);  // °
    case 0x20ac: return one(out, A, 0x08);  // €
    case 0xb4: return two(out, DE_ACUTE, SPACE);  // ´
  }
  // accented letters: the dead key, then the letter
  static const struct {
    uint16_t cp;
    char base;
    uint8_t dead;  // 0 acute, 1 grave, 2 circumflex, 3 tilde
  } acc[] = {
      {0xe1, 'a', 0}, {0xe9, 'e', 0}, {0xed, 'i', 0}, {0xf3, 'o', 0}, {0xfa, 'u', 0},
      {0xe0, 'a', 1}, {0xe8, 'e', 1}, {0xec, 'i', 1}, {0xf2, 'o', 1}, {0xf9, 'u', 1},
      {0xe2, 'a', 2}, {0xea, 'e', 2}, {0xee, 'i', 2}, {0xf4, 'o', 2}, {0xfb, 'u', 2},
      {0xf1, 'n', 3}, {0xe3, 'a', 3}, {0xf5, 'o', 3},
  };
  const kbd_key_t deads[] = {DE_ACUTE, DE_GRAVE, DE_CIRC, DE_TILDE};
  for (size_t i = 0; i < sizeof acc / sizeof acc[0]; i++) {
    // lower case, and its upper case 0x20 below (Á = á - 0x20 for all of these)
    for (int up = 0; up < 2; up++) {
      if (c != (uint32_t)(acc[i].cp - (up ? 0x20 : 0))) continue;
      kbd_key_t key = {(uint8_t)(up ? S : 0), letter(KBD_DE, acc[i].base)};
      return two(out, deads[acc[i].dead], key);
    }
  }
  return 0;
}

int kbd_map_keys(int layout, uint32_t cp, kbd_key_t out[2]) {
  if (layout < 0 || layout >= KBD_LAYOUTS) return 0;
  if (cp == ' ') return one(out, 0, 0x2c);
  if (cp == '\n') return one(out, 0, 0x28);
  if (cp == '\t') return one(out, 0, 0x2b);
  if (cp >= 'a' && cp <= 'z') return one(out, 0, letter(layout, (char)cp));
  if (cp >= 'A' && cp <= 'Z') return one(out, S, letter(layout, (char)(cp - 'A' + 'a')));
  if (cp == '0') return one(out, 0, 0x27);
  if (cp >= '1' && cp <= '9') return one(out, 0, (uint8_t)(0x1e + (cp - '1')));
  return layout == KBD_DE ? de_punct(cp, out) : us_punct(cp, out);
}

uint32_t kbd_utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  if (!*p) return 0;
  uint32_t cp;
  int n;
  if (p[0] < 0x80) cp = p[0], n = 0;
  else if ((p[0] & 0xe0) == 0xc0) cp = p[0] & 0x1f, n = 1;
  else if ((p[0] & 0xf0) == 0xe0) cp = p[0] & 0x0f, n = 2;
  else if ((p[0] & 0xf8) == 0xf0) cp = p[0] & 0x07, n = 3;
  else {
    *s += 1;
    return 0xfffd;
  }
  for (int i = 1; i <= n; i++) {
    if ((p[i] & 0xc0) != 0x80) {
      *s += i;
      return 0xfffd;
    }
    cp = cp << 6 | (p[i] & 0x3f);
  }
  *s += n + 1;
  return cp;
}

int kbd_map_check(int layout, const char *text, char *bad, size_t n) {
  size_t used = 0;
  int count = 0;
  if (n) bad[0] = 0;
  for (const char *s = text; *s;) {
    const char *start = s;
    uint32_t cp = kbd_utf8_next(&s);
    kbd_key_t k[2];
    if (cp != 0xfffd && kbd_map_keys(layout, cp, k)) continue;
    size_t len = (size_t)(s - start);
    int seen = 0;  // once each
    for (const char *t = text; t < start && !seen; t++)
      seen = !strncmp(t, start, len) && (t == text || ((unsigned char)*t & 0xc0) != 0x80);
    if (seen) continue;
    count++;
    if (n && used + len < n) {
      memcpy(bad + used, start, len);
      used += len;
      bad[used] = 0;
    }
  }
  return count;
}
