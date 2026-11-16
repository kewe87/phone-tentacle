// STUB (web interface branch), see kbd_layout.h: remembers the choice in RAM only and changes nothing about typing.
// The real unit (NVS, the key tables) replaces this file on merge.
#include "kbd_layout.h"
#include <string.h>

static const char *layout = "us";

const char *kbd_layout_get() { return layout; }

bool kbd_layout_set(const char *code) {
  if (!strcmp(code, "us")) layout = "us";
  else if (!strcmp(code, "de")) layout = "de";
  else return false;
  return true;
}
