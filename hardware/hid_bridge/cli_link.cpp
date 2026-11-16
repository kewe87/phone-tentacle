// CLI link, see cli_link.h.
#include "cli_link.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include "link.h"
#include "button.h"
#include "cli_tcp.h"
#include "dbg.h"
#include "src/cli/cli_proto.h"
#include "src/cli/pair_gate.h"
#include "src/ota/ota_gate.h"
#include "ota.h"
#include "logbuf.h"

const int MAX_CLIENTS = 4;
const int LINK = 0;                        // the pairing gate's owner: there is one link (cli_tcp.h)
const uint32_t VIDEO_TIMEOUT_MS = 1500;

struct PairedClient {
  uint8_t id[CLI_ID_LEN], key[CLI_KEY_LEN];
};
struct Session {
  volatile bool welcomed, authed;
  int client;
  uint8_t c2b[16], b2c[16];
  uint32_t rx, txCtrl, txVideo, txAudio;
};

static Preferences prefs;
static PairedClient clients[MAX_CLIENTS];
static int clientCount, nextClient;
static Session sess;
static uint8_t boardId[6];
static volatile uint32_t gen;  // sessions authenticated since boot
static SemaphoreHandle_t clientsLock;   // clients[] and its copy in NVS: cli_tcp's task pairs, loop() forgets
static volatile uint32_t lastFrameMs;   // an authenticated session's last frame (0: none since boot or unpair)
// Frame buffers in PSRAM (~9 KB): one for frames from the computer (cli_tcp's task: a command line, or a chunk of a
// firmware image, ota.h), one per sending task.
static uint8_t *rxText, *ctrlBuf, *ctrlOut, *videoOut, *audioOut;
static volatile bool audioOn;
const size_t TEXT_CAP = 520, OUT_CAP = 520 + CLI_OVERHEAD, VIDEO_CAP = CLI_TCP_MTU;
const size_t AUDIO_CAP = 1400 + CLI_OVERHEAD;  // one record: header, times, an AAC-ELD frame
const size_t RX_CAP = 1 + 4 + OTA_CHUNK_MAX;  // 'u', offset, image bytes (a command line is shorter)
const size_t BUF_SIZE = RX_CAP + TEXT_CAP + OUT_CAP + VIDEO_CAP + AUDIO_CAP;

extern "C" void cli_random(uint8_t *p, size_t n) { esp_fill_random(p, n); }

static void saveClients() {
  prefs.begin("cli", false);
  prefs.putBytes("c", clients, sizeof clients);
  prefs.putUChar("n", clientCount);
  prefs.putUChar("next", nextClient);
  prefs.end();
}

// One frame to the computer, and the end of its connection (cli_tcp.h).
static bool notify(const uint8_t *p, size_t n, uint32_t timeout_ms) { return cli_tcp_send(p, n, timeout_ms); }
static void drop() { cli_tcp_drop(); }

void cli_begin() {
  clientsLock = xSemaphoreCreateMutex();
  uint8_t *p = (uint8_t *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!p) p = (uint8_t *)malloc(BUF_SIZE);
  if (!p) return;
  rxText = p;
  ctrlBuf = p + RX_CAP;
  ctrlOut = ctrlBuf + TEXT_CAP;
  videoOut = ctrlOut + OUT_CAP;
  audioOut = videoOut + VIDEO_CAP;
  // The board's id: one of the addresses esp_read_mac derives from the base MAC in the eFuse (the type is only a
  // name: no radio and no Bluetooth stack is involved). Earlier firmware used the same one, so the name's suffix
  // (netcfg.cpp) and the id paired computers know the board by stay what they were.
  esp_read_mac(boardId, ESP_MAC_BT);
  prefs.begin("cli", true);
  clientCount = prefs.getUChar("n", 0);
  nextClient = prefs.getUChar("next", 0);
  if (prefs.getBytes("c", clients, sizeof clients) != sizeof clients || clientCount > MAX_CLIENTS) clientCount = nextClient = 0;
  prefs.end();
}

void cli_forget_all() {
  xSemaphoreTake(clientsLock, portMAX_DELAY);
  memset(clients, 0, sizeof clients);
  clientCount = nextClient = 0;
  saveClients();
  xSemaphoreGive(clientsLock);
  lastFrameMs = 0;  // nobody is paired any more: a new computer may ask at once
  if (sess.welcomed) drop();
}

static int findClient(const uint8_t *id) {
  for (int i = 0; i < clientCount; i++)
    if (!memcmp(clients[i].id, id, CLI_ID_LEN)) return i;
  return -1;
}

static void plain(uint8_t type, const uint8_t *p, size_t n) {
  uint8_t out[128];
  if (n > sizeof out - 1) n = sizeof out - 1;
  out[0] = type;
  memcpy(out + 1, p, n);
  notify(out, n + 1, 1000);
}

static void error(const char *why) { plain(CLI_ERROR, (const uint8_t *)why, strlen(why)); }

// ---- Pairing (docs/cli-protocol.md, "Pairing"): only while the pairing window is open (a double press of BOOT,
// button.h), one request at a time, and the key is stored only after a short press of BOOT. Everything below runs
// on cli_tcp's task: frames, cli_pair_poll(), cli_reset().
//   PAIR (c_pub, id, commit, name) -> PAIR_KEY (b_pub) -> PAIR_NONCE (n_c) -> PAIR_WAIT, code on the display,
//   LED blinking -> BOOT -> PAIRED, key in NVS. No press in 30 s, a dropped link, a bad nonce: ERROR, nothing kept.
struct PairPending {
  uint8_t id[CLI_ID_LEN], key[CLI_KEY_LEN], pub[32], commit[PAIR_COMMIT_LEN];
  char name[CF_TEXT];  // the computer's name, as the display shows it
};
static pair_gate gate;
static PairPending pend;

static void pairEnd(bool paired) {
  if (paired) pair_window_close();  // one double press, one computer
  confirm_clear(CONFIRM_PAIR);  // ours alone: nothing else asks for this kind
  memset(&pend, 0, sizeof pend);
  pg_end(&gate, paired, millis());
}

// A paired computer is driving the phone, or was a moment ago: no request may open now, so the BOOT button stays
// the stop button for as long as there is something to stop.
bool cli_in_use() {
  uint32_t last = lastFrameMs;
  return cli_up() || (last && millis() - last < PAIR_QUIET_MS);
}

static void pair(const uint8_t *p, size_t n) {
  pair_req r;
  if (!pair_parse(p, n, &r)) return error("bad pair request");
  if (!r.commit) return error("this CLI is too old to pair: update tentacle");
  const char *const IN_USE = "busy: board in use (a paired computer's session; end it, wait 10 s)";
  if (cli_in_use()) return error(IN_USE);  // before the window: a double press opens none while the board is in use
  // Not asked for at the board: nothing is shown, the LED stays as it is, and no lock counts up (the gate below
  // never sees the request).
  if (!pair_window_open()) return error("pairing closed: double-press BOOT on the board, then pair within 60 s");
  uint32_t wait = 0;
  char why[80];
  switch (pg_begin(&gate, LINK, false, millis(), &wait)) {
    case PG_OPEN: return error("busy: another pairing request is open");
    case PG_IN_USE: return error(IN_USE);
    case PG_LOCKED:
      snprintf(why, sizeof why, "busy: pairing locked for %lu s (a request was not confirmed)",
               (unsigned long)((wait + 999) / 1000));
      return error(why);
  }
  uint8_t priv[32];
  esp_fill_random(priv, sizeof priv);
  bool ok = cli_pair(priv, r.c_pub, pend.pub, pend.key);
  memset(priv, 0, sizeof priv);
  if (!ok) {
    pairEnd(false);
    return error("bad key");
  }
  memcpy(pend.id, r.client_id, CLI_ID_LEN);
  memcpy(pend.commit, r.commit, PAIR_COMMIT_LEN);
  pair_show_name(r.name, pend.name, sizeof pend.name);
  board_log("cli: pairing asked by \"%s\"", pend.name);
  plain(CLI_PAIR_KEY, pend.pub, 32);
}

static void pairNonce(const uint8_t *p, size_t n) {
  if (!pg_is(&gate, LINK, PG_KEYED)) return error("no pairing request open");
  uint8_t h[PAIR_COMMIT_LEN] = {0};
  if (n == PAIR_NONCE_LEN) cli_pair_commit(p, h);
  if (n != PAIR_NONCE_LEN || memcmp(h, pend.commit, sizeof h) || !pg_wait(&gate, LINK, millis())) {
    pairEnd(false);
    return error("bad pairing nonce");
  }
  char code[7], line[CF_TEXT];
  cli_pair_code(pend.key, p, code);
  snprintf(line, sizeof line, "Code %s", code);
  if (!confirm_request(CONFIRM_PAIR, pend.name, line, PAIR_CONFIRM_MS)) {  // the Wi-Fi setup page is asking
    pairEnd(false);
    return error("busy: the board waits for another confirmation (Wi-Fi setup)");
  }
  DBG("pair: waiting for BOOT, \"%s\", code %s", pend.name, code);
  uint8_t secs = PAIR_CONFIRM_MS / 1000;
  plain(CLI_PAIR_WAIT, &secs, 1);
}

void cli_pair_poll() {
  if (gate.state == PG_IDLE) return;
  if (pg_keyed_late(&gate, millis())) {
    pairEnd(false);
    return error("pairing timed out: no nonce");
  }
  if (cli_up()) {  // a paired computer's session came up meanwhile: BOOT is its stop button
    pairEnd(false);
    board_log("cli: pairing request dropped (a session started)");
    return error("busy: board in use (a paired computer's session; end it, wait 10 s)");
  }
  if (gate.state != PG_WAITING) return;
  int st = confirm_state(CONFIRM_PAIR);
  if (st == CF_WAITING) return;
  if (st != CF_ACCEPTED) {  // nothing was stored
    pairEnd(false);
    board_log("cli: pairing not confirmed");
    return error("pairing not confirmed: BOOT was not pressed within 30 s");
  }
  uint8_t out[32 + 6 + CLI_MAC_LEN];
  memcpy(out, pend.pub, 32);
  memcpy(out + 32, boardId, 6);
  cli_mac(pend.key, "paired", pend.id, CLI_ID_LEN, NULL, 0, out + 38);
  xSemaphoreTake(clientsLock, portMAX_DELAY);
  int i = findClient(pend.id);
  if (i < 0) {
    i = clientCount < MAX_CLIENTS ? clientCount++ : nextClient;  // full: replace the oldest
    nextClient = (i + 1) % MAX_CLIENTS;
  }
  memcpy(clients[i].id, pend.id, CLI_ID_LEN);
  memcpy(clients[i].key, pend.key, CLI_KEY_LEN);
  saveClients();
  xSemaphoreGive(clientsLock);
  board_log("cli: paired \"%s\" (BOOT pressed), %d paired", pend.name, clientCount);
  pairEnd(true);
  plain(CLI_PAIRED, out, sizeof out);
}

bool cli_pair_open() { return gate.state != PG_IDLE; }
int cli_paired() { return clientCount; }
void cli_board_id(uint8_t out[6]) { memcpy(out, boardId, 6); }

static void hello(const uint8_t *p, size_t n) {
  if (n != CLI_ID_LEN + CLI_NONCE_LEN) return error("bad hello");
  uint8_t key[CLI_KEY_LEN];
  xSemaphoreTake(clientsLock, portMAX_DELAY);
  int c = findClient(p);
  if (c >= 0) memcpy(key, clients[c].key, sizeof key);
  xSemaphoreGive(clientsLock);
  if (c < 0) return error("not paired");
  Session &s = sess;
  uint8_t bn[CLI_NONCE_LEN], out[CLI_NONCE_LEN + 6 + CLI_MAC_LEN];
  esp_fill_random(bn, sizeof bn);
  s.authed = false;
  bool ok = cli_session(key, p + CLI_ID_LEN, bn, s.c2b, s.b2c);
  if (ok) cli_mac(key, "hello", p + CLI_ID_LEN, CLI_NONCE_LEN, bn, CLI_NONCE_LEN, out + CLI_NONCE_LEN + 6);
  memset(key, 0, sizeof key);
  if (!ok) return error("session failed");
  s.client = c;
  s.rx = s.txCtrl = s.txVideo = s.txAudio = 0;
  s.welcomed = true;
  memcpy(out, bn, CLI_NONCE_LEN);
  memcpy(out + CLI_NONCE_LEN, boardId, 6);
  plain(CLI_WELCOME, out, sizeof out);
}

static void data(const uint8_t *p, size_t n) {
  Session &s = sess;
  uint8_t *text = rxText;
  uint32_t ctr;
  if (!s.welcomed || !text || n > RX_CAP + CLI_OVERHEAD) return;
  int len = cli_open(s.c2b, 'C', p, n, &ctr, text);
  if (len < 0) return;  // forged or for another session: ignore
  if (ctr != s.rx + 1) { drop(); return; }  // a replay, or a lost frame: start over
  s.rx = ctr;
  lastFrameMs = millis() | 1;
  if (!s.authed) {
    s.authed = true;  // one computer at a time: cli_tcp.h holds one connection
    pair_window_close();  // a session is up: BOOT is its stop button now
    audioOn = false;  // asked again by every session
    gen++;
    board_log("cli: session up");
  }
  if (len > 1 && text[0] == 'c') link_push_cmd((const char *)text + 1, len - 1);
  else if (len > 1 && text[0] == 'u') ota_chunk(text + 1, len - 1);  // a piece of a firmware image (`update`)
}

void cli_on_frame(const uint8_t *p, size_t n) {
  if (!n) return;
  switch (p[0]) {
    case CLI_PAIR: return pair(p + 1, n - 1);
    case CLI_PAIR_NONCE: return pairNonce(p + 1, n - 1);
    case CLI_HELLO: return hello(p + 1, n - 1);
    case CLI_DATA: return data(p, n);
  }
}

void cli_reset() {
  if (gate.state != PG_IDLE) {  // the link that asked to pair is gone: the request with it
    board_log("cli: pairing request withdrawn (link down)");
    pairEnd(false);
  }
  ota_link_down();  // an update that was coming in on this link stops; the old firmware keeps running
  sess.authed = sess.welcomed = false;
}

bool cli_up() { return sess.authed; }
uint32_t cli_gen() { return gen; }

bool cli_send_ctrl(char kind, const char *text, size_t n, uint32_t timeout_ms) {
  if (!cli_up()) return false;
  Session &s = sess;
  uint8_t *buf = ctrlBuf, *out = ctrlOut;
  size_t room = TEXT_CAP - 1;  // the buffer; a record (CLI_TCP_MTU) has room for more
  if (n > room) n = room;
  buf[0] = (uint8_t)kind;
  memcpy(buf + 1, text, n);
  size_t len = cli_seal(s.b2c, CLI_DATA, 'B', s.txCtrl + 1, buf, n + 1, out);
  if (!len || !notify(out, len, timeout_ms)) {
    drop();  // the CLI would see a counter gap anyway
    return false;
  }
  s.txCtrl++;
  return true;
}

size_t cli_video_room() {
  if (!cli_up() || !cli_tcp_up()) return 0;
  return CLI_TCP_MTU - CLI_OVERHEAD - 3;  // a record less the seal and the chunk's own header (VCHUNK_HDR)
}

bool cli_send_video(const uint8_t *chunk, size_t n) {
  if (!cli_up()) return false;
  Session &s = sess;
  uint8_t *out = videoOut;
  if (n > VIDEO_CAP - CLI_OVERHEAD) return false;
  size_t len = cli_seal(s.b2c, CLI_VIDEO, 'V', s.txVideo + 1, chunk, n, out);
  s.txVideo++;  // a chunk that fails is a gap the CLI handles like a seq gap
  return len && notify(out, len, VIDEO_TIMEOUT_MS);
}

void cli_audio(bool on) { audioOn = on; }
bool cli_audio_wanted() { return audioOn && cli_up(); }

bool cli_send_audio(const uint8_t *rec, size_t n) {
  if (!cli_up() || !audioOut || n + CLI_OVERHEAD > AUDIO_CAP) return false;
  Session &s = sess;
  size_t len = cli_seal(s.b2c, CLI_AUDIO, 'A', s.txAudio + 1, rec, n, audioOut);
  s.txAudio++;  // a record that fails is a seq gap for the CLI
  return len && notify(audioOut, len, VIDEO_TIMEOUT_MS);
}
