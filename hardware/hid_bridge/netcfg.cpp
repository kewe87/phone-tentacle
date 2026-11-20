// Board settings in NVS and the Wi-Fi link, see netcfg.h.
#include "netcfg.h"
#include "board_hw.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <dhcpserver/dhcpserver.h>
#include <nvs.h>
#include "button.h"
#include "dbg.h"
#include "logbuf.h"
#include "setup_portal.h"
#include "wifi_qr.h"
#include "src/setup/setup_text.h"
#include "src/web/web_auth.h"
#include "version.h"

#ifndef AP_MAX_CLIENTS
#define AP_MAX_CLIENTS 1  // default: one station on the setup hotspot (flash --multi-client sets 4)
#endif

#if STA_FROM_SECRETS
#if __has_include("secrets.h")
#include "secrets.h"  // WIFI_SSID, WIFI_PASS (git-ignored)
#define BOOT_STA 1
#else
#warning "STA_FROM_SECRETS set but secrets.h is missing: booting as SoftAP"
#endif
#endif

static Preferences prefs;
static String apSsid, apPass, staSsid, staPass, mode;  // mode: "ap" (the setup hotspot) | "sta"
static String pending;
static uint32_t pendingAt;
// A `net` request waits this long before it is applied: its `ok` is still on its way out over the link that the
// change takes down (a session over Wi-Fi asking for another Wi-Fi).
const uint32_t APPLY_AFTER_MS = 400;
static SemaphoreHandle_t pendingLock = xSemaphoreCreateMutex();
// STA retries back off: a tight reconnect loop keeps the radio busy scanning.
static uint32_t retryAt = 0, retryGap = 5000;

String board_suffix() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  char s[5];
  snprintf(s, sizeof s, "%02X%02X", mac[4], mac[5]);
  return s;
}

// 12 of 55 characters: ~69 bits. Called with the radio on (net_begin()), so esp_random() is a true random source.
static String randomPass() {
  const char *abc = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no look-alikes
  String p;
  for (int i = 0; i < 12; i++) p += abc[esp_random() % strlen(abc)];
  return p;
}

// Value of a string field in a flat JSON object ("" if absent). Handles \" and \\ escapes.
static String jsonStr(const String &j, const char *key) {
  int k = j.indexOf(String("\"") + key + "\"");
  if (k < 0) return "";
  int c = j.indexOf(':', k);
  int q = c < 0 ? -1 : j.indexOf('"', c);
  if (q < 0) return "";
  String v;
  for (int i = q + 1; i < (int)j.length() && j[i] != '"'; i++) v += j[i] == '\\' && i + 1 < (int)j.length() ? j[++i] : j[i];
  return v;
}

static String jsonEsc(const String &s) {
  String o;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\';
    o += ch;
  }
  return o;
}

// ---- Setup mode ------------------------------------------------------------------------------------------------
// Without a stored Wi-Fi (mode "ap") the board runs its own WPA2 hotspot with the setup page (setup_portal.cpp).
// A network sent from the page is held in RAM only: the BOOT button has to confirm it (CONFIRM_MS), then the board
// tries it next to the hotspot, and stores it only once it has an address. Then the hotspot goes away for good.
// loop() runs this state machine and makes every Wi-Fi call; the page's task only reads and submits under suLock.
const uint32_t CONFIRM_MS = 30000;   // to press BOOT after the page sent a network
const uint32_t TRY_MS = 20000;       // to get an address on that network
const uint32_t TRY_AGAIN_MS = 6000;  // between attempts within TRY_MS
const uint32_t DONE_MS = 5000;       // the hotspot stays this long after success, so the page can say so
const int SCAN_MAX = 16;

enum { SU_OFF, SU_IDLE, SU_WAIT, SU_TRYING, SU_OK };
static SemaphoreHandle_t suLock = xSemaphoreCreateMutex();  // su, suLast, trySsid, tryPass, suIp, the scan list
static int su = SU_OFF;
static int suLast = NS_IDLE;  // how the last attempt ended: NS_IDLE, NS_EXPIRED, NS_FAILED or NS_NOT_FOUND
static String trySsid, tryPass, suIp;
static uint8_t tryWeb[WA_REC_LEN];  // the web interface's password (web_auth.h), if the page set one
static bool tryWebSet;
static uint32_t suAt, tryAt;
static bool tryNotFound;
static NetScan scanList[SCAN_MAX];
static int scanCount;
static volatile bool scanWanted;
static bool scanning;
static uint32_t scanAt;

static void suSet(int state, int last) {
  xSemaphoreTake(suLock, portMAX_DELAY);
  su = state;
  suLast = last;
  if (state == SU_IDLE || state == SU_OFF) {  // the candidate is done with
    trySsid = "";
    tryPass = "";
    tryWebSet = false;
  }
  xSemaphoreGive(suLock);
}

// The setup hotspot hands out itself as router and DNS server, so a phone or laptop that joins opens the setup page
// by itself (captive portal: the page's DNS server answers every name with our address).
static void captiveDhcp() {
  esp_netif_t *ap = WiFi.AP.netif();
  if (!ap) return;
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = (uint32_t)WiFi.softAPIP();
  dhcps_offer_t offer = OFFER_DNS;
  esp_netif_dhcps_stop(ap);
  esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  esp_netif_dhcps_start(ap);
}

static void leaveSetup() {
  if (su == SU_OFF) return;
  portal_stop();  // port 80 and the DNS server close before the hotspot goes
  confirm_clear(CONFIRM_WIFI);
  if (scanning) WiFi.scanDelete();
  scanning = false;
  scanWanted = false;
  suSet(SU_OFF, NS_IDLE);
}

static void startWifi() {
  leaveSetup();
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.setHostname("tentacle");
  if (mode == "sta") {
    WiFi.mode(WIFI_STA);  // modem sleep stays on (the core's default), off while the phone mirrors (hid_bridge.ino)
    WiFi.setAutoReconnect(false);
    // Full TX power on the home network: the setup hotspot's 5 dBm (below) stays set in the driver when the setup
    // goes on to the station without a restart.
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    retryGap = 5000;
    retryAt = millis() + retryGap;
    // the core rejects open networks by default
    WiFi.setMinSecurity(staPass.isEmpty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  } else {
    WiFi.mode(WIFI_AP_STA);  // the station side scans for the page's list and tries the chosen network
    WiFi.setAutoReconnect(false);
    bool apUp = WiFi.softAP(apSsid.c_str(), apPass.c_str(), 6, 0, AP_MAX_CLIENTS);
    if (!apUp) board_log("wifi: the setup hotspot did not start");
    wifi_qr_make(apSsid, apPass);  // for the OLED and the wifi-setup line
    // The phone is centimetres away and powers the board: low TX power cuts the current peaks that made the
    // phone drop its USB power (the board then came back with reset reason "power-on"). Not on the Waveshare
    // e-paper board: its chip antenna at 5 dBm did not reach a phone at all (2026-11-20).
    WiFi.setTxPower(board_hw() == BOARD_WS_EPD154 ? WIFI_POWER_15dBm : WIFI_POWER_5dBm);
    captiveDhcp();
    suSet(SU_IDLE, NS_IDLE);
    scanWanted = true;
    portal_start();
  }
  Serial.println(net_setup_line());
  board_log("wifi: %s", mode == "sta" ? "station" : "setup hotspot");
}

// BOOT held 10 s, or `net {"mode":"ap"}`: forget the Wi-Fi and open the setup hotspot. The mode goes first, so a
// power cut in between leaves "no Wi-Fi stored" (net_begin() then removes what is left of it).
static void forgetWifi() {
  mode = "ap";
  prefs.putString("mode", mode);
  prefs.remove("sta_ssid");
  prefs.remove("sta_pass");
  staSsid = "";
  staPass = "";
  startWifi();
}

static void pollScan() {
  if (scanning) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING && millis() - scanAt < 15000) return;
    if (n >= 0) {
      xSemaphoreTake(suLock, portMAX_DELAY);
      scanCount = 0;
      for (int i = 0; i < n && scanCount < SCAN_MAX; i++) {  // strongest first, as the driver sorts them
        String s = WiFi.SSID(i);
        if (s.isEmpty() || s.length() > 32) continue;  // hidden networks are typed in by hand
        bool known = false;
        for (int k = 0; k < scanCount; k++) known |= s == scanList[k].ssid;  // a mesh shows up once
        if (known) continue;
        NetScan &e = scanList[scanCount++];
        strlcpy(e.ssid, s.c_str(), sizeof e.ssid);
        e.rssi = (int8_t)constrain(WiFi.RSSI(i), -127, 0);
        e.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      }
      xSemaphoreGive(suLock);
      // what the board hears: an antenna problem shows as few networks, all weak
      board_log("wifi: scan: %d networks, strongest %d dBm", n, n > 0 ? (int)WiFi.RSSI(0) : 0);
    }
    WiFi.scanDelete();
    scanning = false;
  } else if (scanWanted && su == SU_IDLE) {
    scanWanted = false;
    // async: loop() keeps running. 120 ms a channel: the hotspot is off its channel for ~1.5 s in all.
    if (WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_RUNNING) {
      scanning = true;
      scanAt = millis();
    }
  }
}

static void tryBegin() {
  WiFi.disconnect();
  WiFi.setMinSecurity(tryPass.isEmpty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK);
  WiFi.begin(trySsid.c_str(), tryPass.c_str());
  tryAt = millis();
}

// From SU_TRYING on only loop() touches trySsid / tryPass (the page may submit in SU_IDLE and SU_WAIT only).
static void pollSetup() {
  if (su == SU_OFF) return;
  pollScan();
  uint32_t now = millis();
  if (su == SU_WAIT) {
    // Under the lock: the page must not swap in another network between the press and the attempt.
    xSemaphoreTake(suLock, portMAX_DELAY);
    int c = confirm_state(CONFIRM_WIFI);
    bool wait = c == CF_WAITING || (c == CF_ACCEPTED && scanning);  // a running scan ends first (~1.5 s)
    if (!wait) {
      confirm_clear(CONFIRM_WIFI);
      su = c == CF_ACCEPTED ? SU_TRYING : SU_IDLE;
      suLast = c == CF_ACCEPTED ? NS_IDLE : NS_EXPIRED;
      if (su == SU_IDLE) {  // not pressed in time: nothing was stored
        trySsid = "";
        tryPass = "";
        tryWebSet = false;
      }
    }
    xSemaphoreGive(suLock);
    if (su != SU_TRYING) return;
    suAt = now;
    tryNotFound = false;
    board_log("wifi: setup confirmed, trying the network");
    tryBegin();
  } else if (su == SU_TRYING) {
    if (WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
      staSsid = trySsid;
      staPass = tryPass;
      prefs.putString("sta_ssid", staSsid);
      prefs.putString("sta_pass", staPass);
      if (tryWebSet) prefs.putBytes("web_pw", tryWeb, WA_REC_LEN);  // left empty on the page: the old one stays
      prefs.putString("mode", "sta");  // last: without it the board still counts as not set up
      xSemaphoreTake(suLock, portMAX_DELAY);
      suIp = WiFi.localIP().toString();
      tryPass = "";
      xSemaphoreGive(suLock);
      suSet(SU_OK, NS_IDLE);
      suAt = now;
      board_log("wifi: setup done, network stored");
    } else if (now - suAt >= TRY_MS) {
      tryNotFound |= WiFi.status() == WL_NO_SSID_AVAIL;
      WiFi.disconnect();
      suSet(SU_IDLE, tryNotFound ? NS_NOT_FOUND : NS_FAILED);  // nothing was stored; the hotspot stays
      board_log("wifi: setup failed, network %s", tryNotFound ? "not found" : "did not let us in (password?)");
    } else if (now - tryAt >= TRY_AGAIN_MS) {
      tryNotFound = WiFi.status() == WL_NO_SSID_AVAIL;
      tryBegin();
    }
  } else if (su == SU_OK && now - suAt >= DONE_MS) {
    mode = "sta";
    startWifi();  // the hotspot and the page go; from here on the board is only a client
  }
}

#if TENTACLE_DEBUG
static void logWifi(arduino_event_id_t e, arduino_event_info_t info) {
  switch (e) {
    case ARDUINO_EVENT_WIFI_STA_START: DBG("wifi: station started"); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: DBG("wifi: associated, channel %d", info.wifi_sta_connected.channel); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: DBG("wifi: disconnected, reason %d", info.wifi_sta_disconnected.reason); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: DBG("wifi: got ip %s", WiFi.localIP().toString().c_str()); break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP: DBG("wifi: lost ip"); break;
    case ARDUINO_EVENT_WIFI_AP_START: DBG("wifi: hotspot up"); break;
    case ARDUINO_EVENT_WIFI_AP_STOP: DBG("wifi: hotspot down"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: DBG("wifi: a station joined the hotspot"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: DBG("wifi: a station left the hotspot"); break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE: DBG("wifi: scan done"); break;
    default: break;
  }
}
#endif

static void cfg_migrate();
static void loadOpts();

void net_begin() {
#if TENTACLE_DEBUG
  WiFi.onEvent(logWifi);
#endif
  WiFi.persistent(false);  // the driver keeps no copy of a network in flash: ours in NVS is the only one
  prefs.begin("agent", false);
  cfg_migrate();
  loadOpts();
  apSsid = prefs.getString("ap_ssid", "");
  apPass = prefs.getString("ap_pass", "");
  if (apSsid.isEmpty() || apPass.length() < 8) {
    WiFi.mode(WIFI_STA);  // radio on for randomPass()
    delay(50);
    apSsid = "Tentacle-" + board_suffix();
    apPass = randomPass();
    prefs.putString("ap_ssid", apSsid);
    prefs.putString("ap_pass", apPass);
  }
  mode = prefs.getString("mode", "ap");
  if (mode != "sta") {  // not set up; a reset cut short by a power loss may have left these
    mode = "ap";
    if (prefs.isKey("sta_ssid")) prefs.remove("sta_ssid");
    if (prefs.isKey("sta_pass")) prefs.remove("sta_pass");
  }
  staSsid = prefs.getString("sta_ssid", "");
  staPass = prefs.getString("sta_pass", "");
#ifdef BOOT_STA
  mode = "sta";  // build-time test mode, not stored
  staSsid = WIFI_SSID;
  staPass = WIFI_PASS;
#endif
  if (mode == "sta" && staSsid.isEmpty()) mode = "ap";
  startWifi();
}

void net_request(const String &json) {
  xSemaphoreTake(pendingLock, portMAX_DELAY);
  pending = json;
  pendingAt = millis();
  xSemaphoreGive(pendingLock);
}

// A stored network that is out of reach or has a new password is retried forever; the setup hotspot never comes
// back by itself (hold BOOT 10 s, or `net {"mode":"ap"}` on USB serial).
static void retrySta() {
  if (mode != "sta") return;
  if (WiFi.status() == WL_CONNECTED) {
    retryGap = 5000;
    return;
  }
  if ((int32_t)(millis() - retryAt) < 0) return;
  WiFi.disconnect();
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  retryGap = min<uint32_t>(retryGap * 2, 60000);
  retryAt = millis() + retryGap;
}

void net_poll() {
  if (button_take_long()) {
    board_log("wifi: BOOT held 10 s, forgetting the network");
    forgetWifi();
  }
  retrySta();
  pollSetup();
  String j;
  xSemaphoreTake(pendingLock, portMAX_DELAY);
  if (!pending.isEmpty() && millis() - pendingAt >= APPLY_AFTER_MS) {
    j = pending;
    pending = "";
  }
  xSemaphoreGive(pendingLock);
  if (j.isEmpty()) return;
  DBG("wifi: applying a net request");
  // From a paired computer (or USB serial): no button press, the sender is trusted already.
  String m = jsonStr(j, "mode");
  if (m == "ap") {
    forgetWifi();
  } else if (m == "sta" && setup_check(jsonStr(j, "ssid").c_str(), jsonStr(j, "pass").c_str()) == SETUP_OK) {
    mode = "sta";
    staSsid = jsonStr(j, "ssid");
    staPass = jsonStr(j, "pass");
    prefs.putString("sta_ssid", staSsid);
    prefs.putString("sta_pass", staPass);
    prefs.putString("mode", mode);
    startWifi();
  }  // else malformed: keep the current link
}

String net_json() {
  bool sta = mode == "sta";
  String pass = sta ? "" : apPass;  // the hotspot's is needed to join it; the home Wi-Fi's never leaves the board
  return String("{\"mode\":\"") + mode + "\",\"ssid\":\"" + jsonEsc(sta ? staSsid : apSsid) + "\",\"pass\":\"" +
         jsonEsc(pass) + "\",\"ip\":\"" + net_ip() + "\"}";
}

const char *net_mode() {
  if (mode == "sta") return WiFi.status() == WL_CONNECTED ? "sta" : "none";
  return "ap";
}

String net_ip() { return mode == "sta" ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
int net_peers() { return mode == "sta" ? WiFi.status() == WL_CONNECTED : WiFi.softAPgetStationNum(); }
bool net_up() { return mode == "sta" ? WiFi.status() == WL_CONNECTED : true; }

// ---- Setup mode, for the page (its own task), the display and USB serial ----------------------------------------
bool net_setup_active() { return su != SU_OFF; }
String net_ap_ssid() { return apSsid; }
String net_ap_pass() { return apPass; }

String net_setup_line() {
  if (mode == "ap")
    return "wifi-setup {\"setup\":1,\"ssid\":\"" + jsonEsc(apSsid) + "\",\"pass\":\"" + jsonEsc(apPass) + "\",\"url\":\"http://" +
           WiFi.softAPIP().toString() + "/\"" + (wifi_qr_size() ? ",\"qr\":\"" + wifi_qr_hex() + "\"" : String()) + "}";
  return "wifi-setup {\"setup\":0,\"ssid\":\"" + jsonEsc(mode == "sta" ? staSsid : String()) + "\",\"ip\":\"" + net_ip() + "\"}";
}

void net_setup_rescan() { scanWanted = true; }

int net_setup_scan(NetScan *out, int cap, bool *running) {
  xSemaphoreTake(suLock, portMAX_DELAY);
  int n = min(scanCount, cap);
  memcpy(out, scanList, n * sizeof *out);
  xSemaphoreGive(suLock);
  if (running) *running = scanning || scanWanted;
  return n;
}

bool net_setup_submit(const char *ssid, const char *pass, const uint8_t *webPw) {
  if (setup_check(ssid, pass) != SETUP_OK) return false;
  char shown[CF_TEXT];  // for the display, whose font is ASCII
  size_t n = 0;
  for (; ssid[n] && n < sizeof shown - 1; n++) shown[n] = ssid[n] >= 0x20 && ssid[n] < 0x7f ? ssid[n] : '?';
  shown[n] = 0;
  bool ok = false;
  xSemaphoreTake(suLock, portMAX_DELAY);
  if ((su == SU_IDLE || su == SU_WAIT) && confirm_request(CONFIRM_WIFI, "Join this Wi-Fi?", shown, CONFIRM_MS)) {
    trySsid = ssid;
    tryPass = pass;
    tryWebSet = webPw != nullptr;
    if (webPw) memcpy(tryWeb, webPw, WA_REC_LEN);
    su = SU_WAIT;
    ok = true;
  }
  xSemaphoreGive(suLock);
  return ok;
}

void net_setup_status(NetSetup *s) {
  memset(s, 0, sizeof *s);
  uint32_t left = 0;
  char l1[CF_TEXT], l2[CF_TEXT];
  bool waiting = confirm_open(l1, l2, &left);
  xSemaphoreTake(suLock, portMAX_DELAY);
  switch (su) {
    case SU_OFF: s->state = NS_OFF; break;
    case SU_IDLE: s->state = suLast; break;
    case SU_WAIT: s->state = NS_WAIT; break;
    case SU_TRYING: s->state = NS_TRYING; break;
    default: s->state = NS_OK;
  }
  if (su == SU_WAIT && waiting) s->left_ms = left;
  strlcpy(s->ssid, trySsid.c_str(), sizeof s->ssid);
  if (su == SU_OK) strlcpy(s->ip, suIp.c_str(), sizeof s->ip);
  xSemaphoreGive(suLock);
}

uint16_t cfg_width() { return prefs.getUShort("w", 480); }
uint16_t cfg_height() { return prefs.getUShort("h", 1040); }
// 15 fps since 0.3.2 (30 in 0.2.2, 5 before): iOS sends a frame when the screen changes, at most this often; 5 made
// every change wait, 30 let a blinking cursor grow the GOP ~315 KB a minute, too much to replay over a weak link. Was:
// 30 fps since 0.2.2 (5 before): iOS sends a frame when the screen changes, at most this often, and 5 made every
// change wait. A stored 5 from before 0.2.2 goes to 30 once (cfg_migrate()); `fps N` sets it after that.
uint8_t cfg_fps() { return prefs.getUChar("fps", 15); }

// cfgv 1 (0.2.2): a stored fps of 5 goes. The hardware settings of 0.4.0 (cfg_opts()) need no step here: a board
// without their keys gets the defaults, which are what the release build does (OLED when one answers, absolute
// pointer, no battery gauge).
static void cfg_migrate() {
  if (prefs.getUChar("cfgv", 0) >= 1) return;
  if (prefs.isKey("fps") && prefs.getUChar("fps", 30) == 5) prefs.remove("fps");
  prefs.putUChar("cfgv", 1);
}

// ---- The hardware settings (netcfg.h) ----------------------------------------------------------------------------
static board_opts opts;  // read by net_begin(), under optsMux after that
static portMUX_TYPE optsMux = portMUX_INITIALIZER_UNLOCKED;

static void loadOpts() {
  board_opts o;
  bo_defaults(&o);
  uint8_t d = prefs.getUChar("disp", BO_DISPLAY_AUTO);
  if (d <= BO_DISPLAY_OFF) o.display = d;
  o.abs = prefs.getUChar("abs", 1) ? 1 : 0;
  int8_t pin = prefs.getChar("bat", BO_BAT_NONE);
  if (bo_bat_pin_ok(pin)) o.bat_pin = pin;
  uint16_t div = prefs.getUShort("bat_div", BO_DIV_DEFAULT);
  if (div >= BO_DIV_MIN && div <= BO_DIV_MAX) o.bat_div = div;
  uint8_t l = prefs.getUChar("oled_lang", BO_LANG_AUTO);
  if (l <= BO_LANG_DE) o.lang = l;
  o.sdlog = prefs.getUChar("sdlog", 0);
  if (o.sdlog > BO_SD_VIDEO) o.sdlog = BO_SD_OFF;
  portENTER_CRITICAL(&optsMux);
  opts = o;
  portEXIT_CRITICAL(&optsMux);
}

void cfg_opts(board_opts *o) {
  portENTER_CRITICAL(&optsMux);
  *o = opts;
  portEXIT_CRITICAL(&optsMux);
}

int cfg_set_opt(const char *key, const char *value) {
  board_opts o;
  cfg_opts(&o);
  int r = bo_set(&o, key, value);
  if (r != BO_OK) return r;
  if (!strcmp(key, "display")) prefs.putUChar("disp", o.display);
  else if (!strcmp(key, "abs")) prefs.putUChar("abs", o.abs);
  else if (!strcmp(key, "battery")) prefs.putChar("bat", o.bat_pin);
  else if (!strcmp(key, "battery_div")) prefs.putUShort("bat_div", o.bat_div);
  else if (!strcmp(key, "sdlog")) prefs.putUChar("sdlog", o.sdlog);
  else prefs.putUChar("oled_lang", o.lang);
  portENTER_CRITICAL(&optsMux);
  opts = o;
  portEXIT_CRITICAL(&optsMux);
  return BO_OK;
}

// Straight from NVS, not through prefs: USB starts before setup() with the report descriptor's length fixed
// (hid_pointer.h), and the phone may ask for the descriptor itself before setup() runs. The USB task then asks
// here, possibly before the core has initialised NVS: it waits for that, 0.5 s at most.
static volatile int8_t absActive = -1;

bool cfg_abs_active() {
  if (absActive < 0) {
    uint8_t v = 1;
    for (int i = 0; i < 50; i++) {
      nvs_handle_t h;
      esp_err_t e = nvs_open("agent", NVS_READONLY, &h);
      if (e == ESP_OK) {
        nvs_get_u8(h, "abs", &v);  // not there: the default stays
        nvs_close(h);
        break;
      }
      if (e != ESP_ERR_NVS_NOT_INITIALIZED) break;  // no namespace yet (a new board): the default
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    absActive = v ? 1 : 0;
  }
  return absActive == 1;
}

void cfg_set_res(uint16_t w, uint16_t h) {
  prefs.putUShort("w", w);
  prefs.putUShort("h", h);
}

void cfg_set_fps(uint8_t fps) { prefs.putUChar("fps", fps); }

void cfg_ed25519_seed(uint8_t seed[32]) {
  if (prefs.getBytes("ed_seed", seed, 32) == 32) return;
  esp_fill_random(seed, 32);
  prefs.putBytes("ed_seed", seed, 32);
}

// ---- The board's name and the web interface's settings (NVS "agent" too) ----------------------------------------
static bool nameOk(const String &n) {
  if (n.length() < 1 || n.length() > 20 || n[0] == ' ' || n[n.length() - 1] == ' ') return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != ' ' && c != '-' && c != '_' && c != '.') return false;
  return true;
}

String board_name() {  // read once: a new name takes effect on the next boot (and the display asks often)
  static String name;
  if (!name.length()) {
    String n = prefs.getString("name", "");
    name = nameOk(n) ? n : String(BOARD_NAME_TEXT);
  }
  return name;
}

String board_name_saved() {
  String n = prefs.getString("name", "");
  return nameOk(n) ? n : String(BOARD_NAME_TEXT);
}

bool board_set_name(const String &name) {
  if (!nameOk(name)) return false;
  if (name == BOARD_NAME_TEXT) prefs.remove("name");
  else prefs.putString("name", name);
  return true;
}

bool web_pw_get(uint8_t rec[WA_REC_LEN]) { return prefs.getBytes("web_pw", rec, WA_REC_LEN) == WA_REC_LEN; }
void web_pw_set(const uint8_t rec[WA_REC_LEN]) { prefs.putBytes("web_pw", rec, WA_REC_LEN); }
String web_lang() { return prefs.getString("web_lang", ""); }
void web_set_lang(const String &lang) { prefs.putString("web_lang", lang); }
