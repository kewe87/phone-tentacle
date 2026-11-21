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
#include "epd_logo.h"
#include "src/airplay/airplay.h"
#include <WiFi.h>
#include <initializer_list>
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

// One line centred at baseline y in the current font (UTF-8).
static void epdCenter(int y, const char *t) {
  int w = g_epd.getUTF8Width(t);
  g_epd.drawUTF8(w >= EPD_W ? 0 : (EPD_W - w) / 2, y, t);
}

// Centred in the largest of the fonts (largest first) it fits in, with 4 px to spare on each side.
static void epdFit(int y, const char *t, std::initializer_list<const uint8_t *> fonts) {
  for (const uint8_t *f : fonts) {
    g_epd.setFont(f);
    if (g_epd.getUTF8Width(t) <= EPD_W - 8) break;
  }
  epdCenter(y, t);
}

// The panel's own words, in the board's language (the setting lang; auto: the web interface's).
enum { EW_RUNNING, EW_STOPPED, EW_NO_WIFI, EW_SD_OFF, EW_SD_NOCARD, EW_SD_ERR, EW_N };
static const char *const EPD_WORDS[2][EW_N] = {
    {"running", "STOPPED", "no Wi-Fi: hold BOOT 10 s", "SD off", "no card", "card error"},
    {"läuft", "GESTOPPT", "kein WLAN: BOOT 10 s halten", "SD aus", "keine Karte", "Kartenfehler"},
};
static const char *epdWord(int w) { return EPD_WORDS[lang == BO_LANG_DE ? 1 : 0][w]; }

// A thick arc around (cx, cy) from 45 to 135 degrees (upwards): the Wi-Fi symbol's waves.
static void epdArc(int cx, int cy, int r, int th) {
  for (int rr = r; rr < r + th; rr++)
    for (int i = 0; i <= 90; i++) {
      float a = (45 + i) * (float)M_PI / 180;
      g_epd.drawPixel(cx + (int)lroundf(rr * cosf(a)), cy - (int)lroundf(rr * sinf(a)));
    }
}

// Wi-Fi: a dot and three waves, the reached ones thick, the others thin; no Wi-Fi: thin and crossed out.
static void epdWifi(int cx, int cy, int bars, bool up) {
  g_epd.drawDisc(cx, cy, 3);
  for (int i = 0; i < 3; i++) {
    int r = 9 + i * 8;
    if (up && i < bars) epdArc(cx, cy, r, 4);
    else epdArc(cx, cy, r + 1, 1);
  }
  if (!up) {
    for (int d = -1; d <= 1; d++) {
      g_epd.drawLine(cx - 14 + d, cy - 30, cx + 14 + d, cy - 2);
      g_epd.drawLine(cx + 14 + d, cy - 30, cx - 14 + d, cy - 2);
    }
  }
}

// The phone: its screen filled while it mirrors to the board, empty otherwise.
static void epdPhone(int x, int y, bool on) {
  g_epd.drawRFrame(x, y, 24, 42, 5);
  g_epd.drawRFrame(x + 1, y + 1, 22, 40, 4);
  g_epd.drawBox(x + 9, y + 3, 6, 2);  // the speaker
  if (on) g_epd.drawBox(x + 4, y + 7, 16, 28);
  else g_epd.drawFrame(x + 4, y + 7, 16, 28);
  g_epd.drawDisc(x + 12, y + 38, 1);
}

// The computer: its screen filled while the CLI is connected.
static void epdLaptop(int x, int y, bool on) {
  g_epd.drawRFrame(x, y, 36, 26, 3);
  g_epd.drawRFrame(x + 1, y + 1, 34, 24, 2);
  if (on) g_epd.drawBox(x + 4, y + 4, 28, 18);
  g_epd.drawRBox(x - 5, y + 27, 46, 5, 2);
  g_epd.setDrawColor(0);
  g_epd.drawHLine(x + 13, y + 28, 10);  // the touchpad's notch
  g_epd.setDrawColor(1);
}

// A microSD card: cut corner, contacts.
static void epdCard(int x, int y) {
  const int w = 18, h = 24;
  for (int t = 0; t < 2; t++) {
    g_epd.drawLine(x + t, y + 6, x + 6, y + t);
    g_epd.drawLine(x + 6, y + t, x + w - t, y + t);
    g_epd.drawLine(x + w - t, y, x + w - t, y + h);
    g_epd.drawLine(x + t, y + h - t, x + w, y + h - t);
    g_epd.drawLine(x + t, y + 6, x + t, y + h);
  }
  for (int i = 0; i < 3; i++) g_epd.drawBox(x + 6 + i * 4, y + 4, 2, 5);
}

// A battery, white on the black head: outline, nub, filled by the charge.
static void epdBattery(int x, int y, int pct) {
  g_epd.drawFrame(x, y, 24, 12);
  g_epd.drawBox(x + 24, y + 3, 2, 6);
  int f = pct * 20 / 100;
  if (f > 0) g_epd.drawBox(x + 2, y + 2, f, 8);
}

// Signal bars from the RSSI, held for 10 s so the panel does not refresh for every dB.
static int epdBars() {
  static int bars = -1;
  static uint32_t at;
  if (bars >= 0 && millis() - at < 10000) return bars;
  int rssi = WiFi.RSSI();
  bars = rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -80 ? 1 : 0;
  at = millis();
  return bars;
}

// Seconds left, in steps of 5 (each step is a refresh of the panel).
static const uint32_t SPLASH_MS = 4000;  // the start picture, until Wi-Fi is mostly up

static unsigned long left5(uint32_t ms) { return (ms + 4999) / 5000 * 5; }

static void epdFrame() {
  char a[48], b[48];
  char c1[CF_TEXT], c2[CF_TEXT];
  uint32_t left = 0;
  bool stopped = stop_active();
  g_epd.clearBuffer();
  g_epd.setDrawColor(1);
  if (millis() < SPLASH_MS) {  // just switched on: the mark, big, and the name of the thing
    g_epd.drawXBM((EPD_W - LOGO_BIG_W) / 2, 22, LOGO_BIG_W, LOGO_BIG_H, LOGO_BIG);
    g_epd.setFont(u8g2_font_helvB24_tf);
    epdCenter(168, "Tentacle");
    g_epd.setFont(u8g2_font_helvB10_tf);
    epdCenter(190, FW_VERSION);
    return;
  }
  // the head: the board's name on black, the battery (if there is one) on the right
  g_epd.drawBox(0, 0, EPD_W, 26);
  g_epd.setDrawColor(0);
  g_epd.drawXBM(3, 2, LOGO_SMALL_W, LOGO_SMALL_H, LOGO_SMALL);
  g_epd.setFont(u8g2_font_helvB14_tf);
  oled_ascii(board_name().c_str(), a, sizeof a);
  g_epd.drawStr(30, 20, a);
  int bat = batPct;
  if (bat >= 0) epdBattery(EPD_W - 32, 7, bat);
  g_epd.setDrawColor(1);

  if (confirm_open(c1, c2, &left)) {  // a request waits for the BOOT button
    g_epd.drawFrame(2, 32, EPD_W - 4, 164);
    g_epd.drawFrame(3, 33, EPD_W - 6, 162);
    epdFit(62, oled_text(lang, OT_CONFIRM), {u8g2_font_helvB14_tf, u8g2_font_helvB12_tf, u8g2_font_helvB10_tf});
    oled_ascii(oled_request(lang, c1), a, sizeof a);
    epdFit(98, a, {u8g2_font_helvB14_tf, u8g2_font_helvB12_tf, u8g2_font_helvB10_tf});
    oled_ascii(oled_request(lang, c2), a, sizeof a);
    epdFit(124, a, {u8g2_font_helvB14_tf, u8g2_font_helvB12_tf, u8g2_font_helvB10_tf});
    g_epd.setFont(u8g2_font_helvB18_tr);
    snprintf(a, sizeof a, oled_text(lang, OT_LEFT), left5(left));
    epdCenter(170, a);
    return;
  }
  if (pair_window_open(&left)) {  // BOOT was double-pressed
    epdFit(62, oled_text(lang, OT_PAIR_OPEN), {u8g2_font_helvB18_tf, u8g2_font_helvB14_tf, u8g2_font_helvB12_tf});
    epdFit(98, oled_text(lang, OT_PAIR_RUN), {u8g2_font_helvB14_tf, u8g2_font_helvB12_tf, u8g2_font_helvB10_tf});
    g_epd.setFont(u8g2_font_courB18_tf);
    epdCenter(132, "tentacle pair");
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
  // running (a play triangle) or stopped (a square, on black): the same size as before
  const char *state = epdWord(stopped ? EW_STOPPED : EW_RUNNING);
  g_epd.setFont(u8g2_font_helvB18_tf);
  int tw = g_epd.getUTF8Width(state), x0 = (EPD_W - (18 + 10 + tw)) / 2;
  if (stopped) {
    g_epd.drawBox(0, 30, EPD_W, 34);
    g_epd.setDrawColor(0);
    g_epd.drawBox(x0, 38, 18, 18);
  } else g_epd.drawTriangle(x0, 37, x0, 57, x0 + 18, 47);
  g_epd.drawUTF8(x0 + 28, 56, state);
  g_epd.setDrawColor(1);

  // Wi-Fi, the phone mirroring, the computer (CLI) connected
  bool up = net_up();
  epdWifi(36, 116, up ? epdBars() : 0, up);
  epdPhone(88, 76, airplay_state() == AIRPLAY_STREAMING);
  epdLaptop(146, 84, cli_up());

  // the address to open in a browser, big; without Wi-Fi how to set it up
  if (up) {
    String ip = net_ip();
    epdFit(150, ip.c_str(), {u8g2_font_helvB18_tf, u8g2_font_helvB14_tf});
  } else epdFit(150, epdWord(EW_NO_WIFI), {u8g2_font_helvB12_tf, u8g2_font_helvB10_tf});

  // the foot: the recording on the card
  int sd = sdlog_state();
  if (sd == SDLOG_NONE) return;
  g_epd.drawHLine(0, 164, EPD_W);
  g_epd.drawHLine(0, 165, EPD_W);
  if (sd == SDLOG_REC) {
    g_epd.drawDisc(16, 183, 9);
    g_epd.setFont(u8g2_font_helvB14_tf);
    g_epd.drawStr(32, 190, "REC");
    const char *f = strrchr(sdlog_file(), '/');
    g_epd.setFont(u8g2_font_helvB12_tf);
    if (f) g_epd.drawStr(EPD_W - 6 - g_epd.getStrWidth(f + 1), 189, f + 1);
  } else {
    epdCard(8, 171);
    if (sd != SDLOG_OFF) {  // no card, an error: crossed out
      g_epd.drawLine(4, 197, 30, 169);
      g_epd.drawLine(5, 197, 31, 169);
    }
    g_epd.setFont(u8g2_font_helvB14_tf);
    g_epd.drawUTF8(38, 190, epdWord(sd == SDLOG_NOCARD ? EW_SD_NOCARD : sd == SDLOG_ERR ? EW_SD_ERR : EW_SD_OFF));
  }
}

static void epdPoll() {
  static uint32_t last = 0;
  if (!epaperOn || millis() - last < 500) return;
  last = millis();
  static bool splashOver;
  if (!splashOver && millis() >= SPLASH_MS) {  // from the start picture to the status: a full refresh, no ghost
    splashOver = true;
    epdShownValid = false;
  }
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
