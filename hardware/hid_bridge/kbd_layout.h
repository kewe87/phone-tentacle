// STUB (web interface branch): the keyboard layout unit comes from the parallel keyboard fix and replaces this file
// on merge. Same interface: the layout the phone's hardware keyboard setting expects ("us", "de"), kept in NVS,
// session command `kbd layout <code>`, status field "kbd".
#pragma once

const char *kbd_layout_get();             // "us" | "de"
bool kbd_layout_set(const char *code);    // false: unknown code
