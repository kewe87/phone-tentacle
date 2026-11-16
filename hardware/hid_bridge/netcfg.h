// Board settings in NVS and the Wi-Fi link. Without a stored Wi-Fi the board is in setup mode: its own WPA2 hotspot
// (random credentials made once) with the setup page (setup_portal.h). Once a Wi-Fi is stored (from the page plus a
// press of BOOT, or `net` from a paired computer) it is only a client there; holding BOOT 10 s forgets it.
// A build with -DSTA_FROM_SECRETS=1 boots in station mode with WIFI_SSID / WIFI_PASS from the git-ignored secrets.h.
// NVS ("agent"): mode ("ap" = setup | "sta"), sta_ssid, sta_pass, ap_ssid, ap_pass.
#pragma once
#include <Arduino.h>

void net_begin();
void net_poll();                         // call from loop(): setup mode, STA retries, a pending `net` write
String net_json();                       // `net`'s answer: {"mode","ssid","pass" (the setup hotspot's only),"ip"}
void net_request(const String &json);    // `net {...}`; applied by net_poll()
const char *net_mode();                  // "ap" (setup) | "sta" | "none" (station not connected)
String net_ip();
int net_peers();                         // stations on our SoftAP, or 1 when connected as a station
bool net_up();                           // has an IP address

// ---- Setup mode. net_setup_scan/_rescan/_submit/_status are safe from the page's task; the rest is loop()'s.
bool net_setup_active();
String net_ap_ssid();                    // the setup hotspot's name and password (display)
String net_ap_pass();
// USB serial, at every Wi-Fi start and on `0 setup`; flash.sh / flash.ps1 read it after flashing:
//   wifi-setup {"setup":1,"ssid":"Tentacle-1A2B","pass":"...","url":"http://192.168.4.1/","qr":"fe0b...,..."}
//   (qr: the hotspot as a Wi-Fi QR code, wifi_qr.h: one hex string per row, first module in the top bit)
//   wifi-setup {"setup":0,"ssid":"<the stored Wi-Fi>","ip":"<address, 0.0.0.0 while not connected>"}
String net_setup_line();

struct NetScan {
  char ssid[33];
  int8_t rssi;
  bool open;
};
int net_setup_scan(NetScan *out, int cap, bool *running);  // the last scan, strongest first
void net_setup_rescan();
// A network from the page: held in RAM until BOOT is pressed (30 s), then tried, stored only if it works.
// webPw: the web interface's password (a record of src/web/web_auth.h, 53 bytes), stored with the network; NULL
// keeps the one stored before. false: bad lengths, an attempt is running, or another confirmation is open.
bool net_setup_submit(const char *ssid, const char *pass, const uint8_t *webPw = nullptr);

enum { NS_OFF, NS_IDLE, NS_WAIT, NS_TRYING, NS_OK, NS_EXPIRED, NS_FAILED, NS_NOT_FOUND };
struct NetSetup {
  int state;         // NS_EXPIRED / NS_FAILED / NS_NOT_FOUND: how the last attempt ended (nothing stored)
  uint32_t left_ms;  // NS_WAIT: time left to press BOOT
  char ssid[33];     // the network being confirmed or tried
  char ip[16];       // NS_OK: our address there
};
void net_setup_status(NetSetup *s);

// AirPlay settings (take effect on the next boot of the receiver)
uint16_t cfg_width();
uint16_t cfg_height();
uint8_t cfg_fps();
void cfg_set_res(uint16_t w, uint16_t h);
void cfg_set_fps(uint8_t fps);
void cfg_ed25519_seed(uint8_t seed[32]);  // created once, stable across boots
String board_suffix();                    // "1A2B": the board id's last two bytes (cli_link.h, cli_board_id)

// The board's name (AirPlay receiver, CLI discovery, display): BOARD_NAME_TEXT unless set. 1-20 characters of
// A-Z a-z 0-9 space . _ -, no space at either end. Takes effect on the next boot.
String board_name();
bool board_set_name(const String &name);
// The web interface (web_server.cpp): its password as a record of src/web/web_auth.h (53 bytes; false: none set),
// and the page's language ("de", "en"; "": from the browser).
bool web_pw_get(uint8_t rec[53]);
void web_pw_set(const uint8_t rec[53]);
String web_lang();
void web_set_lang(const String &lang);
