// The status OLED and the battery gauge, see status_display.h. Shows name/firmware, net mode + IP, peers, and
// running/STOPPED; while something waits at the board: the setup Wi-Fi (with its QR code), the open pairing window,
// or the request BOOT confirms.
#include "status_display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include "button.h"
#include "cli_link.h"
#include "logbuf.h"
#include "netcfg.h"
#include "version.h"
#include "wifi_qr.h"
#include "src/setup/board_opts.h"
#include "src/ui/oled_text.h"

// SSD1306 modules answer at 0x3C (most) or 0x3D (address jumper). Pins: board_opts.h (XIAO D4 SDA, D5 SCL).
static const uint8_t OLED_ADDRS[] = {0x3C, 0x3D};
const uint16_t PROBE_TIMEOUT_MS = 20;  // per address, should the bus hang (a pin held low)
const uint32_t BATTERY_EVERY_MS = 2000;

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_oled(U8G2_R0, U8X8_PIN_NONE, BO_OLED_SCL, BO_OLED_SDA);
// Where the display may be wired: the documented XIAO pins (D4/D5 = GPIO5/6) first, then GPIO8/9, where the
// firmware up to 0.3.x looked (Wire.begin() without pins on the generic esp32s3 board), so a display wired by
// those builds keeps working.
static const uint8_t OLED_PINS[][2] = {{BO_OLED_SDA, BO_OLED_SCL}, {8, 9}};
static int wirePins = -1;  // index into OLED_PINS the bus is on (-1: not started)
static bool begun, present;
static volatile bool again;  // display_reconfigure(): look again from loop()
static int lang = BO_LANG_EN;
static volatile int batPct = -1;

// The setting and what is on the bus: present or not. Only loop() (and setup()) calls it.
static void look() {
  board_opts o;
  cfg_opts(&o);
  lang = bo_oled_lang(o.lang, web_lang().c_str());
  if (o.display == BO_DISPLAY_OFF) {
    if (present) g_oled.setPowerSave(1);  // blank; the I2C bus is left alone from here on
    if (present) board_log("display: off (setting)");
    present = false;
    return;
  }
  uint8_t addr = 0;
  int found = -1;
  // Once drawing, the pins stay (begin() bound the display to them); before that each pair is tried.
  int first = begun ? wirePins : 0, last = begun ? wirePins : (int)(sizeof OLED_PINS / sizeof OLED_PINS[0]) - 1;
  for (int i = first; i <= last && !addr; i++) {
    if (wirePins != i) {
      if (wirePins >= 0) Wire.end();
      Wire.begin(OLED_PINS[i][0], OLED_PINS[i][1]);
      Wire.setTimeOut(PROBE_TIMEOUT_MS);
      wirePins = i;
    }
    for (uint8_t a : OLED_ADDRS) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        addr = a;
        found = i;
        break;
      }
    }
  }
  if (!addr && o.display == BO_DISPLAY_ON) {  // "on": drawn whether it answers or not, on the documented pins
    addr = OLED_ADDRS[0];
    found = begun ? wirePins : 0;
  }
  if (found >= 0 && !begun) {
    if (wirePins != found) {
      Wire.end();
      Wire.begin(OLED_PINS[found][0], OLED_PINS[found][1]);
      Wire.setTimeOut(PROBE_TIMEOUT_MS);
      wirePins = found;
    }
    u8x8_SetPin(g_oled.getU8x8(), U8X8_PIN_I2C_DATA, OLED_PINS[found][0]);
    u8x8_SetPin(g_oled.getU8x8(), U8X8_PIN_I2C_CLOCK, OLED_PINS[found][1]);
  }
  bool was = present;
  present = addr != 0;
  if (!present) {
    if (was) board_log("display: no longer answers");
    else board_log("display: none on I2C (GPIO%d/%d, 8/9); setting %s", BO_OLED_SDA, BO_OLED_SCL, bo_display_name(o.display));
    return;
  }
  g_oled.setI2CAddress(addr * 2);
  if (!begun) g_oled.begin();
  else g_oled.setPowerSave(0);
  begun = true;
  if (!was) board_log("display: SSD1306 at 0x%02X on GPIO%d/%d (setting %s)", addr, OLED_PINS[wirePins][0],
                      OLED_PINS[wirePins][1], bo_display_name(o.display));
}

void display_begin() { look(); }
void display_reconfigure() { again = true; }
bool display_present() { return present; }
int battery_percent() { return batPct; }

static void sampleBattery() {
  static uint32_t last;
  if (last && millis() - last < BATTERY_EVERY_MS) return;
  last = millis();
  board_opts o;
  cfg_opts(&o);
  if (o.bat_pin == BO_BAT_NONE) {
    batPct = -1;
    return;
  }
  float v = analogReadMilliVolts(o.bat_pin) / 1000.0f * o.bat_div / 100.0f;  // battery volts
  int p = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);  // rough LiPo curve: 3.3 V = 0%, 4.2 V = 100%
  batPct = p < 0 ? 0 : (p > 100 ? 100 : p);
}

// The setup hotspot while it is up: its QR code on the left (dark modules on a lit square, as scanners expect, 2 px
// per module when it fits), its name, password and address on the right. false: no code to show.
static bool showSetupQr() {
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
  const char *lines[] = {oled_text(lang, OT_QR_TITLE), ssid.c_str(), oled_text(lang, OT_QR_PASS), pass.c_str(), ip.c_str()};
  const int ys[] = {8, 20, 34, 46, 61};
  for (int i = 0; i < 5; i++) {  // 5x7 where it fits, else 4x6 (Tentacle-1A2B is 13 characters)
    g_oled.setFont(u8g2_font_5x7_tr);
    if (g_oled.getStrWidth(lines[i]) > 128 - 66) g_oled.setFont(u8g2_font_4x6_tr);
    g_oled.drawStr(66, ys[i], lines[i]);
  }
  g_oled.sendBuffer();
  return true;
}

void display_poll() {  // ~2 Hz from loop()
  static uint32_t last = 0;
  sampleBattery();
  if (again) {
    again = false;
    look();
  }
  if (!present || millis() - last < 500) return;
  last = millis();
  char l[4][48];
  char c1[CF_TEXT], c2[CF_TEXT];
  uint32_t left = 0;
  const char *state = oled_text(lang, stop_active() ? OT_STOPPED : OT_RUNNING);
  if (confirm_open(c1, c2, &left)) {  // a request waits for the BOOT button: what, and how long still
    snprintf(l[0], sizeof l[0], "%s", oled_text(lang, OT_CONFIRM));
    oled_ascii(oled_request(lang, c1), l[1], sizeof l[1]);  // a computer's or a network's name may be UTF-8
    oled_ascii(oled_request(lang, c2), l[2], sizeof l[2]);
    snprintf(l[3], sizeof l[3], oled_text(lang, OT_LEFT), (unsigned long)((left + 999) / 1000));
  } else if (pair_window_open(&left)) {  // BOOT was double-pressed: a computer may ask to pair now
    snprintf(l[0], sizeof l[0], "%s", oled_text(lang, OT_PAIR_OPEN));
    snprintf(l[1], sizeof l[1], "%s", oled_text(lang, OT_PAIR_RUN));
    snprintf(l[2], sizeof l[2], "tentacle pair");
    snprintf(l[3], sizeof l[3], oled_text(lang, OT_LEFT), (unsigned long)((left + 999) / 1000));
  } else if (net_setup_active()) {  // no Wi-Fi stored: how to reach the setup page
    if (showSetupQr()) return;
    snprintf(l[0], sizeof l[0], "%s", oled_text(lang, OT_SETUP_JOIN));
    snprintf(l[1], sizeof l[1], "%s", net_ap_ssid().c_str());
    snprintf(l[2], sizeof l[2], oled_text(lang, OT_PASS), net_ap_pass().c_str());
    snprintf(l[3], sizeof l[3], "http://%s", net_ip().c_str());
  } else {
    snprintf(l[0], sizeof l[0], "%s %s", board_name().c_str(), FW_VERSION);
    snprintf(l[1], sizeof l[1], oled_text(lang, OT_NET), net_mode(), net_ip().c_str());
    // the stored network is out of reach, or its password changed: BOOT held 10 s sets up a new one
    if (!net_up()) snprintf(l[2], sizeof l[2], "%s", oled_text(lang, OT_NO_WIFI));
    else snprintf(l[2], sizeof l[2], oled_text(lang, OT_PEERS), net_peers(), (int)cli_up());
    int bat = batPct;
    if (bat >= 0) snprintf(l[3], sizeof l[3], oled_text(lang, OT_BAT), state, bat);
    else snprintf(l[3], sizeof l[3], "%s", state);
  }
  g_oled.clearBuffer();
  g_oled.setFont(u8g2_font_6x12_tr);
  for (int i = 0; i < 4; i++) g_oled.drawStr(0, 12 + 16 * i, l[i]);
  g_oled.sendBuffer();
}
