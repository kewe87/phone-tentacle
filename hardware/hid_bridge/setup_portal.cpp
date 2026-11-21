// The Wi-Fi setup page, see setup_portal.h.
#include "setup_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "dbg.h"
#include "netcfg.h"
#include "kbd_layout.h"
#include "sdlog.h"
#include "src/setup/board_opts.h"
#include "version.h"
#include "src/setup/setup_text.h"
#include "src/web/web_auth.h"
#include <esp_random.h>

struct Text {
  const char *lang, *title, *intro, *net, *choose, *open, *rescan, *scanning, *manual, *pass, *go, *hint;
  const char *badSsid, *badPass, *busy;
  const char *web, *webPw, *webPw2, *webHint, *badWeb, *badWeb2;  // the web interface's password (optional)
  const char *wait, *trying, *ok, *expired, *failed, *notFound, *idle, *lost, *back;  // %s: the network, %u: seconds
  // the board's first settings, stored with the network
  const char *board, *name, *nameHint, *language, *kbd, *kbdHint, *sd, *sdOff, *sdLog, *sdVideo, *badName;
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
  "Remote control in the browser",
  "Password for the web interface (optional)",
  "Repeat the password",
  "With a password the board's web page (https://tentacle.local/) shows the phone and controls it. 8 to 64 "
  "characters. Leave both empty to keep the current one (none on a new board: the web page then only explains how "
  "to set one).",
  "The web interface's password needs 8 to 64 characters, no line breaks (or leave both fields empty).",
  "The two web interface passwords differ.",
  "Now press the BOOT button on the board to confirm “%s” (%u s left).",
  "Confirmed. Connecting to “%s”…",
  "Connected: the board is on “%s” now, at %s. The setup hotspot switches off; you can close this page. Its web "
  "interface: https://%s/ (in that network).",
  "The button was not pressed in time. Nothing was stored.",
  "Could not connect. Is the password right? Nothing was stored.",
  "That network was not found. Nothing was stored.",
  "Nothing to confirm.",
  "No answer from the board. If the setup hotspot is gone, the board has joined your Wi-Fi. If not, join the "
  "hotspot again and reload this page.",
  "Back",
  "The board",
  "Name",
  "How the board shows up: in Screen Mirroring, on the display, on the network. 1 to 20 letters, digits, space, "
  ". _ -",
  "Language (this page, the web interface, the display)",
  "The iPhone's keyboard layout",
  "As set on the iPhone under Settings > General > Keyboard > Hardware Keyboard: the board types for it.",
  "Recording on the microSD card",
  "Off",
  "Log (inputs and events; typed text never)",
  "Log and screen video",
  "The name needs 1 to 20 letters, digits, spaces or . _ - and no space at either end.",
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
  "Fernsteuerung im Browser",
  "Passwort für die Weboberfläche (optional)",
  "Passwort wiederholen",
  "Mit einem Passwort zeigt die Webseite des Boards (https://tentacle.local/) das iPhone und steuert es. 8 bis 64 "
  "Zeichen. Beide Felder leer lassen, um das bisherige zu behalten (ein neues Board hat keins: die Webseite erklärt "
  "dann nur, wie man eins setzt).",
  "Das Passwort der Weboberfläche braucht 8 bis 64 Zeichen, ohne Zeilenumbruch (oder beide Felder leer lassen).",
  "Die beiden Passwörter der Weboberfläche sind verschieden.",
  "Jetzt den BOOT-Taster am Board drücken, um „%s“ zu bestätigen (noch %u s).",
  "Bestätigt. Verbinde mit „%s“…",
  "Verbunden: Das Board ist jetzt in „%s“, Adresse %s. Das Einrichtungs-WLAN schaltet sich ab; diese Seite kann "
  "geschlossen werden. Seine Weboberfläche: https://%s/ (in diesem Netz).",
  "Der Taster wurde nicht rechtzeitig gedrückt. Es wurde nichts gespeichert.",
  "Verbindung fehlgeschlagen. Stimmt das Passwort? Es wurde nichts gespeichert.",
  "Dieses WLAN wurde nicht gefunden. Es wurde nichts gespeichert.",
  "Es gibt nichts zu bestätigen.",
  "Keine Antwort vom Board. Wenn das Einrichtungs-WLAN verschwunden ist, hat sich das Board mit deinem WLAN "
  "verbunden. Sonst erneut mit dem Einrichtungs-WLAN verbinden und diese Seite neu laden.",
  "Zurück",
  "Das Board",
  "Name",
  "So erscheint das Board: in der Bildschirmsynchronisierung, auf dem Display, im Netz. 1 bis 20 Buchstaben, "
  "Ziffern, Leerzeichen, . _ -",
  "Sprache (diese Seite, die Weboberfläche, das Display)",
  "Tastaturlayout des iPhones",
  "Wie am iPhone unter Einstellungen > Allgemein > Tastatur > Hardwaretastatur eingestellt: dafür tippt das Board.",
  "Aufzeichnung auf der microSD-Karte",
  "Aus",
  "Protokoll (Eingaben und Ereignisse; getippter Text nie)",
  "Protokoll und Bildschirmvideo",
  "Der Name braucht 1 bis 20 Buchstaben, Ziffern, Leerzeichen oder . _ - und kein Leerzeichen am Anfang oder Ende.",
};

enum { ERR_NONE = 0, ERR_SSID = SETUP_BAD_SSID, ERR_PASS = SETUP_BAD_PASS, ERR_BUSY, ERR_WEB, ERR_WEB2, ERR_NAME };
const int NETS_MAX = 16;

static WebServer server(80);
static DNSServer dns;
static volatile bool want, up;
static TaskHandle_t task;

// The page's language: chosen on the page (lang=de|en, kept in the form), else the browser's.
static bool pageDe() {
  String l = server.arg("lang");
  if (l == "de" || l == "en") return l == "de";
  return setup_lang_de(server.header("Accept-Language").c_str());
}
static const Text &text() { return pageDe() ? DE : EN; }

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
#if TENTACLE_DEBUG
  if (server.uri() != "/status")  // the wait page asks every second
    DBG("portal: %s %s", server.method() == HTTP_POST ? "POST" : "GET", server.uri().c_str());
#endif
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

// The look of every page (docs/brand): the style sheet and the small logo, everything inline (the setup hotspot
// has no internet, and the CSP allows nothing from elsewhere). One constant in flash; sendPage() writes it to the
// client as it is, so it is never copied to the heap. The pages are dark only.
static const char LOOK[] =
  "<style>body{margin:0;padding:12px;background:#0d0a12;color:#f6f1f4;"
  "font:16px/1.45 'Segoe UI',Inter,-apple-system,system-ui,sans-serif}"
  ".c{max-width:26em;margin:0 auto;padding:20px;border:1px solid #30263a;border-radius:20px;"
  "background:linear-gradient(#1d1724,#0d0a12)}"
  "svg{display:block;border-radius:22%}"
  "h1{font-size:1.35em;font-weight:800;margin:14px 0 18px}"
  "h2{font-size:1.1em;font-weight:800;margin:30px 0 4px}"
  "h1:after{content:'';display:block;width:56px;height:4px;margin-top:10px;border-radius:2px;"
  "background:linear-gradient(90deg,#ffb03a,#ff6a4d,#ff3d8b)}"
  "label{display:block;margin:18px 0 6px;font-weight:600}"
  "select,input,button{font:inherit;width:100%;box-sizing:border-box;min-height:48px;padding:10px 12px;"
  "border-radius:12px}"
  "select,input{color:#f6f1f4;background:#130d19;border:1px solid #6b5c78}"
  "button{margin-top:22px;border:0;font-weight:700;color:#1a1420;"
  "background:linear-gradient(90deg,#ffb03a,#ff6a4d,#ff3d8b)}"
  ":focus-visible{outline:3px solid #ffb03a;outline-offset:2px}"
  "a{color:#ff8a5c}.h{color:#a59bab;font-size:.9em}"
  ".e{color:#ff8a5c;font-weight:700;padding:10px 12px;border:1px solid #ff6a4d;border-radius:12px}"
  "#m{font-size:1.1em;padding:14px;border-left:4px solid #ff6a4d;border-radius:0 12px 12px 0;background:#130d19}"
  "</style></head><body><div class=c>"
  // docs/brand/logo-mark-small.svg on a 64-unit grid; the style sheet above rounds its corners
  "<svg viewBox='0 0 64 64' width=44 height=44 aria-hidden=true><linearGradient id=g gradientUnits=userSpaceOnUse "
  "x1=8 y1=59 x2=55 y2=9><stop stop-color='#ffb03a'/><stop offset=.5 stop-color='#ff6a4d'/><stop offset=1 "
  "stop-color='#ff3d8b'/></linearGradient><path fill='#17121d' d='M0 0h64v64H0z'/><rect x=32 y=10.2 width=24 "
  "height=45 rx=6 fill='#2a1c31' stroke='#f6f1f4' stroke-width=4.2 /><path d='M-0.6 67.9C-0.6 67.3-0.6 65.6-0.6 "
  "64.5C-0.6 63.3-0.5 62.2-0.5 61.1C-0.4 59.9-0.3 58.8-0.2 57.6C0 56.5 0.1 55.4 0.3 54.2C0.5 53.1 0.7 52 1 50.9C1.2"
  " 49.7 1.5 48.6 1.9 47.5C2.2 46.4 2.5 45.3 2.9 44.2C3.3 43.1 3.8 42 4.2 40.9C4.7 39.8 5.2 38.7 5.8 37.7C6.4 36.6 "
  "7 35.6 7.6 34.6C8.3 33.6 9 32.6 9.7 31.7C10.5 30.7 11.2 29.8 12.1 28.9C12.9 28 13.8 27.2 14.7 26.4C15.7 25.6 "
  "16.7 24.8 17.8 24.1C18.9 23.4 20 22.7 21.2 22.2C22.3 21.6 23.5 21.2 24.8 20.8C26.1 20.4 27.4 20.2 28.8 20C30.1 "
  "19.9 31.6 19.9 33 20.1C34.5 20.3 36 20.7 37.3 21.3C38.7 21.9 40 22.8 41.1 23.8C42.2 24.8 43.1 25.9 43.9 "
  "27.1C44.6 28.4 45.2 29.7 45.6 31.1C45.9 32.5 46.1 34.1 45.9 35.6C45.8 37 44.8 39.3 44.6 40A4 4 0 0 1 37.4 "
  "36.5C37.4 36.3 37.6 35.6 37.5 35.2C37.5 34.7 37.3 34.2 37.1 33.8C36.9 33.3 36.5 32.8 36.2 32.4C35.8 32 35.4 31.6"
  " 35 31.4C34.5 31.1 34.1 30.9 33.6 30.9C33.2 30.8 32.7 30.7 32.1 30.8C31.6 30.8 31.1 30.9 30.5 31.1C29.9 31.2 "
  "29.3 31.4 28.7 31.7C28.1 32 27.4 32.4 26.9 32.8C26.3 33.2 25.7 33.6 25.2 34.1C24.7 34.6 24.2 35.1 23.8 35.6C23.3"
  " 36.2 22.8 36.8 22.4 37.5C21.9 38.1 21.5 38.8 21.1 39.5C20.7 40.2 20.4 40.9 20.1 41.6C19.8 42.3 19.4 43.1 19.2 "
  "43.8C18.9 44.6 18.7 45.4 18.5 46.1C18.3 46.9 18.1 47.7 17.9 48.5C17.8 49.3 17.6 50.1 17.5 51C17.4 51.8 17.4 52.6"
  " 17.3 53.5C17.2 54.3 17.2 55.1 17.2 56C17.2 56.8 17.2 57.6 17.2 58.5C17.3 59.4 17.3 60.2 17.4 61C17.5 61.9 17.6 "
  "62.7 17.7 63.6C17.8 64.4 18 65.7 18.1 66.1A9.4 9.4 0 0 1-0.6 67.9Z' fill='url(#g)' stroke='#17121d' "
  "stroke-width=3.2 stroke-linejoin=round /><circle cx=41.5 cy=44 r=4.8 fill='#f6f1f4'/></svg>";

// A page goes out in three parts: its head up to the title, LOOK, its body.
static void pageHead(String &h, const Text &t, const char *extra) {
  h += "<!doctype html><html lang=";
  h += t.lang;
  h += "><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
       "<meta name=color-scheme content=dark>";
  h += extra;
  h += "<title>";
  h += t.title;
  h += "</title>";
}

static void pageTitle(String &b, const Text &t) {
  b += "<h1>";
  b += esc(board_name().c_str());
  b += " – ";
  b += t.title;
  b += "</h1>";
}

static void sendPage(const String &head, const String &body) {
  headers();
  server.setContentLength(head.length() + sizeof(LOOK) - 1 + body.length());  // known, so the reply is not chunked
  server.send(200, "text/html; charset=utf-8", head);
  server.sendContent_P(LOOK, sizeof(LOOK) - 1);
  server.sendContent(body);
}

static void option(String &h, const char *value, const char *label, bool selected) {
  h += "<option value=";
  h += value;
  if (selected) h += " selected";
  h += ">";
  h += label;
  h += "</option>";
}

// The board's first settings: name, language, the phone's keyboard, the card (boards with a slot).
static void boardFields(String &h, const Text &t, bool de) {
  h += "<h2>";
  h += t.board;
  h += "</h2><label for=bn>";
  h += t.name;
  h += "</label><input id=bn name=bname maxlength=20 autocapitalize=none autocorrect=off spellcheck=false value=\"";
  h += esc(board_name_saved().c_str());
  h += "\"><p class=h>";
  h += t.nameHint;
  h += "</p><label for=kb>";
  h += t.kbd;
  h += "</label><select id=kb name=kbd>";
  // the stored layout; a new board (us) on a German page: probably de
  String cur = kbd_layout_get();
  if (cur == "us" && de) cur = "de";
  option(h, "us", de ? "Englisch (US)" : "English (US)", cur == "us");
  option(h, "de", de ? "Deutsch" : "German", cur == "de");
  h += "</select><p class=h>";
  h += t.kbdHint;
  h += "</p>";
  if (sdlog_state() != SDLOG_NONE) {
    board_opts o;
    cfg_opts(&o);
    h += "<label for=sd>";
    h += t.sd;
    h += "</label><select id=sd name=sdlog>";
    option(h, "off", t.sdOff, o.sdlog == BO_SD_OFF);
    option(h, "log", t.sdLog, o.sdlog == BO_SD_LOG);
    option(h, "video", t.sdVideo, o.sdlog == BO_SD_VIDEO);
    h += "</select>";
  }
}

static void sendForm(int err) {
  const Text &t = text();
  static NetScan nets[NETS_MAX];  // only this task uses it
  bool running = false;
  int n = net_setup_scan(nets, NETS_MAX, &running);
  String head, h;
  h.reserve(4096);
  // Reload by itself only while there is no list yet: later a reload would empty a half-filled form.
  pageHead(head, t, !n && running && !err ? "<meta http-equiv=refresh content=3>" : "");
  pageTitle(h, t);
  if (err) {
    h += "<p class=e>";
    h += err == ERR_SSID ? t.badSsid : err == ERR_PASS ? t.badPass : err == ERR_WEB ? t.badWeb : err == ERR_WEB2 ? t.badWeb2
         : err == ERR_NAME ? t.badName : t.busy;
    h += "</p>";
  }
  bool de = pageDe();
  h += "<p class=h><a href=\"/?lang=de\">";
  h += de ? "<b>Deutsch</b>" : "Deutsch";
  h += "</a> · <a href=\"/?lang=en\">";
  h += de ? "English" : "<b>English</b>";
  h += "</a></p><p>";
  h += t.intro;
  h += "</p><form method=post action=/join><input type=hidden name=lang value=";
  h += de ? "de" : "en";
  h += "><label for=n>";
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
  h += "</select><p class=h><a href=\"/?scan=1&amp;lang=";
  h += de ? "de" : "en";
  h += "\">";
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
  h += "</label><input id=p name=pass type=password maxlength=63 autocomplete=off><label for=w>";
  h += t.webPw;
  h += "</label><input id=w name=webpw type=password maxlength=64 autocomplete=new-password><label for=w2>";
  h += t.webPw2;
  h += "</label><input id=w2 name=webpw2 type=password maxlength=64 autocomplete=new-password><p class=h>";
  h += t.webHint;
  h += "</p>";
  boardFields(h, t, de);
  h += "<button>";
  h += t.go;
  h += "</button></form><p class=h>";
  h += t.hint;
  h += "</p></div></body></html>";
  sendPage(head, h);
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
    case NS_OK: snprintf(out, cap, t.ok, s.ssid, s.ip, s.ip); break;
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
  char msg[400], lost[2 * 320];
  stateText(t, s, msg, sizeof msg);
  setup_json_esc(t.lost, lost, sizeof lost);
  String head, h;
  h.reserve(2048);
  bool live = s.state == NS_WAIT || s.state == NS_TRYING || s.state == NS_OK;
  pageHead(head, t, live ? "<noscript><meta http-equiv=refresh content=2></noscript>" : "");
  pageTitle(h, t);
  h += "<p id=m>";
  h += esc(msg);
  h += "</p><p><a href=\"/?lang=";
  h += pageDe() ? "de" : "en";
  h += "\">";
  h += t.back;
  h += "</a></p>";
  if (live) {  // poll /status once a second; textContent only, so nothing from the board is parsed as HTML
    h += "<script>var L=\"";
    h += lost;
    h += "\",n=0,d=0,m=document.getElementById('m');"
         "function r(){if(++n>=4&&!d)m.textContent=L;if(n<90)setTimeout(p,1500)}"
         "function p(){var x=new XMLHttpRequest();x.open('GET','/status'+location.search,true);x.timeout=3000;"
         "x.onload=function(){var s;try{s=JSON.parse(x.responseText)}catch(e){return r()}"
         "n=0;m.textContent=s.msg;d=s.state=='ok';"
         "if(d||s.state=='wait'||s.state=='try')setTimeout(p,1000)};"
         "x.onerror=x.ontimeout=r;x.send()}"
         "setTimeout(p,1000)</script>";
  }
  h += "</div></body></html>";
  sendPage(head, h);
}

static void sendStatus() {
  NetSetup s;
  net_setup_status(&s);
  char msg[400], m[2 * 400], ssid[6 * 33];
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
  // The web interface's password: optional; only its salted hash (web_auth.h) leaves this function.
  String web = server.arg("webpw"), web2 = server.arg("webpw2");
  static uint8_t rec[WA_REC_LEN];  // only this task uses it
  bool withWeb = web.length() || web2.length();
  if (withWeb) {
    if (web != web2) return sendForm(ERR_WEB2);
    if (wa_check_new(web.c_str()) != WA_OK) return sendForm(ERR_WEB);
    uint8_t salt[WA_SALT_LEN];
    esp_fill_random(salt, sizeof salt);  // the radio is on: a true random source
    bool made = wa_make(web.c_str(), salt, WA_ITERATIONS, rec);
    for (size_t i = 0; i < web.length(); i++) web[i] = 0, web2[i] = 0;
    if (!made) return sendForm(ERR_BUSY);
  }
  SetupExtras x = {};
  String name = server.arg("bname"), kbd = server.arg("kbd"), sd = server.arg("sdlog"), lang = server.arg("lang");
  name.trim();
  if (name.length() && !board_name_ok(name)) return sendForm(ERR_NAME);
  strlcpy(x.name, name.c_str(), sizeof x.name);
  if (lang == "de" || lang == "en") strlcpy(x.lang, lang.c_str(), sizeof x.lang);
  if (kbd.length() && strstr(kbd_layout_codes(), kbd.c_str()) && kbd.length() < sizeof x.kbd)
    strlcpy(x.kbd, kbd.c_str(), sizeof x.kbd);
  if (sd == "off" || sd == "log" || sd == "video") strlcpy(x.sdlog, sd.c_str(), sizeof x.sdlog);
  if (!net_setup_submit(ssid, pass.c_str(), withWeb ? rec : nullptr, &x)) return sendForm(ERR_BUSY);
  redirect(lang == "de" || lang == "en" ? "/wait?lang=" + lang : String("/wait"));
}

static void routes() {
  static const char *keep[] = {"Accept-Language"};
  server.collectHeaders(keep, 1);
  server.on("/", HTTP_GET, [] {
    if (server.hasArg("scan")) {
      net_setup_rescan();
      String l = server.arg("lang");  // so a reload does not search again
      return redirect(l == "de" || l == "en" ? "/?lang=" + l : String("/"));
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
      DBG("portal: up");
    } else if (!want && up) {
      server.stop();
      dns.stop();
      up = false;
      DBG("portal: down");
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
