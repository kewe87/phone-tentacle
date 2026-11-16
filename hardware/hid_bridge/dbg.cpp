// Bring-up debugging over USB serial, see dbg.h. The whole file is empty unless built with TENTACLE_DEBUG.
#include "dbg.h"
#if TENTACLE_DEBUG
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include "tusb.h"
#include "button.h"
#include "cli_link.h"
#include "cli_tcp.h"
#include "netcfg.h"
#include "logbuf.h"
#include "ota.h"
#include "version.h"
#include "src/airplay/airplay.h"

const uint32_t MARKS_MS = 10000;
static Print *out = &Serial;  // where a dbg command answers: USB serial, or the session's buffer (dbg_session)
static bool rebootAsked;

void dbg_log(const char *fmt, ...) {
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  uint32_t ms = millis();
  Serial.printf("[%lu.%03lu] D %s\n", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), line);
  char kept[210];
  snprintf(kept, sizeof kept, "D %s", line);
  board_log_keep(kept);  // also in the RAM log: `logs`, and `tentacle logs --board --follow` over Wi-Fi
}

void dbg_begin(const char *reset, const char *crash) {
  dbg_log("boot: fw %s, reset %s, last crash: %s", FW_VERSION, reset, crash);
  dbg_log("boot: DEBUG BUILD, serial commands: dbg state | nvs | wifi [scan] | btn short | btn long | btn double | reboot | forget");
}

static const char *usb() { return !tud_mounted() ? "off" : tud_suspended() ? "asleep" : "ok"; }
static const char *airplay() {
  static const char *names[] = {"idle", "connected", "streaming"};
  return names[airplay_state()];
}

// Every state another part keeps, as one JSON line.
static void state() {
  char c1[CF_TEXT] = "", c2[CF_TEXT] = "";
  uint32_t left = 0;
  bool waiting = confirm_open(c1, c2, &left);
  uint32_t pairLeft = 0;
  bool pairOpen = pair_window_open(&pairLeft);  // the pairing window (BOOT double press), and what is left of its 60 s
  NetSetup su;
  net_setup_status(&su);
  uint8_t id[6];
  cli_board_id(id);
  out->printf("dbg state {\"fw\":\"%s\",\"up\":%lu,\"id\":\"%02x%02x%02x%02x%02x%02x\",\"net\":\"%s\",\"ip\":\"%s\","
                "\"wifi_status\":%d,\"rssi\":%d,\"setup\":%d,\"setup_state\":%d,\"usb\":\"%s\",\"airplay\":\"%s\","
                "\"stopped\":%d,\"confirm\":%d,\"confirm_l1\":\"%s\",\"confirm_l2\":\"%s\",\"confirm_left\":%lu,"
                "\"tcp\":%d,\"session\":%d,\"pair_open\":%d,\"pair_left\":%lu,\"pairing\":%d,\"paired\":%d,"
                "\"heap\":%u,\"heap_min\":%u,\"psram\":%u,\"ota\":%s}\n",
                FW_VERSION, (unsigned long)millis(), id[0], id[1], id[2], id[3], id[4], id[5], net_mode(),
                net_ip().c_str(), (int)WiFi.status(), (int)WiFi.RSSI(), (int)net_setup_active(), su.state, usb(), airplay(),
                (int)stop_active(), (int)waiting, c1, c2, (unsigned long)left, (int)cli_tcp_up(), (int)cli_up(),
                (int)pairOpen, (unsigned long)pairLeft, (int)cli_pair_open(), cli_paired(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), ota_json().c_str());
}

// What is stored, without anything secret: passwords as lengths, keys not at all, client ids cut short.
static void nvs() {
  Preferences p;
  p.begin("agent", true);
  out->printf("dbg nvs agent {\"mode\":\"%s\",\"sta_ssid\":\"%s\",\"sta_pass_len\":%u,\"ap_ssid\":\"%s\",\"ap_pass_len\":%u,"
                "\"w\":%u,\"h\":%u,\"fps\":%u,\"ed_seed\":%d}\n",
                p.getString("mode", "").c_str(), p.getString("sta_ssid", "").c_str(),
                (unsigned)p.getString("sta_pass", "").length(), p.getString("ap_ssid", "").c_str(),
                (unsigned)p.getString("ap_pass", "").length(), p.getUShort("w", 0), p.getUShort("h", 0),
                p.getUChar("fps", 0), (int)p.isKey("ed_seed"));
  p.end();
  struct { uint8_t id[16], key[32]; } c[4] = {};
  p.begin("cli", true);
  int n = p.getUChar("n", 0);
  bool ok = p.getBytes("c", c, sizeof c) == sizeof c;
  out->printf("dbg nvs cli {\"n\":%d,\"next\":%u,\"ids\":[", n, p.getUChar("next", 0));
  p.end();
  for (int i = 0; ok && i < n && i < 4; i++)
    out->printf("%s\"%02x%02x%02x%02x..\"", i ? "," : "", c[i].id[0], c[i].id[1], c[i].id[2], c[i].id[3]);
  out->println("]}");
  memset(c, 0, sizeof c);
}

static void wifi(bool scan) {
  out->printf("dbg wifi {\"mode\":\"%s\",\"status\":%d,\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"ip\":\"%s\","
                "\"gw\":\"%s\",\"mac\":\"%s\",\"ap_stations\":%d}\n",
                net_mode(), (int)WiFi.status(), WiFi.SSID().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel(),
                net_ip().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.macAddress().c_str(),
                (int)WiFi.softAPgetStationNum());
  if (!scan) return;
  if (net_setup_active()) {  // setup mode scans by itself: show its list
    NetScan list[16];
    bool running = false;
    int n = net_setup_scan(list, 16, &running);
    for (int i = 0; i < n; i++) out->printf("dbg wifi scan \"%s\" %d dBm%s\n", list[i].ssid, list[i].rssi, list[i].open ? " open" : "");
    out->printf("dbg wifi scan done %d%s\n", n, running ? " (a scan is running)" : "");
    return;
  }
  int n = WiFi.scanNetworks(false, false, false, 120);  // blocks loop() ~2 s, under the watchdog's 8 s
  for (int i = 0; i < n; i++)
    out->printf("dbg wifi scan \"%s\" %d dBm ch %d%s\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " open" : "");
  out->printf("dbg wifi scan done %d\n", n);
  WiFi.scanDelete();
}

bool dbg_command(const char *line) {
  String l = line;
  l.trim();
  if (l != "dbg" && !l.startsWith("dbg ")) return false;
  String arg = l.substring(3);
  arg.trim();
  if (arg == "state") state();
  else if (arg == "nvs") nvs();
  else if (arg == "wifi") wifi(false);
  else if (arg == "wifi scan") wifi(true);
  else if (arg == "btn short" || arg == "btn long" || arg == "btn double") {
    button_debug_press(arg == "btn long" ? DBG_BTN_LONG : arg == "btn double" ? DBG_BTN_DOUBLE : DBG_BTN_SHORT);
    out->printf("dbg ok %s\n", arg.c_str());
  } else if (arg == "reboot") {
    out->println("dbg ok reboot");
    if (out != &Serial) {  // the session's reply goes out first (dbg_take_reboot)
      rebootAsked = true;
      return true;
    }
    Serial.flush();
    delay(100);
    ESP.restart();
  } else if (arg == "forget") {  // the Wi-Fi and every paired computer: as new
    cli_forget_all();
    net_request("{\"mode\":\"ap\"}");
    out->println("dbg ok forget");
  } else {
    out->println("dbg err usage: dbg state | nvs | wifi [scan] | btn short | btn long | btn double | reboot | forget");
  }
  return true;
}

// ---- The same commands from the computer's session (`tentacle debug "dbg state"`): the answer is kept and read
// a page at a time, like `logs`. Not `dbg btn` (it would press BOOT from the network: pairing and the setup page
// are confirmed by a person at the board) and not `dbg forget` (it ends the session: unpair --board, net
// hotspot do that).
const size_t SESSION_OUT_MAX = 4096;
const size_t PAGE_MAX = 400;

class Collect : public Print {
 public:
  String text;
  size_t write(uint8_t c) override {
    if (text.length() >= SESSION_OUT_MAX) return 0;
    text += (char)c;
    return 1;
  }
};
static String sessionOut;

String dbg_page(const String &arg) {
  unsigned long from = 0;
  sscanf(arg.c_str(), "%lu", &from);
  size_t end = sessionOut.length();
  if (from > end) from = end;
  String page;
  size_t i = from;
  for (; i < end && page.length() < PAGE_MAX; i++) {
    char c = sessionOut[i];
    if (c == '\n') page += "\\n";
    else if (c == '\\') page += "\\\\";
    else if (c != '\r') page += c;
  }
  return "ok " + String((unsigned long)i) + " " + String((unsigned long)end) + " " + page;
}

String dbg_session(const String &args) {
  String l = args;
  l.trim();
  if (!l.startsWith("dbg")) l = "dbg " + l;
  String arg = l.substring(3);
  arg.trim();
  if (arg.startsWith("btn")) return "err dbg btn: USB serial only (it presses BOOT)";
  if (arg == "forget") return "err dbg forget: USB serial only (it ends the session)";
  Collect c;
  out = &c;
  dbg_command(l.c_str());
  out = &Serial;
  sessionOut = c.text;
  return dbg_page("0");
}

bool dbg_take_reboot() {
  bool r = rebootAsked;
  rebootAsked = false;
  return r;
}

static void marks() {
  static const char *tasks[] = {"loopTask", "cli_tcp", "link_tx", "button", "status", "rtsp", "portal", "video", "audio"};
  char s[220];
  int n = snprintf(s, sizeof s, "mem: heap %u (min %u, largest %u), psram %u, stack left:",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  for (const char *t : tasks) {
    TaskHandle_t h = xTaskGetHandle(t);
    if (h && n < (int)sizeof s - 24) n += snprintf(s + n, sizeof s - n, " %s %u", t, (unsigned)uxTaskGetStackHighWaterMark(h));
  }
  dbg_log("%s", s);
}

void dbg_poll() {
  static uint32_t lastMarks;
  static String net, ip, usbWas, ap;
  static int stopped = -1, tcp = -1, session = -1, pairing = -1, paired = -1, setup = -1;
  uint32_t now = millis();
  if (now - lastMarks >= MARKS_MS) {
    lastMarks = now;
    marks();
  }
  String v = net_mode(), i = net_ip();
  if (v != net || i != ip) { net = v; ip = i; dbg_log("state: net %s, ip %s", net.c_str(), ip.c_str()); }
  if ((v = usb()) != usbWas) { usbWas = v; dbg_log("state: usb %s", usbWas.c_str()); }
  if ((v = airplay()) != ap) { ap = v; dbg_log("state: airplay %s", ap.c_str()); }
  int x;
  if ((x = stop_active()) != stopped) { stopped = x; dbg_log("state: %s", x ? "STOPPED (HID output suppressed)" : "running"); }
  if ((x = cli_tcp_up()) != tcp) { tcp = x; dbg_log("state: tcp 7202 %s", x ? "connected" : "free"); }
  if ((x = cli_up()) != session) { session = x; dbg_log("state: session %s", x ? "up" : "none"); }
  if ((x = cli_pair_open()) != pairing) { pairing = x; dbg_log("state: pairing request %s", x ? "open" : "none"); }
  if ((x = cli_paired()) != paired) { paired = x; dbg_log("state: %d paired computer(s)", x); }
  if ((x = net_setup_active()) != setup) { setup = x; dbg_log("state: setup hotspot %s", x ? "on" : "off"); }
}
#endif
