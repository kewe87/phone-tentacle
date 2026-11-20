// The status OLED and the battery gauge, see status_display.h. Shows name/firmware, net mode + IP, peers, and
// running/STOPPED; while something waits at the board: the setup Wi-Fi (with its QR code), the open pairing window,
// or the request BOOT confirms.
#include "status_display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include "board_hw.h"
#include "button.h"
#include "epd.h"
#include "sdlog.h"
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

// ---- The Waveshare ESP32-S3-ePaper-1.54's panel (board_hw.h, epd.h) -------------------------------------------
// u8g2 draws into a 200x200 buffer laid out as the panel's RAM (rows of 25 bytes, MSB left); epd.cpp sends it.
// Its display callback only reports the size: nothing is ever sent through u8g2 itself.
static bool epaper;          // this board has the panel (decided once, at display_begin())
static bool epaperOn;        // ... and the setting `display` is not off
static uint8_t *epdBuf, *epdShown;  // in PSRAM, only on this board (display_begin())
static bool epdShownValid;

static u8x8_display_info_t epdInfo;
static uint8_t epdCb(u8x8_t *u8x8, uint8_t msg, uint8_t, void *) {
  if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) u8x8_d_helper_display_setup_memory(u8x8, &epdInfo);
  return 1;
}
class U8G2_EPD200 : public U8G2 {
 public:
  // After epdBuf is allocated (the buffer is bound here).
  void setup() {
    epdInfo.chip_disable_level = 1;
    epdInfo.tile_width = EPD_W / 8;
    epdInfo.tile_height = EPD_H / 8;
    epdInfo.pixel_width = EPD_W;
    epdInfo.pixel_height = EPD_H;
    u8g2_SetupDisplay(&u8g2, epdCb, u8x8_cad_empty, u8x8_byte_empty, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u8g2, epdBuf, EPD_H / 8, u8g2_ll_hvline_horizontal_right_lsb, U8G2_R0);
  }
};
static U8G2_EPD200 g_epd;  // set up in display_begin(), on this board only

// The setting and what is on the bus: present or not. Only loop() (and setup()) calls it.
static void look() {
  board_opts o;
  cfg_opts(&o);
  lang = bo_oled_lang(o.lang, web_lang().c_str());
  if (epaper) {  // the panel is part of the board: nothing to look for on I2C (GPIO5/6/8/9 are the panel's here)
    bool was = epaperOn;
    epaperOn = o.display != BO_DISPLAY_OFF;
    if (was && !epaperOn) {  // blank it once; e-paper keeps the last picture without power
      memset(epdBuf, 0, EPD_BYTES);
      epd_show(epdBuf, true);
      board_log("display: e-paper off (setting)");
    }
    epdShownValid = false;  // redraw everything (the language may have changed), with a full refresh
    return;
  }
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

void display_begin() {
  if (board_hw() == BOARD_WS_EPD154) {
    epdBuf = (uint8_t *)heap_caps_malloc(EPD_BYTES, MALLOC_CAP_SPIRAM);
    epdShown = (uint8_t *)heap_caps_malloc(EPD_BYTES, MALLOC_CAP_SPIRAM);
    epaper = epdBuf && epdShown && epd_begin();
    if (epaper) g_epd.setup();
    if (epaper) board_log("display: e-paper 200x200 (SSD1681)");
    else board_log("display: e-paper does not answer (BUSY stuck)");
    if (!epaper) return;  // never the OLED search on this board: its pins are the panel's
  }
  look();
}
void display_reconfigure() { again = true; }
bool display_present() { return epaper ? epaperOn : present; }
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

// One line centred at baseline y in the current font.
static void epdCenter(int y, const char *t) {
  int w = g_epd.getStrWidth(t);
  g_epd.drawStr(w >= EPD_W ? 0 : (EPD_W - w) / 2, y, t);
}

// Seconds left, in steps of 5 (each step is a refresh of the panel).
static unsigned long left5(uint32_t ms) { return (ms + 4999) / 5000 * 5; }

static void epdFrame() {
  char a[48], b[48];
  char c1[CF_TEXT], c2[CF_TEXT];
  uint32_t left = 0;
  bool stopped = stop_active();
  g_epd.clearBuffer();
  g_epd.setDrawColor(1);
  // the head: the board's name on black, the firmware on the right
  g_epd.drawBox(0, 0, EPD_W, 26);
  g_epd.setDrawColor(0);
  g_epd.setFont(u8g2_font_helvB12_tr);
  oled_ascii(board_name().c_str(), a, sizeof a);
  g_epd.drawStr(4, 19, a);
  g_epd.setFont(u8g2_font_5x7_tr);
  g_epd.drawStr(EPD_W - 4 - g_epd.getStrWidth(FW_VERSION), 19, FW_VERSION);
  g_epd.setDrawColor(1);

  if (confirm_open(c1, c2, &left)) {  // a request waits for the BOOT button
    g_epd.drawFrame(2, 32, EPD_W - 4, 164);
    g_epd.drawFrame(3, 33, EPD_W - 6, 162);
    g_epd.setFont(u8g2_font_helvB10_tr);
    epdCenter(58, oled_text(lang, OT_CONFIRM));
    g_epd.setFont(u8g2_font_helvR10_tr);
    oled_ascii(oled_request(lang, c1), a, sizeof a);
    epdCenter(92, a);
    oled_ascii(oled_request(lang, c2), a, sizeof a);
    epdCenter(114, a);
    g_epd.setFont(u8g2_font_helvB18_tr);
    snprintf(a, sizeof a, oled_text(lang, OT_LEFT), left5(left));
    epdCenter(170, a);
    return;
  }
  if (pair_window_open(&left)) {  // BOOT was double-pressed
    g_epd.setFont(u8g2_font_helvB14_tr);
    epdCenter(62, oled_text(lang, OT_PAIR_OPEN));
    g_epd.setFont(u8g2_font_helvR10_tr);
    epdCenter(96, oled_text(lang, OT_PAIR_RUN));
    g_epd.setFont(u8g2_font_courB12_tr);
    epdCenter(124, "tentacle pair");
    g_epd.setFont(u8g2_font_helvB18_tr);
    snprintf(a, sizeof a, oled_text(lang, OT_LEFT), left5(left));
    epdCenter(176, a);
    return;
  }
  if (net_setup_active()) {  // no Wi-Fi stored: the setup hotspot's QR code, as large as fits below the head
    int n = wifi_qr_size();
    // a quiet zone of 2 modules on each side (the head above is black); the panel's 6x12 text was too small to read
    int s = n ? (EPD_H - 26) / (n + 4) : 0;
    if (s >= 2) {
      int q = n * s, x0 = (EPD_W - q) / 2, y0 = 26 + (EPD_H - 26 - q) / 2;
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
          if (wifi_qr_dark(x, y)) g_epd.drawBox(x0 + x * s, y0 + y * s, s, s);
    } else {  // no code: the hotspot's name and password as text
      String ssid = net_ap_ssid(), pass = net_ap_pass();
      g_epd.setFont(u8g2_font_helvB10_tr);
      epdCenter(60, oled_text(lang, OT_SETUP_JOIN));
      oled_ascii(ssid.c_str(), a, sizeof a);
      epdCenter(100, a);
      snprintf(b, sizeof b, oled_text(lang, OT_PASS), pass.c_str());
      epdCenter(124, b);
    }
    return;
  }
  // running or stopped, big; stopped on black
  g_epd.setFont(u8g2_font_helvB18_tr);
  const char *state = oled_text(lang, stopped ? OT_STOPPED : OT_RUNNING);
  if (stopped) {
    g_epd.drawBox(0, 34, EPD_W, 40);
    g_epd.setDrawColor(0);
    epdCenter(63, state);
    g_epd.setDrawColor(1);
  } else epdCenter(63, state);
  g_epd.setFont(u8g2_font_6x12_tr);
  snprintf(a, sizeof a, oled_text(lang, OT_NET), net_mode(), net_ip().c_str());
  g_epd.drawStr(6, 98, a);
  if (!net_up()) snprintf(a, sizeof a, "%s", oled_text(lang, OT_NO_WIFI));
  else snprintf(a, sizeof a, oled_text(lang, OT_PEERS), net_peers(), (int)cli_up());
  g_epd.drawStr(6, 116, a);
  int bat = batPct;
  if (bat >= 0) {
    snprintf(b, sizeof b, oled_text(lang, OT_BAT), "", bat);
    g_epd.drawStr(6, 134, b + 2);  // without the state (shown above) and its two spaces
  }
  // the foot: the session log on the card
  g_epd.drawHLine(0, 170, EPD_W);
  int sd = sdlog_state();
  if (sd == SDLOG_REC) {
    g_epd.drawDisc(14, 185, 7);
    g_epd.setFont(u8g2_font_helvB12_tr);
    g_epd.drawStr(26, 191, "REC");
    g_epd.setFont(u8g2_font_6x12_tr);
    const char *f = strrchr(sdlog_file(), '/');
    g_epd.drawStr(70, 190, f ? f + 1 : "");
  } else if (sd != SDLOG_NONE) {
    g_epd.setFont(u8g2_font_6x12_tr);
    g_epd.drawStr(6, 190, sd == SDLOG_NOCARD ? "SD: -" : sd == SDLOG_ERR ? "SD: error" : "SD log off");
  }
}

static void epdPoll() {
  static uint32_t last = 0;
  if (!epaperOn || millis() - last < 500) return;
  last = millis();
  epdFrame();
  if (epdShownValid && !memcmp(epdBuf, epdShown, EPD_BYTES)) return;  // unchanged: the panel keeps it
  epd_show(epdBuf, !epdShownValid);  // the first picture (and after a setting changed) with a full refresh
  memcpy(epdShown, epdBuf, EPD_BYTES);
  epdShownValid = true;
}

void display_poll() {  // ~2 Hz from loop()
  static uint32_t last = 0;
  sampleBattery();
  if (again) {
    again = false;
    look();
  }
  if (epaper) {
    epdPoll();
    return;
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
