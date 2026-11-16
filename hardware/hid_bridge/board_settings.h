// The board's settings as one command (docs/cli-protocol.md, `config`) and one JSON object, for the computer's
// session and the web interface. The values live in NVS (netcfg.h); what they mean: src/setup/board_opts.h.
#pragma once
#include <Arduino.h>

// {"display":"auto","oled":1,"abs":"on","abs_active":1,"battery":"none","battery_div":"2.00","lang":"auto",
//  "name":"Tentacle","restart":0} (+ "bat":87 with a battery pin). oled: the display is in use; abs_active: the
// pointer this boot runs with; restart 1: a stored `abs` or `name` waits for a restart.
String settings_json();
// One setting: display, abs, battery, battery_div, lang (board_opts.h) or name (netcfg.h). "" when stored, else why
// not ("unknown setting ...", "bad value for ...: auto|on|off").
String settings_set(const char *key, const char *value);
// `config` -> "ok <json>"; `config KEY VALUE` -> stored, "ok <json>"; otherwise "err ...".
String settings_command(const String &arg);
