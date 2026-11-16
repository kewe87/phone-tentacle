#pragma once

#include <Arduino.h>
#include "USBHIDKeyboard.h"

// The phone's hardware keyboard layout (src/hid/kbd_map.h), kept in NVS; en-US until set. The session command
// `kbd layout [us|de]` and the setup page set it; the status reports it as "kbd".

void kbd_layout_begin();               // setup(): reads the stored layout
const char *kbd_layout_get();          // "us", "de"
bool kbd_layout_set(const char *code);  // stores it; false for a layout the board does not know (kbd_layout_codes())
const char *kbd_layout_codes();        // the layouts, space separated: "us de"

// Types UTF-8 text with the layout's keys: "ok", or "err cannot type on layout de: ..." (then nothing was typed).
String kbd_type(USBHIDKeyboard &kb, const String &text);

// Presses the keys of one character for a combo (cmd+z): its modifiers and key stay down until releaseAll().
// False when the layout has no single key for it.
bool kbd_press_char(USBHIDKeyboard &kb, char c);
