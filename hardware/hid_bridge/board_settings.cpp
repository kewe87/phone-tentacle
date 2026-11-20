// The board's settings for the session and the web interface, see board_settings.h.
#include "board_settings.h"
#include "logbuf.h"
#include "netcfg.h"
#include "sdlog.h"
#include "status_display.h"
#include "src/setup/board_opts.h"

static const char *const USAGE = "err usage: config [display auto|on|off | abs on|off | battery none|GPIO | "
                                  "battery_div 1.00-10.00 | lang auto|en|de | sdlog off|log|video | name NAME]";

String settings_json() {
  board_opts o;
  cfg_opts(&o);
  String j = "{";
  char v[16];
  for (const char *const *k = BO_KEYS; *k; k++) {
    bo_get(&o, *k, v, sizeof v);
    j += String("\"") + *k + "\":\"" + v + "\",";
    if (!strcmp(*k, "display")) j += String("\"oled\":") + (display_present() ? 1 : 0) + ",";
    if (!strcmp(*k, "abs")) j += String("\"abs_active\":") + (cfg_abs_active() ? 1 : 0) + ",";
    if (!strcmp(*k, "sdlog")) j += String("\"sd\":\"") + sdlog_state_name() + "\",\"sd_file\":\"" + sdlog_file() + "\",";
    if (!strcmp(*k, "battery_div") && battery_percent() >= 0) j += String("\"bat\":") + battery_percent() + ",";
  }
  String saved = board_name_saved();  // [A-Za-z0-9 ._-] only: nothing to escape
  bool restart = o.abs != (cfg_abs_active() ? 1 : 0) || saved != board_name();
  return j + "\"name\":\"" + saved + "\",\"restart\":" + (restart ? 1 : 0) + "}";
}

String settings_set(const char *key, const char *value) {
  if (!strcmp(key, "name")) {
    if (!board_set_name(value)) return "bad name: 1-20 of A-Z a-z 0-9 space . _ -, no space at either end";
    board_log("settings: name \"%s\" (after a restart)", value);
    return "";
  }
  int r = cfg_set_opt(key, value);
  if (r == BO_UNKNOWN_KEY) return String("unknown setting ") + key;
  if (r != BO_OK) return String("bad value for ") + key + ": " + bo_values(key);
  board_log("settings: %s %s%s", key, value, !strcmp(key, "abs") ? " (after a restart)" : "");
  if (!strcmp(key, "display") || !strcmp(key, "lang")) display_reconfigure();
  if (!strcmp(key, "sdlog")) sdlog_reconfigure();
  return "";
}

String settings_command(const String &arg) {
  String a = arg;
  a.trim();
  if (!a.length()) return "ok " + settings_json();
  int sp = a.indexOf(' ');
  if (sp < 0) return USAGE;
  String key = a.substring(0, sp), value = a.substring(sp + 1);
  value.trim();
  if (!value.length()) return USAGE;
  String why = settings_set(key.c_str(), value.c_str());
  if (why.length()) return "err " + why;
  return "ok " + settings_json();
}
