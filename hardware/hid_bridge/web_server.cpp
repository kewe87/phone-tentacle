// The web interface, see web_server.h.
//
// Tasks: the HTTPS server's own (ESP-IDF esp_https_server, core 0) runs every handler and is the only one that
// touches a TLS connection: everything sent on the WebSocket goes through httpd_queue_work(). The "web" task (core
// 0) starts and stops the server with the Wi-Fi, answers port 80, sends the page its status once a second and
// minds the BOOT confirmation of a new password. The web's video task (video_link.cpp, core 1) hands each chunk to
// the server's task and waits for it. loop() (core 1) runs the page's input. One mutex (mu) guards the shared state.
#include "web_server.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include "web_https.h"
#include "web_assets.h"
#include "button.h"
#include "cli_link.h"
#include "kbd_layout.h"
#include "src/hid/kbd_map.h"
#include "logbuf.h"
#include "netcfg.h"
#include "ota.h"
#include "version.h"
#include "video_link.h"
#include "board_settings.h"
#include "status_display.h"
#include "src/airplay/airplay.h"
#include "src/setup/setup_text.h"
#include "src/web/web_auth.h"
#include "src/web/web_cert.h"
#include "src/web/web_ctl.h"
#include "src/web/web_http.h"
#include "src/web/web_input.h"

String run(const String &line);  // hid_bridge.ino: the firmware's HID commands
const char *usbState();          // hid_bridge.ino: "ok" | "asleep" | "off"

// `abs X Y`: the page can point at the screen (the setting `abs`, as this boot runs).
static bool hasAbs() { return cfg_abs_active(); }

const uint16_t HTTPS_PORT = 443, HTTP_PORT = 80;
const char *const HOST_NAME = "tentacle.local";   // the mDNS name (src/airplay/mdns.c)
const size_t WEB_PAYLOAD = 8192;                   // video bytes per WebSocket frame
const uint32_t SESSION_IDLE_MS = 12 * 3600000u;    // a sign-in unused this long ends
const uint32_t PW_CONFIRM_MS = 30000;              // to press BOOT for a new password
const size_t WS_MSG_MAX = 640;                     // one message from the page
const size_t BODY_MAX = 512;                       // a form-like request body
const size_t OTA_PIECE = 4096;                     // = OTA_CHUNK_MAX (src/ota/ota_gate.h)

static SemaphoreHandle_t mu;
static volatile httpd_handle_t srv;
static String crtPem, keyPem;
static char certFp[WC_FP_LEN];

// ---- Under mu ----------------------------------------------------------------------------------------------------
static wc_state ctl;
static wa_limit lim;
static char sid[65];                // the one signed-in browser's token, hex ("" none)
static uint32_t sidAt;
static volatile int wsFd = -1;      // the page's WebSocket (written on the server's task only)
static volatile uint32_t wsGen;     // bumps with every page that connects
static volatile bool wsStream;      // it asked for the video
static wi_state inp;
static wi_queue q;
static bool webHolds;               // the page holds the button down (released when it loses control)
static uint32_t waitUntil;          // a `wait` in the queue (0: none)
static uint8_t newRec[WA_REC_LEN];  // a new password waiting for BOOT
enum { PW_NONE, PW_WAIT, PW_SAVED, PW_EXPIRED };
static int pwState;
static volatile uint32_t restartAt; // a restart once the reply is out (0: none)

static void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(mu); }

// ---- Sending on the WebSocket (from any task) -------------------------------------------------------------------
struct TxJob {
  int fd;
  uint32_t gen;
  size_t n;
  char data[];
};

static void txWork(void *arg) {  // the server's task
  TxJob *j = (TxJob *)arg;
  if (srv && j->fd == wsFd && j->gen == wsGen) {
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.final = true;
    f.payload = (uint8_t *)j->data;
    f.len = j->n;
    httpd_ws_send_frame_async(srv, j->fd, &f);
  }
  free(j);
}

static void sendText(const char *s) {
  httpd_handle_t h = srv;
  int fd = wsFd;
  if (fd < 0 || !h) return;
  size_t n = strlen(s);
  TxJob *j = (TxJob *)malloc(sizeof(TxJob) + n);
  if (!j) return;
  j->fd = fd, j->gen = wsGen, j->n = n;
  memcpy(j->data, s, n);
  if (httpd_queue_work(h, txWork, j) != ESP_OK) free(j);
}

static void sendErr(const char *why) {
  char e[200], m[240];
  setup_json_esc(why, e, sizeof e);
  snprintf(m, sizeof m, "{\"t\":\"err\",\"msg\":\"%s\"}", e);
  sendText(m);
}

// Video: one chunk at a time, handed to the server's task; the web's video task waits for it. A slow browser
// holds up only that task (it falls behind in the board's buffer and catches up later); the AirPlay receiver never
// waits.
static SemaphoreHandle_t vDone;
static volatile bool vStuck, vOk;
static const uint8_t *vP;
static size_t vN;
static int vFd;
static uint32_t vGen;

static void vWork(void *) {
  bool ok = false;
  if (srv && vFd == wsFd && vGen == wsGen) {
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_BINARY;
    f.final = true;
    f.payload = (uint8_t *)vP;
    f.len = vN;
    ok = httpd_ws_send_frame_async(srv, vFd, &f) == ESP_OK;
  }
  vOk = ok;
  vStuck = false;
  xSemaphoreGive(vDone);
}

static size_t webRoom() { return srv && wsFd >= 0 && wsStream && !vStuck ? WEB_PAYLOAD : 0; }
static uint32_t webGen() { return wsGen; }

static bool webSend(const uint8_t *p, size_t n) {
  httpd_handle_t h = srv;
  if (!h) return false;
  xSemaphoreTake(vDone, 0);  // a late answer to a job given up on
  vP = p, vN = n, vFd = wsFd, vGen = wsGen;
  vStuck = true;  // until the job ran, the chunk buffer is not to be touched: webRoom() is 0
  if (httpd_queue_work(h, vWork, NULL) != ESP_OK) {
    vStuck = false;
    return false;
  }
  return xSemaphoreTake(vDone, pdMS_TO_TICKS(15000)) == pdTRUE && vOk;
}

// ---- Status to the page, once a second and after a change ---------------------------------------------------------
static void pushStatus() {
  static const char *ap[] = {"idle", "connected", "streaming"};
  if (wsFd < 0) return;
  VideoStats v = video_stats();
  uint32_t waited;
  int vw = video_web_state(&waited);
  uint32_t now = millis();
  bool cli = cli_up();
  lock();
  wc_tick(&ctl, cli, now);
  bool control = ctl.control, watching = ctl.watching, cliActive = wc_cli_active(&ctl, cli, now);
  int queued = q.n, pw = pwState;
  unlock();
  int st = airplay_state();
  char s[360];
  snprintf(s, sizeof s,
           "{\"t\":\"st\",\"now\":%lu,\"ctl\":%d,\"watch\":%d,\"cli\":%d,\"stop\":%d,\"ap\":\"%s\",\"vage\":%lu,"
           "\"fps\":%.1f,\"kbps\":%lu,\"vw\":%d,\"vwait\":%lu,\"q\":%d,\"pw\":%d,\"usb\":\"%s\",\"abs\":%d}",
           (unsigned long)now, control, watching, cliActive, stop_active(), ap[st >= 0 && st <= 2 ? st : 0],
           (unsigned long)v.age_ms, v.fps, (unsigned long)v.kbps, vw, (unsigned long)waited, queued, pw, usbState(),
           hasAbs());
  sendText(s);
}

// ---- HTTP helpers ------------------------------------------------------------------------------------------------
static bool header(httpd_req_t *r, const char *name, char *out, size_t cap) {
  out[0] = 0;
  return httpd_req_get_hdr_value_str(r, name, out, cap) == ESP_OK;
}

static void common(httpd_req_t *r) {
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
  httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
}

static esp_err_t reply(httpd_req_t *r, const char *status, const char *json) {
  common(r);
  httpd_resp_set_status(r, status);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t replyStr(httpd_req_t *r, const String &json) { return reply(r, "200 OK", json.c_str()); }

static String jsonStr(const char *s) {
  size_t cap = strlen(s) * 6 + 3;
  char *b = (char *)malloc(cap);
  if (!b) return "\"\"";
  setup_json_esc(s, b, cap);
  String o = String("\"") + b + "\"";
  free(b);
  return o;
}

// The browser's own page: an Origin of https://<the Host it asked for>. Every POST and the WebSocket need it, so a
// page from elsewhere can not use the sign-in cookie (SameSite=Strict says the same; this does not rely on it).
static bool sameOrigin(httpd_req_t *r) {
  char host[96], h[96], origin[128];
  return header(r, "Host", host, sizeof host) && wh_host(host, h, sizeof h) && header(r, "Origin", origin, sizeof origin) &&
         wh_same_origin(origin, h);
}

static bool signedIn(httpd_req_t *r) {
  char cookie[256], tok[65];
  if (!header(r, "Cookie", cookie, sizeof cookie) || !wh_cookie(cookie, "tsid", tok, sizeof tok)) return false;
  uint32_t now = millis();
  lock();
  bool ok = sid[0] && wa_same(tok, sid) && now - sidAt < SESSION_IDLE_MS;
  if (ok) sidAt = now;
  else if (sid[0] && now - sidAt >= SESSION_IDLE_MS) sid[0] = 0;
  unlock();
  return ok;
}

// A POST's guard: false when it already replied (403 / 401).
static bool postOk(httpd_req_t *r, bool needSignIn = true) {
  if (!sameOrigin(r)) {
    reply(r, "403 Forbidden", "{\"err\":\"origin\"}");
    return false;
  }
  if (needSignIn && !signedIn(r)) {
    reply(r, "401 Unauthorized", "{\"err\":\"signin\"}");
    return false;
  }
  return true;
}

static bool body(httpd_req_t *r, char *out, size_t cap) {
  if (r->content_len >= cap) return false;
  size_t got = 0;
  while (got < r->content_len) {
    int n = httpd_req_recv(r, out + got, r->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return false;
    got += n;
  }
  out[got] = 0;
  return true;
}

static bool hasPassword() {
  uint8_t rec[WA_REC_LEN];
  return web_pw_get(rec);
}

static bool langDe(httpd_req_t *r) {
  String l = web_lang();
  if (l == "de") return true;
  if (l == "en") return false;
  char al[128];
  header(r, "Accept-Language", al, sizeof al);
  return setup_lang_de(al);
}

static void closeWs() {  // the server's task (handlers) or with the server stopping
  int fd = wsFd;
  if (fd >= 0 && srv) httpd_sess_trigger_close(srv, fd);
}

// ---- Pages -------------------------------------------------------------------------------------------------------
struct Asset {
  const uint8_t *gz;
  size_t len;
  const char *type;
};
static const Asset A_INDEX = {WEB_INDEX_HTML, sizeof WEB_INDEX_HTML, "text/html; charset=utf-8"};
static const Asset A_JS = {WEB_APP_JS, sizeof WEB_APP_JS, "text/javascript; charset=utf-8"};
static const Asset A_CSS = {WEB_APP_CSS, sizeof WEB_APP_CSS, "text/css; charset=utf-8"};
static const Asset A_ICON = {WEB_ICON_SVG, sizeof WEB_ICON_SVG, "image/svg+xml"};

static esp_err_t onAsset(httpd_req_t *r) {
  const Asset *a = (const Asset *)r->user_ctx;
  common(r);
  if (a == &A_INDEX) {
    // Scripts and styles only from here; the WebSocket only to the same host (older Safari does not count wss: as
    // 'self').
    static char csp[320];
    char host[96], h[96];
    if (!header(r, "Host", host, sizeof host) || !wh_host(host, h, sizeof h)) strlcpy(h, HOST_NAME, sizeof h);
    snprintf(csp, sizeof csp,
             "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self' "
             "wss://%s; frame-ancestors 'none'; base-uri 'none'; form-action 'none'",
             h);
    httpd_resp_set_hdr(r, "Content-Security-Policy", csp);
  }
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");  // a new firmware brings a new page
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  httpd_resp_set_type(r, a->type);
  return httpd_resp_send(r, (const char *)a->gz, a->len);
}

// GET /api/state: what the page shows before anything else (no sign-in needed).
static esp_err_t onState(httpd_req_t *r) {
  bool in = signedIn(r);
  lock();
  uint32_t wait = wa_limit_wait(&lim, millis());
  unlock();
  String j = "{\"pw\":" + String(hasPassword()) + ",\"auth\":" + String(in) + ",\"lang\":\"" + (langDe(r) ? "de" : "en") +
             "\",\"name\":" + jsonStr((board_name() + " " + board_suffix()).c_str()) + ",\"fw\":\"" FW_VERSION "\",\"lock\":" +
             String((wait + 999) / 1000) + ",\"abs\":" + String(hasAbs()) + "}";
  return replyStr(r, j);
}

// POST /api/login, the body is the password. One global lock after five wrong ones (web_auth.h).
static esp_err_t onLogin(httpd_req_t *r) {
  if (!postOk(r, false)) return ESP_OK;
  char pw[WA_PW_MAX + 2];
  if (!body(r, pw, sizeof pw)) return reply(r, "400 Bad Request", "{\"err\":\"bad\"}");
  uint8_t rec[WA_REC_LEN];
  if (!web_pw_get(rec)) return reply(r, "409 Conflict", "{\"err\":\"nopw\"}");
  uint32_t now = millis();
  lock();
  uint32_t wait = wa_limit_wait(&lim, now);
  unlock();
  if (wait) {
    char j[48];
    snprintf(j, sizeof j, "{\"err\":\"locked\",\"lock\":%lu}", (unsigned long)(wait + 999) / 1000);
    return reply(r, "429 Too Many Requests", j);
  }
  bool ok = wa_verify(pw, rec, sizeof rec);  // ~0.3 s: the server's task only
  memset(pw, 0, sizeof pw);
  lock();
  if (!ok) {
    wa_limit_fail(&lim, millis());
    wait = wa_limit_wait(&lim, millis());
    unlock();
    board_log("web: wrong password%s", wait ? ", sign-in locked" : "");
    char j[48];
    snprintf(j, sizeof j, "{\"err\":\"wrong\",\"lock\":%lu}", (unsigned long)(wait + 999) / 1000);
    return reply(r, "403 Forbidden", j);
  }
  wa_limit_ok(&lim);
  uint8_t raw[32];
  esp_fill_random(raw, sizeof raw);
  for (int i = 0; i < 32; i++) snprintf(sid + 2 * i, 3, "%02x", raw[i]);
  sidAt = millis();
  unlock();
  closeWs();  // a page of the browser signed in before has to sign in again
  static char cookie[140];
  snprintf(cookie, sizeof cookie, "tsid=%s; Path=/; Max-Age=43200; HttpOnly; Secure; SameSite=Strict", sid);
  httpd_resp_set_hdr(r, "Set-Cookie", cookie);
  board_log("web: signed in");
  return reply(r, "200 OK", "{\"ok\":true}");
}

static esp_err_t onLogout(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  lock();
  sid[0] = 0;
  unlock();
  closeWs();
  httpd_resp_set_hdr(r, "Set-Cookie", "tsid=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict");
  return reply(r, "200 OK", "{\"ok\":true}");
}

static esp_err_t onSettingsGet(httpd_req_t *r) {
  if (!signedIn(r)) return reply(r, "401 Unauthorized", "{\"err\":\"signin\"}");
  String j = "{\"name\":" + jsonStr(board_name().c_str()) + ",\"suffix\":\"" + board_suffix() + "\",\"kbd\":\"" +
             kbd_layout_get() + "\",\"lang\":\"" + web_lang() + "\",\"ssid\":" + jsonStr(WiFi.SSID().c_str()) +
             ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + ",\"host\":\"" + HOST_NAME +
             "\",\"fw\":\"" FW_VERSION "\",\"fp\":\"" + certFp + "\",\"heap\":" +
             String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) + ",\"psram\":" +
             String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + ",\"up\":" + String(millis() / 1000) +
             ",\"ota\":" + ota_json() + ",\"hw\":" + settings_json() + "}";
  return replyStr(r, j);
}

// POST /api/settings: lines key=value (name, kbd, lang; display, abs, battery, battery_div, oled_lang: the board's
// hardware settings, board_settings.h, with oled_lang for its `lang`). The name and abs take effect on the next boot.
static esp_err_t onSettingsPost(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  char b[BODY_MAX];
  if (!body(r, b, sizeof b)) return reply(r, "400 Bad Request", "{\"err\":\"bad\"}");
  bool renamed = false;
  for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = line, *v = eq + 1;
    if (!strcmp(k, "name")) {
      if (board_name() != v) {
        if (!board_set_name(v)) return reply(r, "400 Bad Request", "{\"err\":\"name\"}");
        renamed = true;
        board_log("web: board name set to \"%s\" (after a restart)", v);
      }
    } else if (!strcmp(k, "kbd")) {
      if (!kbd_layout_set(v)) return reply(r, "400 Bad Request", "{\"err\":\"kbd\"}");
    } else if (!strcmp(k, "lang")) {
      if (strcmp(v, "de") && strcmp(v, "en") && *v) return reply(r, "400 Bad Request", "{\"err\":\"lang\"}");
      web_set_lang(v);
      display_reconfigure();  // the OLED's language may follow the page's
    } else if (!strcmp(k, "display") || !strcmp(k, "abs") || !strcmp(k, "battery") || !strcmp(k, "battery_div") ||
               !strcmp(k, "oled_lang")) {
      if (settings_set(strcmp(k, "oled_lang") ? k : "lang", v).length()) {
        String e = String("{\"err\":\"") + k + "\"}";
        return reply(r, "400 Bad Request", e.c_str());
      }
    }
  }
  board_opts o;
  cfg_opts(&o);
  renamed |= o.abs != (cfg_abs_active() ? 1 : 0);  // the pointer changes with a restart too
  return reply(r, "200 OK", renamed ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true}");
}

static esp_err_t onRestart(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  board_log("web: restart asked");
  restartAt = millis() + 1000;
  return reply(r, "200 OK", "{\"ok\":true}");
}

// POST /api/password: "old\nnew". The new one is stored only once BOOT is pressed (GET /api/password: how it went).
static esp_err_t onPasswordPost(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  char b[2 * WA_PW_MAX + 4];
  if (!body(r, b, sizeof b)) return reply(r, "400 Bad Request", "{\"err\":\"bad\"}");
  char *nl = strchr(b, '\n');
  if (!nl) return reply(r, "400 Bad Request", "{\"err\":\"bad\"}");
  *nl = 0;
  const char *oldPw = b, *newPw = nl + 1;
  uint8_t rec[WA_REC_LEN];
  lock();
  uint32_t wait = wa_limit_wait(&lim, millis());
  unlock();
  if (wait) return reply(r, "429 Too Many Requests", "{\"err\":\"locked\"}");
  if (!web_pw_get(rec) || !wa_verify(oldPw, rec, sizeof rec)) {
    lock();
    wa_limit_fail(&lim, millis());
    unlock();
    memset(b, 0, sizeof b);
    return reply(r, "403 Forbidden", "{\"err\":\"wrong\"}");
  }
  if (wa_check_new(newPw) != WA_OK) {
    memset(b, 0, sizeof b);
    return reply(r, "400 Bad Request", "{\"err\":\"weak\"}");
  }
  uint8_t salt[WA_SALT_LEN], made[WA_REC_LEN];
  esp_fill_random(salt, sizeof salt);
  bool ok = wa_make(newPw, salt, WA_ITERATIONS, made);
  memset(b, 0, sizeof b);
  if (!ok || !confirm_request(CONFIRM_WEBPW, "New web password?", "from the web page", PW_CONFIRM_MS))
    return reply(r, "409 Conflict", "{\"err\":\"busy\"}");
  lock();
  memcpy(newRec, made, sizeof made);
  pwState = PW_WAIT;
  unlock();
  board_log("web: a new password waits for the BOOT button");
  return reply(r, "200 OK", "{\"state\":\"wait\"}");
}

// A new password from a paired computer's session (`webpw`), stored only once BOOT confirms it, as from the page.
const char *web_pw_request(const char *pw) {
  if (wa_check_new(pw) != WA_OK) return "err weak: 8 to 64 characters";
  uint8_t salt[WA_SALT_LEN], made[WA_REC_LEN];
  esp_fill_random(salt, sizeof salt);
  if (!wa_make(pw, salt, WA_ITERATIONS, made)) return "err failed";
  if (!confirm_request(CONFIRM_WEBPW, "New web password?", "from a computer", PW_CONFIRM_MS)) {
    memset(made, 0, sizeof made);
    return "err busy: another confirmation is waiting";
  }
  lock();
  memcpy(newRec, made, sizeof made);
  pwState = PW_WAIT;
  unlock();
  memset(made, 0, sizeof made);
  board_log("web: a new password from a computer waits for the BOOT button");
  return "ok wait: press BOOT on the board to confirm";
}

static esp_err_t onPasswordGet(httpd_req_t *r) {
  if (!signedIn(r)) return reply(r, "401 Unauthorized", "{\"err\":\"signin\"}");
  static const char *names[] = {"none", "wait", "saved", "expired"};
  lock();
  int s = pwState;
  unlock();
  char j[48];
  snprintf(j, sizeof j, "{\"state\":\"%s\"}", names[s]);
  return reply(r, "200 OK", j);
}

// Firmware update: begin ("SIZE SHA256 SIGHEX|- VERSION", as the CLI's `update begin`), the image as one body,
// end. The same checks as from the CLI (ota.cpp): a release build takes signed images only.
static esp_err_t onOtaBegin(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  char b[BODY_MAX];
  if (!body(r, b, sizeof b)) return reply(r, "400 Bad Request", "{\"err\":\"bad\"}");
  String rep = ota_web(String("begin ") + b);
  board_log("web: update begin: %s", rep.c_str());
  return replyStr(r, "{\"reply\":" + jsonStr(rep.c_str()) + "}");
}

static esp_err_t onOtaData(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  uint8_t *buf = (uint8_t *)malloc(4 + OTA_PIECE);
  if (!buf) return reply(r, "503 Service Unavailable", "{\"err\":\"memory\"}");
  size_t off = 0, total = r->content_len;
  int timeouts = 0;
  bool ok = true;
  while (off < total && ok) {
    size_t want = min(OTA_PIECE, total - off), got = 0;
    while (got < want) {
      int n = httpd_req_recv(r, (char *)buf + 4 + got, want - got);
      if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
      if (n <= 0) {
        ok = false;
        break;
      }
      got += n;
    }
    if (!ok) break;
    buf[0] = off, buf[1] = off >> 8, buf[2] = off >> 16, buf[3] = off >> 24;
    ota_web_chunk(buf, 4 + got);
    off += got;
  }
  free(buf);
  String st = ota_web("state");
  return replyStr(r, "{\"reply\":" + jsonStr(st.c_str()) + "}");
}

static esp_err_t onOtaEnd(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  String rep = ota_web("end");
  if (rep.startsWith("ok")) restartAt = millis() + 1500;  // after this reply
  return replyStr(r, "{\"reply\":" + jsonStr(rep.c_str()) + "}");
}

static esp_err_t onOtaAbort(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  return replyStr(r, "{\"reply\":" + jsonStr(ota_web("abort").c_str()) + "}");
}

// A stub: the repository is private, so the board has nowhere to look (docs/manual.md, "Web interface").
static esp_err_t onUpdateCheck(httpd_req_t *r) {
  if (!signedIn(r)) return reply(r, "401 Unauthorized", "{\"err\":\"signin\"}");
  return reply(r, "200 OK", "{\"available\":false,\"why\":\"nosource\",\"fw\":\"" FW_VERSION "\"}");
}

// ---- The WebSocket ------------------------------------------------------------------------------------------------
struct WsTag {
  int fd;
  uint32_t gen;
};

static void wsGone(void *ctx) {  // the server's task: the page's connection closed
  WsTag *t = (WsTag *)ctx;
  bool was = false;
  lock();
  if (t->fd == wsFd && t->gen == wsGen) {
    was = true;
    wsFd = -1;
    wsStream = false;
    wc_disconnect(&ctl);
    wi_queue_clear(&q);
    waitUntil = 0;
  }
  unlock();
  if (was) video_web_stream(false);
  free(t);
}

// One message from the page (src/web/web_input.h; plus take, release, stream on|off|on at IDR_TS N, ping).
static void onMessage(const char *m) {
  uint32_t now = millis();
  if (!strcmp(m, "ping")) return;
  if (!strcmp(m, "stream on") || !strcmp(m, "stream off")) {
    wsStream = m[7] == 'o' && m[8] == 'n';
    video_web_stream(wsStream);
    return;
  }
  unsigned long ts, at;
  char extra;
  if (sscanf(m, "stream on at %lu %lu%c", &ts, &at, &extra) == 2) {
    // The page kept its decoder across a reconnect: go on after its last AU. Resume first, then let it take chunks
    // (webRoom()): the video task keeps the resume until it can send, and looks again within 20 ms.
    bool ok = video_web_stream_at(ts, at);
    wsStream = true;
    board_log("web: the page resumes the video at AU %lu: %s", at, ok ? "yes" : "no, from the next whole chain");
    return;
  }
  if (!strcmp(m, "take") || !strcmp(m, "release")) {
    lock();
    if (m[0] == 't') wc_take(&ctl, now);
    else wc_release(&ctl);
    unlock();
    board_log("web: the page %s control", m[0] == 't' ? "took" : "gave back");
    pushStatus();
    return;
  }
  char out[8][WI_CMD_MAX];
  int n = 0;
  const char *why = "";
  bool state = !strncmp(m, "size ", 5) || !strncmp(m, "bars ", 5);  // no input: always taken
  lock();
  wc_tick(&ctl, cli_up(), now);
  int res = WI_ERR;
  if (!state && !ctl.control) why = "control";
  else if (!state && stop_active()) why = "stopped";
  else { int l = kbd_map_find(kbd_layout_get()); inp.layout = l < 0 ? KBD_US : l; res = wi_message(&inp, m, out, 8, &n, &why); }
  bool full = false;
  if (res == WI_INPUT) {
    bool move = !strncmp(m, "move ", 5);
    for (int i = 0; i < n && !full; i++) full = !wi_queue_push(&q, out[i], move);
    wc_web_input(&ctl, now);
  }
  unlock();
  if (res == WI_ERR) sendErr(why);
  else if (full) sendErr("busy");
#if TENTACLE_DEBUG
  if (res == WI_ERR || full) board_log("D web: '%.40s' refused: %s", m, full ? "queue full" : why);
#endif
}

// A short text frame to fd, from the server's task, before the connection is closed: the page tells the person why.
static void bye(int fd, const char *why) {
  char m[64];
  snprintf(m, sizeof m, "{\"t\":\"bye\",\"why\":\"%s\"}", why);
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT;
  f.final = true;
  f.payload = (uint8_t *)m;
  f.len = strlen(m);
  if (srv) httpd_ws_send_frame_async(srv, fd, &f);
}

// The page's ticket for its WebSocket: ESP-IDF 5.5 no longer calls the handler for the handshake, so the cookie and
// the origin can not be checked there. A signed-in page of this origin asks for a ticket (POST, so origin-checked),
// opens the socket and sends `hello TICKET` first; the ticket works once, for a short while.
static char wsTicket[33];
static uint32_t wsTicketAt;
static const uint32_t TICKET_MS = 15000;

static esp_err_t onWsTicket(httpd_req_t *r) {
  if (!postOk(r)) return ESP_OK;
  uint8_t raw[16];
  esp_fill_random(raw, sizeof raw);
  lock();
  for (int i = 0; i < 16; i++) snprintf(wsTicket + 2 * i, 3, "%02x", raw[i]);
  wsTicketAt = millis();
  char j[64];
  snprintf(j, sizeof j, "{\"ticket\":\"%s\"}", wsTicket);
  unlock();
  return reply(r, "200 OK", j);
}

static bool takeTicket(const char *t) {  // under the lock
  bool ok = wsTicket[0] && strlen(t) == 32 && wa_same(t, wsTicket) && millis() - wsTicketAt < TICKET_MS;
  wsTicket[0] = 0;  // once, right or wrong
  return ok;
}

// The page that sent a valid hello becomes the one page; an open one is told and closed.
static void adopt(httpd_req_t *r, int fd) {
  WsTag *t = (WsTag *)malloc(sizeof(WsTag));
  if (!t) return;
  int old = wsFd;
  wsGen++;
  wsFd = fd;
  wsStream = false;
  wc_connect(&ctl, millis());
  wi_init(&inp);
  wi_queue_clear(&q);
  waitUntil = 0;
  t->fd = fd, t->gen = wsGen;
  r->sess_ctx = t;  // freed (wsGone) when the connection closes
  r->free_ctx = wsGone;
  if (old >= 0 && old != fd) {  // one page at a time: the newest
    bye(old, "replaced");
    httpd_sess_trigger_close(srv, old);
    board_log("web: a newer page replaced the open one");
  }
}

static esp_err_t onWs(httpd_req_t *r) {
  int fd = httpd_req_to_sockfd(r);
  if (r->method == HTTP_GET) return ESP_OK;  // older ESP-IDF: the handshake; the page proves itself with hello
  httpd_ws_frame_t f = {};
  if (httpd_ws_recv_frame(r, &f, 0) != ESP_OK || f.len > WS_MSG_MAX) return ESP_FAIL;
  char buf[WS_MSG_MAX + 1];
  f.payload = (uint8_t *)buf;
  if (f.len && httpd_ws_recv_frame(r, &f, f.len) != ESP_OK) return ESP_FAIL;
  buf[f.len] = 0;
  if (f.type != HTTPD_WS_TYPE_TEXT) return fd == wsFd ? ESP_OK : ESP_FAIL;
  if (fd != wsFd) {
    if (strncmp(buf, "hello ", 6)) return ESP_FAIL;  // a page replaced by a newer one, or no hello
    lock();
    bool ok = takeTicket(buf + 6);
    if (ok) adopt(r, fd);
    unlock();
    if (!ok) {
      board_log("web: page refused: no valid ticket");
      bye(fd, "signin");
      return ESP_FAIL;
    }
    board_log("web: page connected");
    return ESP_OK;
  }
  onMessage(buf);
  return ESP_OK;
}

// ---- The server --------------------------------------------------------------------------------------------------
static bool route(httpd_handle_t h, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *), const void *ctx = nullptr,
                  bool ws = false) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = m;
  u.handler = fn;
  u.user_ctx = (void *)ctx;
  u.is_websocket = ws;
  return httpd_register_uri_handler(h, &u) == ESP_OK;
}

static int rng(void *, unsigned char *out, size_t n) {
  esp_fill_random(out, n);  // with the radio on: a true random source
  return 0;
}

// The certificate: from NVS, or made now (about a second).
static bool loadCert() {
  if (crtPem.length() && keyPem.length()) return true;
  Preferences p;
  p.begin("web", false);
  String crt = p.getString("crt", ""), key = p.getString("key", "");
  int32_t build = wc_build_days(__DATE__), floor = wc_days(2026, 1, 1);
  if (!key.length() || !wc_good(crt.c_str(), build)) {
    char *k = (char *)malloc(WC_KEY_PEM_MAX), *c = (char *)malloc(WC_CRT_PEM_MAX);
    int e = k && c ? wc_make(HOST_NAME, (uint32_t)WiFi.localIP(), max(floor, build - 1), rng, nullptr, k, c) : -1;
    if (!e) {
      key = k, crt = c;
      p.putString("key", key);
      p.putString("crt", crt);
      board_log("web: made a new certificate for %s and %s", HOST_NAME, WiFi.localIP().toString().c_str());
    } else {
      board_log("web: making the certificate failed (%d)", e);
    }
    free(k), free(c);
  }
  p.end();
  if (!wc_info(crt.c_str(), certFp, nullptr)) return false;
  crtPem = crt, keyPem = key;
  board_log("web: certificate SHA-256 %s", certFp);
  return true;
}

static void startServer() {
  if (srv || !loadCert()) return;
  uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  web_https_opts o = {};
  o.port = HTTPS_PORT;
  o.ctrl_port = 32770;          // apart from any other httpd's
  o.handshake_ms = 10000;
  o.priority = 3;               // above the video tasks (1, 2), below Wi-Fi and lwIP
  o.stack = 10240;              // a TLS handshake (ECDHE-ECDSA) needs most of it
  o.core = 0;
  o.sockets = 3;                // lwIP has 16 for everything: AirPlay, the CLI, mDNS, port 80
  o.handlers = 20;
  o.header_max = 1536;          // a WebSocket upgrade with cookie and language
  httpd_handle_t h = web_https_start(crtPem.c_str(), keyPem.c_str(), &o);
  if (!h) {
    board_log("web: HTTPS server failed to start");
    return;
  }
  route(h, "/", HTTP_GET, onAsset, &A_INDEX);
  route(h, "/app.js", HTTP_GET, onAsset, &A_JS);
  route(h, "/app.css", HTTP_GET, onAsset, &A_CSS);
  route(h, "/icon.svg", HTTP_GET, onAsset, &A_ICON);
  route(h, "/api/state", HTTP_GET, onState);
  route(h, "/api/login", HTTP_POST, onLogin);
  route(h, "/api/logout", HTTP_POST, onLogout);
  route(h, "/api/settings", HTTP_GET, onSettingsGet);
  route(h, "/api/settings", HTTP_POST, onSettingsPost);
  route(h, "/api/restart", HTTP_POST, onRestart);
  route(h, "/api/password", HTTP_GET, onPasswordGet);
  route(h, "/api/password", HTTP_POST, onPasswordPost);
  route(h, "/api/ota/begin", HTTP_POST, onOtaBegin);
  route(h, "/api/ota/data", HTTP_POST, onOtaData);
  route(h, "/api/ota/end", HTTP_POST, onOtaEnd);
  route(h, "/api/ota/abort", HTTP_POST, onOtaAbort);
  route(h, "/api/update/check", HTTP_GET, onUpdateCheck);
  route(h, "/api/wsticket", HTTP_POST, onWsTicket);
  route(h, "/ws", HTTP_GET, onWs, nullptr, true);
  vStuck = false;
  srv = h;
  if (!video_web_begin(webRoom, webSend, webGen, WEB_PAYLOAD)) board_log("web: no video (PSRAM)");
  board_log("web: https://%s/ and https://%s/ up (internal heap %u -> %u)", HOST_NAME, WiFi.localIP().toString().c_str(),
            (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void stopServer() {
  httpd_handle_t h = srv;
  if (!h) return;
  srv = nullptr;
  web_https_stop(h);  // closes the page's connection too (wsGone)
  lock();
  wsFd = -1;
  wsStream = false;
  wc_disconnect(&ctl);
  wi_queue_clear(&q);
  unlock();
  video_web_stream(false);
  board_log("web: down (no Wi-Fi)");
}

// ---- Port 80: only points to https:// (the setup hotspot has its own page there) --------------------------------
static int http80 = -1;

static void open80() {
  if (http80 >= 0) return;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(HTTP_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (sockaddr *)&a, sizeof a) || listen(s, 2)) {
    close(s);
    return;
  }
  http80 = s;
}

static void close80() {
  if (http80 >= 0) close(http80);
  http80 = -1;
}

static void serve80(uint32_t waitMs) {
  if (http80 < 0) {
    vTaskDelay(pdMS_TO_TICKS(waitMs));
    return;
  }
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(http80, &rd);
  timeval tv = {(time_t)(waitMs / 1000), (suseconds_t)(waitMs % 1000) * 1000};
  if (select(http80 + 1, &rd, nullptr, nullptr, &tv) <= 0) return;
  int c = accept(http80, nullptr, nullptr);
  if (c < 0) return;
  timeval rt = {2, 0};
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &rt, sizeof rt);
  char req[768];
  int n = recv(c, req, sizeof req - 1, 0);
  req[n > 0 ? n : 0] = 0;
  char host[96] = "";
  for (char *p = req; (p = strchr(p, '\n')); ) {  // the Host header, any case
    p++;
    if (!strncasecmp(p, "host:", 5)) {
      p += 5;
      while (*p == ' ') p++;
      char raw[96];
      size_t k = 0;
      while (p[k] && p[k] != '\r' && p[k] != '\n' && k < sizeof raw - 1) raw[k] = p[k], k++;
      raw[k] = 0;
      wh_host(raw, host, sizeof host);
      break;
    }
  }
  if (!host[0]) strlcpy(host, WiFi.localIP().toString().c_str(), sizeof host);
  char resp[256];
  int len = snprintf(resp, sizeof resp,
                     "HTTP/1.1 301 Moved Permanently\r\nLocation: https://%s/\r\nContent-Length: 0\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", host);
  send(c, resp, len, 0);
  close(c);
}

// ---- The web task ------------------------------------------------------------------------------------------------
static void webTask(void *) {
  uint32_t lastTick = 0, upSince = 0;
  for (;;) {
    bool net = !net_setup_active() && WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0;
    uint32_t now = millis();
    if (net && !upSince) upSince = now | 1;
    if (!net) upSince = 0;
    if (net && !srv && now - upSince > 1500) {  // the address settled; also after a Wi-Fi change
      startServer();
      if (srv) open80();
    } else if (!net && srv) {
      close80();
      stopServer();
    }
    serve80(250);

    now = millis();
    if (now - lastTick >= 1000) {
      lastTick = now;
      // A new password: stored once BOOT confirmed it.
      lock();
      bool waiting = pwState == PW_WAIT;
      unlock();
      if (waiting) {
        int c = confirm_state(CONFIRM_WEBPW);
        if (c != CF_WAITING) {
          confirm_clear(CONFIRM_WEBPW);
          lock();
          if (c == CF_ACCEPTED) web_pw_set(newRec);
          memset(newRec, 0, sizeof newRec);
          pwState = c == CF_ACCEPTED ? PW_SAVED : PW_EXPIRED;
          unlock();
          board_log("web: new password %s", c == CF_ACCEPTED ? "confirmed and stored" : "not confirmed, not stored");
        }
      }
      pushStatus();
      if (restartAt && (int32_t)(now - restartAt) >= 0) {
        board_log("web: restarting");
        delay(200);
        ESP.restart();
      }
    }
  }
}

// ---- Public ------------------------------------------------------------------------------------------------------
void web_begin() {
  mu = xSemaphoreCreateMutex();
  vDone = xSemaphoreCreateBinary();
  wc_init(&ctl);
  wa_limit_init(&lim);
  wi_init(&inp);
  wi_queue_init(&q);
  // Core 0, next to Wi-Fi and the setup page: loop() and its HID timing stay alone on core 1.
  xTaskCreatePinnedToCore(webTask, "web", 6144, NULL, 1, NULL, 0);
}

bool web_loop() {
  if (!mu) return false;
  uint32_t now = millis();
  char cmd[WI_CMD_MAX];
  bool got = false, letGo = false;
  lock();
  bool driving = ctl.control && ctl.connected && !stop_active();
  if (!driving) {
    if (q.n) wi_queue_clear(&q);
    letGo = webHolds;
    webHolds = false;
    waitUntil = 0;
  } else if (!waitUntil || (int32_t)(now - waitUntil) >= 0) {
    waitUntil = 0;
    got = wi_queue_pop(&q, cmd);
  }
  unlock();
  if (letGo) {  // control went elsewhere (or stop) with the button held: let go of it
    run("release");
    return true;
  }
  if (!got) return false;
  if (!strncmp(cmd, "wait ", 5)) {
    lock();
    waitUntil = (now + (uint32_t)atoi(cmd + 5)) | 1;
    unlock();
    return true;
  }
  String rep = run(cmd);
  bool ok = rep == "ok";
#if TENTACLE_DEBUG
  board_log("D web: %s -> %s", cmd, rep.c_str());
#endif
  lock();
  if (ok && !strcmp(cmd, "press")) webHolds = true;
  else if (ok && (!strcmp(cmd, "release") || !strncmp(cmd, "press_for", 9))) webHolds = false;
  if (!ok) wi_queue_clear(&q);  // the rest of a gesture makes no sense now
  unlock();
  if (!ok) sendErr(rep.c_str());
  return true;
}

uint32_t web_wait_ms(uint32_t most) {
  if (!mu) return most;
  lock();
  uint32_t w = most;
  if (q.n && ctl.control) {
    if (!waitUntil) w = 0;
    else {
      int32_t left = (int32_t)(waitUntil - millis());
      w = left <= 0 ? 0 : min(most, (uint32_t)left);
    }
  }
  unlock();
  return w;
}

const char *web_cli_gate(const char *line) {
  if (!mu) return nullptr;
  const char *sp = strchr(line, ' ');
  size_t n = sp ? (size_t)(sp - line) : strlen(line);
  if ((n == 4 && !strncmp(line, "ping", 4)) || (n == 4 && !strncmp(line, "pace", 4))) return nullptr;
  uint32_t now = millis();
  lock();
  wc_tick(&ctl, true, now);
  bool busy = ctl.control && ctl.connected;
  if (!busy) wc_cli_input(&ctl, now);
  unlock();
  return busy ? "err busy: the web interface has control of the phone (release it there, or it lapses 5 min after "
                "the page's last input)"
              : nullptr;
}

bool web_in_use() {
  if (!mu) return false;
  lock();
  bool u = ctl.control && ctl.connected;
  unlock();
  return u;
}
