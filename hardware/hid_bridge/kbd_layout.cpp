#include "kbd_layout.h"

#include <Preferences.h>
#include "src/hid/kbd_map.h"

static int layout = KBD_US;

void kbd_layout_begin() {
  Preferences p;
  if (!p.begin("kbd", true)) return;  // never stored: stays en-US
  int l = kbd_map_find(p.getString("layout", "us").c_str());
  p.end();
  if (l >= 0) layout = l;
}

const char *kbd_layout_get() { return kbd_map_code(layout); }

const char *kbd_layout_codes() { return kbd_map_codes(); }

bool kbd_layout_set(const char *code) {
  int l = kbd_map_find(code);
  if (l < 0) return false;
  layout = l;
  Preferences p;
  if (p.begin("kbd", false)) {
    p.putString("layout", kbd_map_code(l));
    p.end();
  }
  return true;
}

// Modifier bits to their key usages (0xE0 left Ctrl, 0xE1 left Shift, 0xE2 left Alt = Option, 0xE3 left GUI = Cmd).
static void pressMods(USBHIDKeyboard &kb, uint8_t mods) {
  for (int i = 0; i < 8; i++)
    if (mods & (1 << i)) kb.pressRaw(0xE0 + i);
}

static void stroke(USBHIDKeyboard &kb, const kbd_key_t &k) {
  pressMods(kb, k.mods);
  kb.pressRaw(k.usage);
  kb.releaseAll();
}

String kbd_type(USBHIDKeyboard &kb, const String &text) {
  int l = layout;
  char bad[48];
  int n = kbd_map_check(l, text.c_str(), bad, sizeof bad);
  if (n) return String("err cannot type on layout ") + kbd_map_code(l) + ": " + bad + (strlen(bad) < (size_t)n ? " ..." : "");
  const char *s = text.c_str();
  for (uint32_t cp; (cp = kbd_utf8_next(&s));) {
    if (cp == '\r') continue;
    kbd_key_t k[2];
    int m = kbd_map_keys(l, cp, k);
    for (int i = 0; i < m; i++) stroke(kb, k[i]);
  }
  return "ok";
}

bool kbd_press_char(USBHIDKeyboard &kb, char c) {
  kbd_key_t k[2];
  if (kbd_map_keys(layout, (unsigned char)c, k) != 1) return false;  // none, or a dead key first
  pressMods(kb, k[0].mods);
  kb.pressRaw(k[0].usage);
  return true;
}
