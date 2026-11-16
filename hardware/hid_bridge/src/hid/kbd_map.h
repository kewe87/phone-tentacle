#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which keys type a character on the phone's hardware keyboard layout. iOS maps the keys a USB keyboard sends by
// the layout set on the phone (Settings > General > Keyboard > Hardware Keyboard); the board has to send the keys
// of that layout: with US keys on a German iPhone `_` came out `?` and Y and Z swapped (2026-11-16).
//
// The layouts are iOS's (Apple's) layouts: "de" is Apple's German ("Deutsch"), whose Option layer differs from the
// PC's AltGr layer (@ is Option+L, not AltGr+Q). Option is the Alt key.

enum { KBD_US = 0, KBD_DE = 1, KBD_LAYOUTS = 2 };

// HID modifier bits (the report's first byte).
enum { KBD_CTRL = 0x01, KBD_SHIFT = 0x02, KBD_ALT = 0x04, KBD_GUI = 0x08 };

typedef struct {
  uint8_t mods;   // KBD_* bits
  uint8_t usage;  // HID keyboard page usage
} kbd_key_t;

int kbd_map_find(const char *code);  // "us", "de" (also "en-US", "de-DE", any case) -> KBD_*, -1 if unknown
const char *kbd_map_code(int layout);  // KBD_* -> "us", "de"
const char *kbd_map_codes(void);       // the layouts, space separated: "us de"

// The keys that type code point cp on the layout, in order (a dead key and then the key it applies to); returns
// how many (1 or 2), 0 when the layout cannot type cp.
int kbd_map_keys(int layout, uint32_t cp, kbd_key_t out[2]);

// The next code point of UTF-8 text; advances *s. 0 at the end, 0xFFFD for a broken sequence.
uint32_t kbd_utf8_next(const char **s);

// Every character of text the layout cannot type, once each, as UTF-8 into bad (always terminated); returns how
// many there were (also those that did not fit).
int kbd_map_check(int layout, const char *text, char *bad, size_t n);

#ifdef __cplusplus
}
#endif
