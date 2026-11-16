// HID bridge: an ESP32-S3 (XIAO, 8 MB PSRAM) plugged into the iPhone. It is the phone's
// USB mouse + keyboard and an AirPlay screen-mirroring receiver. It holds no logic: the computer's CLI decides
// (docs/cli-protocol.md). The CLI finds the board on the Wi-Fi by mDNS (_tentacle._tcp), pairs and talks on
// TCP 7202 (cli_tcp.h). There is no Bluetooth and no phone app.
//
// HID commands (the board answers "ok" or "err <why>"):
//   ping | pace | home | move <dx> <dy> [speed] | press | release | click
//   abs <x> <y> [step] (flash.sh --abs builds: 0-32767 across the screen; `home` and `move` still work)
//   scroll <v> <h> | type <ascii text> | key <combo, e.g. cmd+h, enter>
//   homescreen (the consumer-control Home key, "AC Home": iOS goes to the Home Screen, like a keyboard's home key)

#include <esp_task_wdt.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_core_dump.h>
#include <esp_app_desc.h>
#include "USB.h"
#include "USBHIDMouse.h"
#include "abs_mouse.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "tusb.h"
#include <mdns.h>
#include "link.h"
#include "netcfg.h"
#include "video_link.h"
#include "audio_link.h"
#include "cli_link.h"
#include "cli_tcp.h"
#include "version.h"
#include "src/airplay/airplay.h"
#include "src/hid/pace.h"
#include "src/cli/cli_proto.h"
#include "logbuf.h"
#include "button.h"
#include "dbg.h"
#include "ota.h"
#include "web_server.h"

const int STEP = 10;      // pointer speed per report while moving (any direction, see pace.h)
const int ABS_STEP = 245;      // of 32767, per STEP_MS while dragging with `abs`: the relative mouse's speed
const int ABS_SETTLE_MS = 60;  // between an `abs` move and the press after it: iOS ignores X/Y in a button report
const int STEP_MS = 8;    // report interval; constant pace keeps iOS acceleration predictable
const int HOME_REPORTS = 25;  // per axis
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0  // 1: echo commands and the AirPlay handshake over USB serial
#endif

const int WATCHDOG_MS = 8000;  // a hung loop (e.g. blocked USB) reboots the board instead of staying offline

#if ABS_POINTER
AbsMouse mouse;  // flash.sh --abs: same calls as USBHIDMouse, plus `abs`
#else
USBHIDMouse mouse;
#endif
USBHIDKeyboard keyboard;
USBHIDConsumerControl consumer;
const uint16_t AC_HOME = 0x0223;  // HID consumer page: AC Home
static String receiverName = BOARD_NAME_TEXT;  // startAirplay() adds the board's suffix: "Tentacle 1A2B"

void movePaced(long dx, long dy, int step = STEP) {
  pace_t p;
  pace_start(&p, dx, dy, step);
  int sx, sy;
  while (pace_next(&p, &sx, &sy)) {
    mouse.move(sx, sy);
    delay(STEP_MS);
  }
}

uint8_t keyCode(const String &k) {
  if (k == "enter") return KEY_RETURN;
  if (k == "esc") return KEY_ESC;
  if (k == "tab") return KEY_TAB;
  if (k == "backspace") return KEY_BACKSPACE;
  if (k == "space") return ' ';
  if (k == "up") return KEY_UP_ARROW;
  if (k == "down") return KEY_DOWN_ARROW;
  if (k == "left") return KEY_LEFT_ARROW;
  if (k == "right") return KEY_RIGHT_ARROW;
  if (k == "cmd") return KEY_LEFT_GUI;
  if (k == "ctrl") return KEY_LEFT_CTRL;
  if (k == "alt") return KEY_LEFT_ALT;
  if (k == "shift") return KEY_LEFT_SHIFT;
  return k.length() == 1 ? k[0] : 0;
}

bool pressCombo(String combo) {
  if (combo == "eject") {
    // iOS toggles the on-screen keyboard once per Consumer Control Eject press.
    consumer.press(CONSUMER_CONTROL_EJECT);
    delay(40);
    consumer.release();
    return true;
  }
  combo += '+';
  for (int i; (i = combo.indexOf('+')) >= 0; combo = combo.substring(i + 1)) {
    uint8_t c = keyCode(combo.substring(0, i));
    if (!c) { keyboard.releaseAll(); return false; }
    keyboard.press(c);
  }
  delay(30);
  keyboard.releaseAll();
  return true;
}

// A locked or sleeping phone suspends USB; sending HID reports then can stall. Ask it to wake up instead,
// and report "asleep" right away if it does not, so the board never hangs.
bool usbReady() {
  if (tud_suspended()) {
    tud_remote_wakeup();
    for (int i = 0; i < 50 && tud_suspended(); i++) delay(10);
  }
  return tud_mounted() && !tud_suspended();
}

// `press_for MS`: the button goes up by itself after MS (a long press, or a press then moves: a drag), so a lost
// `release` never leaves it held. 0: nothing pending.
static uint32_t holdUntil;
const long HOLD_MAX_MS = 60000;

// ---- Physical safety controls (SAFETY_CONTROLS, on by default; flash.sh --no-safety builds without) ----------
// A short press of the BOOT button toggles a "stopped" state that suppresses all HID output (the agent can move
// and type nothing), and the LED shows it: 0.5 Hz heartbeat = running, solid = stopped. The button, the LED and
// the button's other jobs (confirming a request, forgetting the Wi-Fi) are in button.h: stop_active().

// ---- Optional 128x64 SSD1306 status display + battery gauge (flash --display) ---------------------
// Shows name/firmware, net mode + IP, peers, and RUNNING/STOPPED on a cheap I2C OLED. Off by default;
// needs the OLED wired to the board's I2C pins. Battery needs BATTERY_PIN. See docs/hardware-addons.md.
#ifndef STATUS_DISPLAY
#define STATUS_DISPLAY 0
#endif
#if STATUS_DISPLAY
#include <Wire.h>
#include <U8g2lib.h>
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_oled(U8G2_R0, U8X8_PIN_NONE);
#ifdef BATTERY_PIN
#ifndef BATTERY_DIVIDER
#define BATTERY_DIVIDER 2.0f  // LiPo-to-ADC divider ratio (two equal resistors = 2.0)
#endif
static int battery_pct() {
  float v = analogReadMilliVolts(BATTERY_PIN) / 1000.0f * BATTERY_DIVIDER;  // battery volts
  int p = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);  // rough LiPo curve: 3.3 V = 0%, 4.2 V = 100%
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}
#endif
#include "wifi_qr.h"
// The setup hotspot while it is up: its QR code on the left (dark modules on a lit square, as scanners expect, 2 px
// per module when it fits), its name, password and address on the right. false: no code to show.
static bool display_setup_qr() {
  int n = wifi_qr_size(), s = n && n * 2 <= 60 ? 2 : 1;  // keep at least 2 px of quiet zone on each side
  if (!n || n * s > 60) return false;
  int o = (64 - n * s) / 2;
  g_oled.clearBuffer();
  g_oled.drawBox(0, 0, 64, 64);
  g_oled.setDrawColor(0);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (wifi_qr_dark(x, y)) g_oled.drawBox(o + x * s, o + y * s, s, s);
  g_oled.setDrawColor(1);
  String ssid = net_ap_ssid(), pass = net_ap_pass(), ip = net_ip();
  const char *lines[] = {"Wi-Fi setup", ssid.c_str(), "Password:", pass.c_str(), ip.c_str()};
  const int ys[] = {8, 20, 34, 46, 61};
  for (int i = 0; i < 5; i++) {  // 5x7 where it fits, else 4x6 (Tentacle-1A2B is 13 characters)
    g_oled.setFont(u8g2_font_5x7_tr);
    if (g_oled.getStrWidth(lines[i]) > 128 - 66) g_oled.setFont(u8g2_font_4x6_tr);
    g_oled.drawStr(66, ys[i], lines[i]);
  }
  g_oled.sendBuffer();
  return true;
}
static void display_begin() {
  Wire.begin();
  g_oled.begin();
}
static void display_poll() {  // ~2 Hz from loop()
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  char l1[32], l2[32], l3[32], l4[32];
  char c1[CF_TEXT], c2[CF_TEXT];
  uint32_t left = 0;
  const char *state = stop_active() ? "STOPPED" : "running";
  if (confirm_open(c1, c2, &left)) {  // a request waits for the BOOT button: what, and how long still
    snprintf(l1, sizeof l1, "PRESS BOOT TO CONFIRM");
    snprintf(l2, sizeof l2, "%s", c1);
    snprintf(l3, sizeof l3, "%s", c2);
    snprintf(l4, sizeof l4, "%lu s left", (unsigned long)((left + 999) / 1000));
  } else if (pair_window_open(&left)) {  // BOOT was double-pressed: a computer may ask to pair now
    snprintf(l1, sizeof l1, "PAIRING OPEN");
    snprintf(l2, sizeof l2, "On the computer run:");
    snprintf(l3, sizeof l3, "tentacle pair");
    snprintf(l4, sizeof l4, "%lu s left", (unsigned long)((left + 999) / 1000));
  } else if (net_setup_active()) {  // no Wi-Fi stored: how to reach the setup page
    if (display_setup_qr()) return;
    snprintf(l1, sizeof l1, "Wi-Fi setup: join");
    snprintf(l2, sizeof l2, "%s", net_ap_ssid().c_str());
    snprintf(l3, sizeof l3, "PW %s", net_ap_pass().c_str());
    snprintf(l4, sizeof l4, "http://%s", net_ip().c_str());
  } else {
    snprintf(l1, sizeof l1, "%s %s", board_name().c_str(), FW_VERSION);
    snprintf(l2, sizeof l2, "net %s %s", net_mode(), net_ip().c_str());
    if (!net_up()) snprintf(l3, sizeof l3, "no Wi-Fi: BOOT 10s=new");  // the stored network is out of reach, or its password changed
    else snprintf(l3, sizeof l3, "peers %d  cli %d", net_peers(), (int)cli_up());
#ifdef BATTERY_PIN
    snprintf(l4, sizeof l4, "%s  bat %d%%", state, battery_pct());
#else
    snprintf(l4, sizeof l4, "%s", state);
#endif
  }
  g_oled.clearBuffer();
  g_oled.setFont(u8g2_font_6x12_tr);
  g_oled.drawStr(0, 12, l1);
  g_oled.drawStr(0, 28, l2);
  g_oled.drawStr(0, 44, l3);
  g_oled.drawStr(0, 60, l4);
  g_oled.sendBuffer();
}
#endif

String run(const String &line) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0, c = STEP;
  sscanf(arg.c_str(), "%ld %ld %ld", &a, &b, &c);

  if (cmd == "ping") return "ok";
#if ABS_POINTER
  if (cmd == "move" || cmd == "flick") {  // emulated on the absolute mouse; never further than across the screen
    a = constrain(a, -AbsMouse::MAX_MOVE, AbsMouse::MAX_MOVE);
    b = constrain(b, -AbsMouse::MAX_MOVE, AbsMouse::MAX_MOVE);
  }
#endif
  if (cmd == "pace") return "ok " + String(PACE_VERSION);  // how moves are paced: the CLI recalibrates on a change
  if (stop_active()) return "err stopped (safety button)";  // ping/pace still answer, all input is suppressed
  if (!usbReady()) return "err phone usb asleep (unlock the phone)";
  if (cmd == "reset") {
    if (arg.length()) return "err usage: reset";
    // Release held inputs in place so the user keeps the current page and text.
    mouse.release(0xff);
    keyboard.releaseAll();
    consumer.release();
    holdUntil = 0;
    return "ok";
  }
  if (cmd == "home") {  // slam the pointer into the top-left corner: left first, then up along the left edge,
    // so it never rides the top edge from the right (that opened Control Center, 2026-11-06)
    for (int i = 0; i < HOME_REPORTS; i++) { mouse.move(-127, 0); delay(2); }
    for (int i = 0; i < HOME_REPORTS; i++) { mouse.move(0, -127); delay(2); }
    return "ok";
  }
  if (cmd == "move") { movePaced(a, b, constrain(c, 1, 40)); return "ok"; }  // c: speed per report (calibration)
#if ABS_POINTER
  if (cmd == "abs") {  // absolute position, 0-32767 across the screen; [step]: per report while dragging
    if (mouse.held()) {
      // Lists need the glide (a jump is a flick, and they fly); system gestures need a flick: a glided home swipe
      // took ~1 s and opened the app switcher instead (2026-11-07). The caller sends a big step for those.
      long step = sscanf(arg.c_str(), "%*ld %*ld %ld", &c) == 1 ? constrain(c, AbsMouse::MIN_STEP, AbsMouse::FULL) : ABS_STEP;
      mouse.glideTo(a, b, step, STEP_MS);
    } else {
      mouse.moveTo(a, b);
      delay(ABS_SETTLE_MS);
    }
    return "ok";
  }
#endif
  if (cmd == "press") { mouse.press(MOUSE_LEFT); holdUntil = 0; return "ok"; }
  if (cmd == "press_for") {  // cy41 #103: a held long press (a 30 s hold used to come up after 5 s)
    if (a < 1 || a > HOLD_MAX_MS) return "err usage: press_for MS (1-60000)";
    mouse.press(MOUSE_LEFT);
    holdUntil = millis() + a;
    if (!holdUntil) holdUntil = 1;
    return "ok";
  }
  if (cmd == "release") { mouse.release(MOUSE_LEFT); holdUntil = 0; return "ok"; }
  if (cmd == "flick") {  // press, move, release with no pause between: a lingering stroke from the home bar
    // opened the app switcher instead of the Home Screen (iddzz 2026-11-12)
    mouse.press(MOUSE_LEFT);
    movePaced(a, b, constrain(c, 1, 40));
    mouse.release(MOUSE_LEFT);
    holdUntil = 0;
    return "ok";
  }
  if (cmd == "click") { mouse.press(MOUSE_LEFT); delay(60); mouse.release(MOUSE_LEFT); holdUntil = 0; return "ok"; }
  if (cmd == "scroll") { mouse.move(0, 0, a, b); return "ok"; }
  if (cmd == "type") { keyboard.print(arg); return "ok"; }
  if (cmd == "key") return pressCombo(arg) ? "ok" : "err unknown key";
  if (cmd == "homescreen") { consumer.press(AC_HOME); delay(40); consumer.release(); return "ok"; }
  return "err unknown command";
}

static bool cliHolds;  // the computer's session holds the mouse button: let go of when the session ends (loop())

// An HID command from who (FROM_CLI or FROM_SERIAL).
String drive(const String &line, char who) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  if (who == FROM_CLI)
    if (const char *busy = web_cli_gate(line.c_str())) return busy;  // the web interface drives (web_server.h)
  String reply = run(line);
  if (reply == "ok") {
    if (cmd == "press" || cmd == "press_for") cliHolds = who == FROM_CLI;
    else if (cmd == "release" || cmd == "click" || cmd == "flick" || cmd == "reset") cliHolds = false;
  }
  return reply;
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_TASK_WDT: case ESP_RST_INT_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_SW: return "software";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

const char *usbState() {
  if (!tud_mounted()) return "off";
  return tud_suspended() ? "asleep" : "ok";
}

// The last crash saved in the core dump partition (kept until the next one): task, PC, backtrace. Read at boot,
// so a crash on a board plugged into the phone can be looked up from the CLI (`crash`) without a serial cable.
static String lastCrash = "none";

void readCrash() {
  auto *c = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  if (c && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(c) == ESP_OK) {
    char b[200];
    int n = snprintf(b, sizeof b, "%.16s pc %08lx bt", c->exc_task, (unsigned long)c->exc_pc);
    for (int i = 0; i < (int)c->exc_bt_info.depth && i < 12 && n < (int)sizeof b - 10; i++)
      n += snprintf(b + n, sizeof b - n, " %08lx", (unsigned long)c->exc_bt_info.bt[i]);
    // Is it this build's? A dump stays until the next crash; erase it once read, so the next one is fresh
    // (2026-11-13: an mdns dump of unknown age was all there was after a reset).
    char sha[17] = {0};
    esp_app_get_elf_sha256(sha, sizeof sha);
    bool mine = !strncmp((const char *)c->app_elf_sha256, sha, 16);
    lastCrash = String(b) + (mine ? " (this build)" : " (older build)");
    esp_core_dump_image_erase();
  }
  free(c);
}

// The session's last replies, so a computer that lost one can ask what happened (cy41 #105: a `move` done in
// 848 ms whose reply never arrived; the caller timed out at 6.3 s and could not tell). `result <id>` ->
// `ok <the reply>`, read only.
struct Recent {
  String id, reply;
};
static Recent recent[8];
static int recentNext;

void keepReply(const String &id, const String &reply) {
  recent[recentNext] = {id, reply};
  recentNext = (recentNext + 1) % 8;
}

String recentReply(const String &id) {
  if (!id.length() || id.indexOf(' ') >= 0) return "err usage: result ID";
  for (int i = 1; i <= 8; i++) {
    const Recent &r = recent[(recentNext - i + 8) % 8];
    if (r.id == id) return "ok " + r.reply;
  }
  return "err unknown id (not among the last 8 replies)";
}

// Board-level commands from the computer's session or USB serial (who); everything else is an HID command for
// run(). What only makes sense with a session (the video, the audio, the session's own replies) is refused on
// USB serial.
const char *const NEEDS_SESSION = "err needs the computer's session (not on USB serial)";

String board(const String &line, const String &id, char who) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  int n = sscanf(arg.c_str(), "%ld %ld", &a, &b);
  if (who != FROM_CLI && (cmd == "stream" || cmd == "snap" || cmd == "bench" || cmd == "listen" || cmd == "result" ||
                         cmd == "update" || cmd == "dbg" || cmd == "dbgout" ||
                         (cmd == "logs" && arg.startsWith("follow"))))
    return NEEDS_SESSION;
  // Firmware update over the session (ota.h): signed releases only (unsigned too in a debug build).
  if (cmd == "update") return ota_command(arg, id);
  // Debugging over Wi-Fi, debug builds only: the serial `dbg` commands, and the log as it is written.
  if (cmd == "dbg" || cmd == "dbgout" || (cmd == "logs" && arg.startsWith("follow"))) {
#if TENTACLE_DEBUG
    if (cmd == "dbgout") return dbg_page(arg);
    if (cmd == "logs") {
      if (arg != "follow on" && arg != "follow off") return "err usage: logs follow on|off";
      link_follow(arg == "follow on");
      return "ok";
    }
    String reply = dbg_session(arg);
    if (dbg_take_reboot()) {
      link_reply(id + " " + reply);
      link_flush();
      vTaskDelay(pdMS_TO_TICKS(300));
      ESP.restart();
    }
    return reply;
#else
    return "err debug build only";
#endif
  }
  if (cmd == "stream") {
    if (arg != "on" && arg != "off") return "err usage: stream on|off";
    video_stream(arg == "on");
    return "ok";
  }
  if (cmd == "log") {  // a marker in the board's log (`logs`) and on USB serial
    if (!arg.length()) return "err usage: log TEXT";
    board_log("log %s", arg.c_str());
    return "ok";
  }
  if (cmd == "net" && who == FROM_CLI) {  // the board's Wi-Fi for the CLI's link (docs/cli-protocol.md, "Wi-Fi link")
    if (arg.length()) {
      if (!arg.startsWith("{")) return "err usage: net [{\"mode\":\"ap\"} | {\"mode\":\"sta\",\"ssid\":...,\"pass\":...}]";
      net_request(arg);  // applied by net_poll(): the reply goes out first
      return "ok";
    }
    return "ok " + net_json();
  }
  // Phone audio for the CLI (docs/cli-protocol.md, "Phone audio"): the raw AirPlay audio, transcribed there.
  if (cmd == "listen") {
    if (arg != "raw on" && arg != "raw off") return "err usage: listen raw on|off";
    cli_audio(arg == "raw on");
    return "ok";
  }
  if (cmd == "result") return recentReply(arg);
  if (cmd == "logs") return board_log_page(arg);  // the RAM log, a page at a time (logbuf.h)
  if (cmd == "vdbg") return "ok " + video_debug();  // why a frozen screen stays frozen (video_link.cpp)
  if (cmd == "crash") return String("ok rst ") + resetReason() + ", last dump: " + lastCrash;
  if (cmd == "snap") { video_snap(id); return ""; }  // replies when the GOP is sent
  if (cmd == "bench") {
    if (n < 1 || a <= 0 || a > 4096) return "err usage: bench KB (1-4096)";
    video_bench(id, a);
    return "";
  }
  if (cmd == "res") {
    if (n < 2 || a < 160 || b < 160 || a > 1920 || b > 2560) return "err usage: res W H";
    cfg_set_res(a, b);
    airplay_set_display(a, b, cfg_fps());
    return "ok";
  }
  if (cmd == "fps") {
    if (n < 1 || a < 1 || a > 60) return "err usage: fps N (1-60)";
    cfg_set_fps(a);
    airplay_set_display(cfg_width(), cfg_height(), a);
    return "ok";
  }
  if (cmd == "txpower") {  // the Wi-Fi's TX power, an A/B knob in 0.25 dBm (8-84), not stored (netcfg.cpp sets the start)
    if (n < 1 || a < 8 || a > 84 || esp_wifi_set_max_tx_power(a) != ESP_OK) return "err usage: txpower 8-84";
    return "ok";
  }
  if (cmd == "unpair") {
    // Forgets every paired computer and drops the link: the computer has to `pair` again (with the BOOT button).
    // A session gets its `ok` first, sealed with the session that then ends.
    if (who != FROM_CLI) {
      cli_forget_all();
      return "ok";
    }
    link_reply(id + " ok");
    link_flush();                    // let the `ok` go out
    vTaskDelay(pdMS_TO_TICKS(150));  // ... and onto the network before the connection closes
    cli_forget_all();
    return "";
  }
  return drive(line, who);
}

#if ABS_POINTER
#define ABS_FIELD "\"abs\":1,"  // in the full and the short status: the CLI sends `abs` and skips calibration
#else
#define ABS_FIELD ""
#endif

String statusJson(bool full) {
  static const char *apStates[] = {"idle", "connected", "streaming"};
  VideoStats v = video_stats();
  airplay_audio_stats au = airplay_audio();
  char s[768];
  if (full) {
    // aud: AirPlay audio packets, sound segments, a segment going on, sender volume (dB), records lost on the way
    // to the computer, the computer asked for the raw audio. link: bytes to the computer, last second.
    snprintf(s, sizeof s,
             "{\"fw\":\"%s\"," ABS_FIELD "\"up\":%lu,\"rst\":\"%s\",\"heap\":%u,\"psram\":%u,\"usb\":\"%s\",\"net\":\"%s\",\"ip\":\"%s\",\"sta\":%d,"
             "\"ap\":\"%s\",\"w\":%u,\"h\":%u,\"kbps\":%lu,\"fps\":%.1f,\"idr\":%lu,\"gop\":%lu,\"vage\":%lu,\"ovf\":%lu,\"fz\":%d,"
             "\"link\":{\"kbps\":%lu},\"aud\":{\"pk\":%lu,\"seg\":%lu,\"snd\":%d,\"vol\":%.0f,\"drop\":%lu,\"raw\":%d},\"lost\":%lu}",
             FW_VERSION, millis(), resetReason(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), usbState(), net_mode(), net_ip().c_str(), net_peers(),
             apStates[airplay_state()], cfg_width(), cfg_height(), (unsigned long)v.kbps, v.fps, (unsigned long)v.idr_ms,
             (unsigned long)v.gop_bytes, (unsigned long)v.age_ms, (unsigned long)v.overflows, v.frozen,
             (unsigned long)cli_tcp_kbps(), (unsigned long)au.packets, (unsigned long)au.segments, au.sound, au.volume,
             (unsigned long)audio_drops(), cli_audio_wanted(), (unsigned long)v.lost);
    return s;
  }
  // The short form, should the full one ever outgrow a reply (link.cpp): what the CLI needs to go on.
  snprintf(s, sizeof s, "{\"fw\":\"%s\"," ABS_FIELD "\"up\":%lu,\"rst\":\"%s\",\"usb\":\"%s\",\"net\":\"%s\",\"ip\":\"%s\",\"ap\":\"%s\",\"fz\":%d,\"lost\":%lu}",
           FW_VERSION, millis(), resetReason(), usbState(), net_mode(), net_ip().c_str(), apStates[airplay_state()], v.frozen,
           (unsigned long)v.lost);
  return s;
}

void statusTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (cli_up()) link_status(statusJson(true), statusJson(false));
  }
}

// AirPlay log lines: to USB serial (debug builds) and the RAM log.
void logLine(const char *msg) {
#if DEBUG_SERIAL
  Serial.printf("[%lu.%03lu] A %s\n", millis() / 1000, millis() % 1000, msg);  // A: the AirPlay receiver's lines
#endif
  board_log_keep(msg);
}

void startAirplay() {
  static airplay_cfg cfg;
  // What the iPhone lists under Screen Mirroring: the same name the CLI sees ("Tentacle 1A2B"), so two boards differ.
  receiverName = board_name() + " " + board_suffix();
  cfg.name = receiverName.c_str();
  esp_read_mac(cfg.hwaddr, ESP_MAC_WIFI_STA);
  cfg_ed25519_seed(cfg.ed25519_seed);
  cfg.width = cfg_width();
  cfg.height = cfg_height();
  cfg.max_fps = cfg_fps();
  cfg.on_config = video_on_config;
  cfg.on_au = video_on_au;
  cfg.on_audio = audio_on_frame;  // the CLI's raw audio (`listen raw on`)
  cfg.log = logLine;
  cfg.on_lost = video_on_lost;
  if (!airplay_start(&cfg)) board_log("airplay: start failed");
}

// How the CLI finds the board on the Wi-Fi (docs/cli-protocol.md, "Discovery"): one more service on the mDNS
// responder the AirPlay receiver started (src/airplay/mdns.c; same host name, no second responder). The TXT record
// holds nothing secret: the board id, its name, the firmware version, how many computers are paired, and whether
// the pairing window is open (so the CLI can say "double-press BOOT first" before it connects).
static const char *const CLI_SERVICE = "_tentacle";
static int advertisedPaired = -1, advertisedOpen = 0;

static void advertiseCli() {
  static char id[13], name[40], paired[4];
  uint8_t m[6];
  cli_board_id(m);
  snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  snprintf(name, sizeof name, "%s %s", board_name().c_str(), board_suffix().c_str());
  advertisedPaired = cli_paired();
  snprintf(paired, sizeof paired, "%d", advertisedPaired);
  advertisedOpen = 0;  // advertiseState() sets it once the window is open
  mdns_txt_item_t txt[] = {{"id", id}, {"name", name}, {"fw", FW_VERSION}, {"paired", paired}, {"open", "0"}, {"v", "1"}};
  esp_err_t e = mdns_service_add(name, CLI_SERVICE, "_tcp", CLI_TCP_PORT, txt, sizeof txt / sizeof txt[0]);
  if (e != ESP_OK) {
    advertisedPaired = -1;  // nothing to keep up to date
    board_log("mdns: %s._tcp failed (%d): the CLI needs --host <ip>", CLI_SERVICE, (int)e);
  } else board_log("mdns: %s._tcp on port %d as \"%s\"", CLI_SERVICE, CLI_TCP_PORT, name);
}

static void advertiseState() {  // loop(): the TXT record follows pair / unpair and the pairing window; so does the log
  static bool wasOpen;
  bool open = pair_window_open();
  if (open != wasOpen) {
    wasOpen = open;
    board_log(open ? "pair: window open for 60 s (BOOT double press)" : "pair: window closed");
  }
  if (advertisedPaired < 0) return;  // the service is not there
  int n = cli_paired();
  if (n != advertisedPaired) {
    char paired[4];
    snprintf(paired, sizeof paired, "%d", n);
    if (mdns_service_txt_item_set(CLI_SERVICE, "_tcp", "paired", paired) == ESP_OK) advertisedPaired = n;
  }
  if ((int)open != advertisedOpen && mdns_service_txt_item_set(CLI_SERVICE, "_tcp", "open", open ? "1" : "0") == ESP_OK)
    advertisedOpen = open;
}

void setup() {
  Serial.begin(115200);
#if DEBUG_SERIAL
  for (int i = 0; i < 30 && !Serial; i++) delay(100);  // give a serial monitor 3 s to catch the boot lines
#endif
  mouse.begin();
  keyboard.begin();
  consumer.begin();
  USB.productName("Tentacle");
  USB.begin();
  esp_task_wdt_config_t wdt = {.timeout_ms = WATCHDOG_MS, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);
  readCrash();
#if TENTACLE_DEBUG
  dbg_begin(resetReason(), lastCrash.c_str());
#endif
  button_begin();
#if STATUS_DISPLAY
  display_begin();
#endif

  net_begin();
  if (!video_begin()) board_log("video: no PSRAM for the GOP buffer");
  link_begin();
  cli_begin();
  if (!cli_tcp_begin()) board_log("cli: Wi-Fi link failed to start");
  startAirplay();
  advertiseCli();  // after AirPlay: it starts the mDNS responder
  ota_setup();     // after the log is up: says whether this firmware is new and on trial
  web_begin();     // the web interface: starts once the board is in a Wi-Fi
  xTaskCreatePinnedToCore(statusTask, "status", 4096, NULL, 1, NULL, 1);
  board_log("boot %s fw %s: free internal heap %u (largest %u), PSRAM %u, reset %s\n", receiverName.c_str(), FW_VERSION,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), resetReason());
}

void loop() {
  static bool cliLinked = false;
  esp_task_wdt_reset();
#if STATUS_DISPLAY
  display_poll();
#endif
  net_poll();
  advertiseState();
  ota_poll();
#if TENTACLE_DEBUG
  dbg_poll();
#endif
  bool cli = cli_up();
  // A gesture cut off mid-way must not leave the button held (long press): the session that went held it.
  if (cliLinked && !cli) {
    if (cliHolds) mouse.release(MOUSE_LEFT);
    cliHolds = false;
    video_stream(false);
  }
  cliLinked = cli;
  if (holdUntil && (int32_t)(millis() - holdUntil) >= 0) {  // press_for ran out
    mouse.release(MOUSE_LEFT);
    holdUntil = 0;
    cliHolds = false;
  }

  String line;
  // USB serial takes the same `<id> <command>` lines, for debugging and recovery. Never wait for the rest of a line:
  // bytes without a newline (esptool's sync, say) held the loop until the watchdog reset the board.
  static String serialLine;
  bool serialDone = false;
  while (Serial.available() && !serialDone) {
    char ch = Serial.read();
    if (ch == '\n') serialDone = true;
    else if (serialLine.length() < 512) serialLine += ch;
  }
  if (serialDone) {
    line = serialLine;
    serialLine = "";
    line.trim();
#if TENTACLE_DEBUG
    if (dbg_command(line.c_str())) return;
#endif
    if (line.startsWith("0 net ")) { net_request(line.substring(6)); Serial.println("0 ok"); return; }
    if (line == "0 netinfo") { Serial.println("0 " + net_json()); return; }  // what `net` answers the CLI
    if (line == "0 setup") { Serial.println(net_setup_line()); return; }  // the setup hotspot's name and password (flash.sh reads it)
    if (line == "0 status") { Serial.println("0 " + statusJson(true)); return; }
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    Serial.println(id + " " + board(sp < 0 ? "" : line.substring(sp + 1), id, FROM_SERIAL));
    return;
  }
  if (web_loop()) return;  // the web interface's input, one command at a time between the computer's
  if (!link_pop_cmd(line, web_wait_ms(50))) return;
  int sp = line.indexOf(' ');
  String id = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
#if DEBUG_SERIAL
  unsigned long started = millis();
#endif
  String reply = id.toInt() || id == "0" ? board(rest, id, FROM_CLI) : "err missing request id";
  if (reply.length()) {
    if (!rest.startsWith("result")) keepReply(id, reply);
    link_reply(id + " " + reply);
  }
#if DEBUG_SERIAL
  // A Wi-Fi password never goes to the serial log: `net {...}` is shown without its argument.
  Serial.printf("[%lu.%03lu] C %s -> %s (%lu ms)\n", started / 1000, started % 1000,
                rest.startsWith("net ") ? (id + " net {...}").c_str() : line.c_str(), reply.c_str(), millis() - started);
#endif
#if TENTACLE_DEBUG
  // ... and into the RAM log, for `tentacle logs --board --follow`; not the log's own reads, nor typed text
  if (!rest.startsWith("logs") && !rest.startsWith("dbgout")) {
    char kept[200];
    String shown = rest.startsWith("net ") ? id + " net {...}" : rest.startsWith("type ") ? id + " type ..." : line;
    snprintf(kept, sizeof kept, "C %s -> %.80s (%lu ms)", shown.c_str(), reply.c_str(), millis() - started);
    board_log_keep(kept);
  }
#endif
}
