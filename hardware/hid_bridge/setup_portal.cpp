// The Wi-Fi setup page, see setup_portal.h.
#include "setup_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "netcfg.h"
#include "version.h"
#include "src/setup/setup_text.h"

struct Text {
  const char *lang, *title, *intro, *net, *choose, *open, *rescan, *scanning, *manual, *pass, *go, *hint;
  const char *badSsid, *badPass, *busy;
  const char *wait, *trying, *ok, *expired, *failed, *notFound, *idle, *lost, *back;  // %s: the network, %u: seconds
};

static const Text EN = {
  "en",
  "Wi-Fi setup",
  "Choose the Wi-Fi this board should join. After that it is only a device on that network, and this setup "
  "hotspot switches off.",
  "Wi-Fi network",
  "— choose —",
  "open",
  "Search again",
  "Searching for networks…",
  "or type its name (hidden network)",
  "Password (leave empty for an open network)",
  "Connect",
  "After sending, press the BOOT button on the board within 30 seconds. Nothing is stored before that.",
  "Choose a network or type its name (at most 32 characters).",
  "The password needs 8 to 63 characters (or none for an open network).",
  "The board is busy right now. Try again in a moment.",
  "Now press the BOOT button on the board to confirm “%s” (%u s left).",
  "Confirmed. Connecting to “%s”…",
  "Connected: the board is on “%s” now, at %s. The setup hotspot switches off; you can close this page.",
  "The button was not pressed in time. Nothing was stored.",
  "Could not connect. Is the password right? Nothing was stored.",
  "That network was not found. Nothing was stored.",
  "Nothing to confirm.",
  "No answer from the board. If the setup hotspot is gone, the board has joined your Wi-Fi. If not, join the "
  "hotspot again and reload this page.",
  "Back",
};

static const Text DE = {
  "de",
  "WLAN einrichten",
  "Wähle das WLAN, mit dem sich das Board verbinden soll. Danach ist es nur noch ein Gerät in diesem Netz, und "
  "dieses Einrichtungs-WLAN schaltet sich ab.",
  "WLAN",
  "— auswählen —",
  "offen",
  "Erneut suchen",
  "Suche nach Netzen…",
  "oder Namen eingeben (verstecktes WLAN)",
  "Passwort (leer lassen bei offenem WLAN)",
  "Verbinden",
  "Nach dem Absenden innerhalb von 30 Sekunden den BOOT-Taster am Board drücken. Vorher wird nichts gespeichert.",
  "Bitte ein WLAN auswählen oder den Namen eingeben (höchstens 32 Zeichen).",
  "Das Passwort braucht 8 bis 63 Zeichen (oder keins bei offenem WLAN).",
  "Das Board ist gerade beschäftigt. Bitte gleich noch einmal versuchen.",
  "Jetzt den BOOT-Taster am Board drücken, um „%s“ zu bestätigen (noch %u s).",
  "Bestätigt. Verbinde mit „%s“…",
  "Verbunden: Das Board ist jetzt in „%s“, Adresse %s. Das Einrichtungs-WLAN schaltet sich ab; diese Seite kann "
  "geschlossen werden.",
  "Der Taster wurde nicht rechtzeitig gedrückt. Es wurde nichts gespeichert.",
  "Verbindung fehlgeschlagen. Stimmt das Passwort? Es wurde nichts gespeichert.",
  "Dieses WLAN wurde nicht gefunden. Es wurde nichts gespeichert.",
  "Es gibt nichts zu bestätigen.",
  "Keine Antwort vom Board. Wenn das Einrichtungs-WLAN verschwunden ist, hat sich das Board mit deinem WLAN "
  "verbunden. Sonst erneut mit dem Einrichtungs-WLAN verbinden und diese Seite neu laden.",
  "Zurück",
};

enum { ERR_NONE = 0, ERR_SSID = SETUP_BAD_SSID, ERR_PASS = SETUP_BAD_PASS, ERR_BUSY };
const int NETS_MAX = 16;

static WebServer server(80);
static DNSServer dns;
static volatile bool want, up;
static TaskHandle_t task;

static const Text &text() { return setup_lang_de(server.header("Accept-Language").c_str()) ? DE : EN; }

static String esc(const char *s) {
  size_t cap = strlen(s) * 6 + 1;  // every character as its longest entity
  char *b = (char *)malloc(cap);
  if (!b) return "";
  setup_html_esc(s, b, cap);
  String out = b;
  free(b);
  return out;
}

static void headers() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; "
                    "form-action 'self'; base-uri 'none'");
}

static void redirect(const String &to) {
  headers();
  server.sendHeader("Location", to);
  server.send(303, "text/plain", "");
}

static void pageHead(String &h, const Text &t, const char *extra) {
  h += "<!doctype html><html lang=";
  h += t.lang;
  h += "><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
       "<meta name=color-scheme content=\"light dark\">";
  h += extra;
  h += "<title>";
  h += t.title;
  h += "</title><style>body{font:16px/1.45 system-ui,sans-serif;margin:0 auto;max-width:28em;padding:1em}"
       "h1{font-size:1.3em}label{display:block;margin:1em 0 .3em}"
       "select,input,button{font:inherit;width:100%;box-sizing:border-box;padding:.6em}button{margin-top:1.2em}"
       ".e{color:#b00020;font-weight:bold}.h{color:#666;font-size:.9em}"
       "@media(prefers-color-scheme:dark){.e{color:#ff6b6b}.h{color:#aaa}}</style></head><body><h1>";
  h += esc(BOARD_NAME_TEXT);
  h += " – ";
  h += t.title;
  h += "</h1>";
}

static void sendForm(int err) {
  const Text &t = text();
  static NetScan nets[NETS_MAX];  // only this task uses it
  bool running = false;
  int n = net_setup_scan(nets, NETS_MAX, &running);
  String h;
  h.reserve(4096);
  // Reload by itself only while there is no list yet: later a reload would empty a half-filled form.
  pageHead(h, t, !n && running && !err ? "<meta http-equiv=refresh content=3>" : "");
  if (err) {
    h += "<p class=e>";
    h += err == ERR_SSID ? t.badSsid : err == ERR_PASS ? t.badPass : t.busy;
    h += "</p>";
  }
  h += "<p>";
  h += t.intro;
  h += "</p><form method=post action=/join><label for=n>";
  h += t.net;
  h += "</label><select id=n name=net><option value=\"\">";
  h += running && !n ? t.scanning : t.choose;
  h += "</option>";
  for (int i = 0; i < n; i++) {
    char hex[2 * 32 + 2];
    setup_hex_enc(nets[i].ssid, hex, sizeof hex);  // the value is hex: any byte of a name survives the round trip
    int bars = nets[i].rssi >= -55 ? 4 : nets[i].rssi >= -67 ? 3 : nets[i].rssi >= -78 ? 2 : 1;
    h += "<option value=";
    h += hex;
    h += ">";
    h += esc(nets[i].ssid);  // names come from the air: escaped
    h += " ";
    for (int b = 0; b < 4; b++) h += b < bars ? "●" : "○";
    if (nets[i].open) {
      h += " (";
      h += t.open;
      h += ")";
    }
    h += "</option>";
  }
  h += "</select><p class=h><a href=\"/?scan=1\">";
  h += t.rescan;
  h += "</a>";
  if (running && n) {
    h += " · ";
    h += t.scanning;
  }
  h += "</p><label for=s>";
  h += t.manual;
  h += "</label><input id=s name=ssid maxlength=32 autocapitalize=none autocorrect=off spellcheck=false>"
       "<label for=p>";
  h += t.pass;
  h += "</label><input id=p name=pass type=password maxlength=63 autocomplete=off><button>";
  h += t.go;
  h += "</button></form><p class=h>";
  h += t.hint;
  h += "</p></body></html>";
  headers();
  server.send(200, "text/html; charset=utf-8", h);
}

// What the wait page says for a state; plain text (the caller escapes it for HTML or JSON).
static const char *stateName(int s) {
  switch (s) {
    case NS_WAIT: return "wait";
    case NS_TRYING: return "try";
    case NS_OK: return "ok";
    case NS_EXPIRED: return "expired";
    case NS_FAILED: return "fail";
    case NS_NOT_FOUND: return "notfound";
    default: return "idle";
  }
}

static void stateText(const Text &t, const NetSetup &s, char *out, size_t cap) {
  switch (s.state) {
    case NS_WAIT: snprintf(out, cap, t.wait, s.ssid, (unsigned)((s.left_ms + 999) / 1000)); break;
    case NS_TRYING: snprintf(out, cap, t.trying, s.ssid); break;
    case NS_OK: snprintf(out, cap, t.ok, s.ssid, s.ip); break;
    case NS_EXPIRED: strlcpy(out, t.expired, cap); break;
    case NS_FAILED: strlcpy(out, t.failed, cap); break;
    case NS_NOT_FOUND: strlcpy(out, t.notFound, cap); break;
    default: strlcpy(out, t.idle, cap);
  }
}

static void sendWait() {
  const Text &t = text();
  NetSetup s;
  net_setup_status(&s);
  char msg[320], lost[2 * 320];
  stateText(t, s, msg, sizeof msg);
  setup_json_esc(t.lost, lost, sizeof lost);
  String h;
  h.reserve(2048);
  bool live = s.state == NS_WAIT || s.state == NS_TRYING || s.state == NS_OK;
  pageHead(h, t, live ? "<noscript><meta http-equiv=refresh content=2></noscript>" : "");
  h += "<p id=m>";
  h += esc(msg);
  h += "</p><p><a href=\"/\">";
  h += t.back;
  h += "</a></p>";
  if (live) {  // poll /status once a second; textContent only, so nothing from the board is parsed as HTML
    h += "<script>var L=\"";
    h += lost;
    h += "\",n=0,d=0,m=document.getElementById('m');"
         "function r(){if(++n>=4&&!d)m.textContent=L;if(n<90)setTimeout(p,1500)}"
         "function p(){var x=new XMLHttpRequest();x.open('GET','/status',true);x.timeout=3000;"
         "x.onload=function(){var s;try{s=JSON.parse(x.responseText)}catch(e){return r()}"
         "n=0;m.textContent=s.msg;d=s.state=='ok';"
         "if(d||s.state=='wait'||s.state=='try')setTimeout(p,1000)};"
         "x.onerror=x.ontimeout=r;x.send()}"
         "setTimeout(p,1000)</script>";
  }
  h += "</body></html>";
  headers();
  server.send(200, "text/html; charset=utf-8", h);
}

static void sendStatus() {
  NetSetup s;
  net_setup_status(&s);
  char msg[320], m[2 * 320], ssid[6 * 33];
  stateText(text(), s, msg, sizeof msg);
  setup_json_esc(msg, m, sizeof m);
  setup_json_esc(s.ssid, ssid, sizeof ssid);
  char j[sizeof m + sizeof ssid + 96];
  snprintf(j, sizeof j, "{\"state\":\"%s\",\"left\":%u,\"ssid\":\"%s\",\"ip\":\"%s\",\"msg\":\"%s\"}", stateName(s.state),
           (unsigned)((s.left_ms + 999) / 1000), ssid, s.ip, m);
  headers();
  server.send(200, "application/json", j);
}

static void onJoin() {
  String typed = server.arg("ssid"), pass = server.arg("pass");
  char ssid[33] = "";
  if (typed.length()) {
    if (typed.length() <= 32) strlcpy(ssid, typed.c_str(), sizeof ssid);
  } else if (!setup_hex_dec(server.arg("net").c_str(), ssid, sizeof ssid)) {
    ssid[0] = 0;
  }
  int bad = pass.length() > 63 ? SETUP_BAD_PASS : setup_check(ssid, pass.c_str());
  if (bad) return sendForm(bad);
  if (!net_setup_submit(ssid, pass.c_str())) return sendForm(ERR_BUSY);
  redirect("/wait");
}

static void routes() {
  static const char *keep[] = {"Accept-Language"};
  server.collectHeaders(keep, 1);
  server.on("/", HTTP_GET, [] {
    if (server.hasArg("scan")) {
      net_setup_rescan();
      return redirect("/");  // so a reload does not search again
    }
    sendForm(ERR_NONE);
  });
  server.on("/join", HTTP_POST, onJoin);
  server.on("/wait", HTTP_GET, sendWait);
  server.on("/status", HTTP_GET, sendStatus);
  // Everything else (the phones' connectivity probes, any site typed in): to the page.
  server.onNotFound([] { redirect("http://" + WiFi.softAPIP().toString() + "/"); });
}

static void portalTask(void *) {
  for (;;) {
    if (want && !up) {
      dns.setTTL(10);
      dns.start(53, "*", WiFi.softAPIP());
      server.begin();
      up = true;
    } else if (!want && up) {
      server.stop();
      dns.stop();
      up = false;
    }
    if (up) server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(up ? 2 : 50));
  }
}

void portal_start() {
  want = true;
  if (task) return;
  routes();
  // Core 0, next to the Wi-Fi stack: loop() and its HID timing stay alone on core 1.
  xTaskCreatePinnedToCore(portalTask, "portal", 8192, NULL, 1, &task, 0);
}

void portal_stop() {
  want = false;
  for (int i = 0; i < 200 && up; i++) delay(10);
}
