// CLI link, see cli_link.h.
#include "cli_link.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include "ble_link.h"
#include "button.h"
#include "cli_tcp.h"
#include "dbg.h"
#include "src/cli/cli_proto.h"
#include "src/cli/pair_gate.h"
#include "logbuf.h"

const int MAX_CLIENTS = 4;
const int BLE_SLOTS = 2;                   // ble_link's MAX_PEERS
const int SLOTS = BLE_SLOTS + 1;           // + the Wi-Fi link (cli_tcp.h)
const uint32_t VIDEO_TIMEOUT_MS = 1500;    // as for the app's video

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
static Session sessions[SLOTS];
static uint8_t boardId[6];
static volatile uint32_t gen;  // sessions authenticated since boot
static SemaphoreHandle_t clientsLock;   // clients[] and its copy in NVS: cli_tcp's task pairs, loop() forgets
static volatile uint32_t lastFrameMs;   // an authenticated session's last frame (0: none since boot or unpair)
// Frame buffers in PSRAM (~3.5 KB): one for frames from the computer per receiving task (NimBLE host, cli_tcp),
// one per sending task.
static uint8_t *rxText[2], *ctrlBuf, *ctrlOut, *videoOut, *audioOut;
static volatile bool audioOn;
const size_t TEXT_CAP = 520, OUT_CAP = 520 + CLI_OVERHEAD, VIDEO_CAP = CLI_TCP_MTU;
const size_t AUDIO_CAP = 1400 + CLI_OVERHEAD;  // one record: header, times, an AAC-ELD frame
const size_t BUF_SIZE = TEXT_CAP * 3 + OUT_CAP + VIDEO_CAP + AUDIO_CAP;

extern "C" void cli_random(uint8_t *p, size_t n) { esp_fill_random(p, n); }

static void saveClients() {
  prefs.begin("cli", false);
  prefs.putBytes("c", clients, sizeof clients);
  prefs.putUChar("n", clientCount);
  prefs.putUChar("next", nextClient);
  prefs.end();
}

// The radio under a slot: ble_link's peer slots, or the Wi-Fi link.
static uint16_t mtu(int slot) { return slot == CLI_TCP_SLOT ? CLI_TCP_MTU + 3 : ble_cli_mtu(slot); }
static bool linkUp(int slot) { return slot == CLI_TCP_SLOT ? cli_tcp_up() : ble_cli_subscribed(slot); }
static bool notify(int slot, const uint8_t *p, size_t n, bool video, uint32_t timeout_ms) {
  return slot == CLI_TCP_SLOT ? cli_tcp_send(p, n, timeout_ms) : ble_cli_notify(slot, p, n, video, timeout_ms);
}
static void drop(int slot) {
  if (slot == CLI_TCP_SLOT) cli_tcp_drop();
  else ble_cli_drop(slot);
}

void cli_begin() {
  clientsLock = xSemaphoreCreateMutex();
  uint8_t *p = (uint8_t *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!p) p = (uint8_t *)malloc(BUF_SIZE);
  if (!p) return;
  rxText[0] = p;
  rxText[1] = p + TEXT_CAP;
  ctrlBuf = p + TEXT_CAP * 2;
  ctrlOut = p + TEXT_CAP * 3;
  videoOut = p + TEXT_CAP * 3 + OUT_CAP;
  audioOut = videoOut + VIDEO_CAP;
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
  for (int i = 0; i < SLOTS; i++)
    if (sessions[i].welcomed) drop(i);
}

static int findClient(const uint8_t *id) {
  for (int i = 0; i < clientCount; i++)
    if (!memcmp(clients[i].id, id, CLI_ID_LEN)) return i;
  return -1;
}

static void plain(int slot, uint8_t type, const uint8_t *p, size_t n) {
  uint8_t out[128];
  if (n > sizeof out - 1) n = sizeof out - 1;
  out[0] = type;
  memcpy(out + 1, p, n);
  notify(slot, out, n + 1, false, 1000);
}

static void error(int slot, const char *why) { plain(slot, CLI_ERROR, (const uint8_t *)why, strlen(why)); }

// ---- Pairing (docs/cli-protocol.md, "Pairing"): over Wi-Fi only, one request at a time, and the key is stored
// only after a short press of BOOT. Everything below runs on cli_tcp's task: frames, cli_pair_poll(), cli_reset().
//   PAIR (c_pub, id, commit, name) -> PAIR_KEY (b_pub) -> PAIR_NONCE (n_c) -> PAIR_WAIT, code on the display,
//   LED blinking -> BOOT -> PAIRED, key in NVS. No press in 30 s, a dropped link, a bad nonce: ERROR, nothing kept.
struct PairPending {
  uint8_t id[CLI_ID_LEN], key[CLI_KEY_LEN], pub[32], commit[PAIR_COMMIT_LEN];
  char name[CF_TEXT];  // the computer's name, as the display shows it
};
static pair_gate gate;
static PairPending pend;

static void pairEnd(bool paired) {
  confirm_clear(CONFIRM_PAIR);  // ours alone: nothing else asks for this kind
  memset(&pend, 0, sizeof pend);
  pg_end(&gate, paired, millis());
}

// A paired computer is driving the phone, or was a moment ago: no request may open now, so the BOOT button stays
// the stop button for as long as there is something to stop.
static bool inUse() {
  uint32_t last = lastFrameMs;
  return cli_slot() >= 0 || (last && millis() - last < PAIR_QUIET_MS);
}

static void pair(int slot, const uint8_t *p, size_t n) {
  pair_req r;
  if (!pair_parse(p, n, &r)) return error(slot, "bad pair request");
  if (slot != CLI_TCP_SLOT) return error(slot, "pair over Wi-Fi (TCP 7202): it needs the BOOT button");
  if (!r.commit) return error(slot, "this CLI is too old to pair: update esp32agent");
  uint32_t wait = 0;
  char why[80];
  switch (pg_begin(&gate, slot, inUse(), millis(), &wait)) {
    case PG_OPEN: return error(slot, "busy: another pairing request is open");
    case PG_IN_USE: return error(slot, "busy: board in use (a paired computer's session; end it, wait 10 s)");
    case PG_LOCKED:
      snprintf(why, sizeof why, "busy: pairing locked for %lu s (a request was not confirmed)",
               (unsigned long)((wait + 999) / 1000));
      return error(slot, why);
  }
  uint8_t priv[32];
  esp_fill_random(priv, sizeof priv);
  bool ok = cli_pair(priv, r.c_pub, pend.pub, pend.key);
  memset(priv, 0, sizeof priv);
  if (!ok) {
    pairEnd(false);
    return error(slot, "bad key");
  }
  memcpy(pend.id, r.client_id, CLI_ID_LEN);
  memcpy(pend.commit, r.commit, PAIR_COMMIT_LEN);
  pair_show_name(r.name, pend.name, sizeof pend.name);
  board_log("cli: pairing asked by \"%s\"", pend.name);
  plain(slot, CLI_PAIR_KEY, pend.pub, 32);
}

static void pairNonce(int slot, const uint8_t *p, size_t n) {
  if (!pg_is(&gate, slot, PG_KEYED)) return error(slot, "no pairing request open");
  uint8_t h[PAIR_COMMIT_LEN] = {0};
  if (n == PAIR_NONCE_LEN) cli_pair_commit(p, h);
  if (n != PAIR_NONCE_LEN || memcmp(h, pend.commit, sizeof h) || !pg_wait(&gate, slot, millis())) {
    pairEnd(false);
    return error(slot, "bad pairing nonce");
  }
  char code[7], line[CF_TEXT];
  cli_pair_code(pend.key, p, code);
  snprintf(line, sizeof line, "Code %s", code);
  if (!confirm_request(CONFIRM_PAIR, pend.name, line, PAIR_CONFIRM_MS)) {  // the Wi-Fi setup page is asking
    pairEnd(false);
    return error(slot, "busy: the board waits for another confirmation (Wi-Fi setup)");
  }
  DBG("pair: waiting for BOOT, \"%s\", code %s", pend.name, code);
  uint8_t secs = PAIR_CONFIRM_MS / 1000;
  plain(slot, CLI_PAIR_WAIT, &secs, 1);
}

void cli_pair_poll() {
  if (gate.state == PG_IDLE) return;
  int slot = gate.owner;
  if (pg_keyed_late(&gate, millis())) {
    pairEnd(false);
    return error(slot, "pairing timed out: no nonce");
  }
  if (cli_slot() >= 0) {  // a paired computer's session came up meanwhile (on another link): BOOT is its stop button
    pairEnd(false);
    board_log("cli: pairing request dropped (a session started)");
    return error(slot, "busy: board in use (a paired computer's session; end it, wait 10 s)");
  }
  if (gate.state != PG_WAITING) return;
  int st = confirm_state(CONFIRM_PAIR);
  if (st == CF_WAITING) return;
  if (st != CF_ACCEPTED) {  // nothing was stored
    pairEnd(false);
    board_log("cli: pairing not confirmed");
    return error(slot, "pairing not confirmed: BOOT was not pressed within 30 s");
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
  plain(slot, CLI_PAIRED, out, sizeof out);
}

bool cli_pair_open() { return gate.state != PG_IDLE; }
int cli_paired() { return clientCount; }
void cli_board_id(uint8_t out[6]) { memcpy(out, boardId, 6); }

static void hello(int slot, const uint8_t *p, size_t n) {
  if (n != CLI_ID_LEN + CLI_NONCE_LEN) return error(slot, "bad hello");
  uint8_t key[CLI_KEY_LEN];
  xSemaphoreTake(clientsLock, portMAX_DELAY);
  int c = findClient(p);
  if (c >= 0) memcpy(key, clients[c].key, sizeof key);
  xSemaphoreGive(clientsLock);
  if (c < 0) return error(slot, "not paired");
  Session &s = sessions[slot];
  uint8_t bn[CLI_NONCE_LEN], out[CLI_NONCE_LEN + 6 + CLI_MAC_LEN];
  esp_fill_random(bn, sizeof bn);
  s.authed = false;
  bool ok = cli_session(key, p + CLI_ID_LEN, bn, s.c2b, s.b2c);
  if (ok) cli_mac(key, "hello", p + CLI_ID_LEN, CLI_NONCE_LEN, bn, CLI_NONCE_LEN, out + CLI_NONCE_LEN + 6);
  memset(key, 0, sizeof key);
  if (!ok) return error(slot, "session failed");
  s.client = c;
  s.rx = s.txCtrl = s.txVideo = s.txAudio = 0;
  s.welcomed = true;
  memcpy(out, bn, CLI_NONCE_LEN);
  memcpy(out + CLI_NONCE_LEN, boardId, 6);
  plain(slot, CLI_WELCOME, out, sizeof out);
}

static void data(int slot, const uint8_t *p, size_t n) {
  Session &s = sessions[slot];
  uint8_t *text = rxText[slot == CLI_TCP_SLOT];
  uint32_t ctr;
  if (!s.welcomed || !text || n > TEXT_CAP + CLI_OVERHEAD) return;
  int len = cli_open(s.c2b, 'C', p, n, &ctr, text);
  if (len < 0) return;  // forged or for another session: ignore
  if (ctr != s.rx + 1) { drop(slot); return; }  // a replay, or a lost frame: start over
  s.rx = ctr;
  lastFrameMs = millis() | 1;
  if (!s.authed) {
    // One computer at a time. The same one may move its session between BLE and Wi-Fi: the new link takes over
    // and the old one stays up, idle, as its way back.
    int old = cli_slot();
    if (old >= 0 && sessions[old].client != s.client) { drop(slot); return; }
    if (old >= 0) sessions[old].authed = false;
    s.authed = true;
    audioOn = false;  // asked again by every session
    gen++;
    board_log(slot == CLI_TCP_SLOT ? "cli: session up (wifi)" : "cli: session up");
  }
  if (len > 1 && text[0] == 'c') ble_cli_push_cmd((const char *)text + 1, len - 1);
}

void cli_on_frame(int slot, const uint8_t *p, size_t n) {
  if (slot < 0 || slot >= SLOTS || !n) return;
  switch (p[0]) {
    case CLI_PAIR: return pair(slot, p + 1, n - 1);
    case CLI_PAIR_NONCE: return pairNonce(slot, p + 1, n - 1);
    case CLI_HELLO: return hello(slot, p + 1, n - 1);
    case CLI_DATA: return data(slot, p, n);
  }
}

void cli_reset(int slot) {
  if (slot < 0 || slot >= SLOTS) return;
  if (gate.state != PG_IDLE && gate.owner == slot) {  // the link that asked to pair is gone: the request with it
    board_log("cli: pairing request withdrawn (link down)");
    pairEnd(false);
  }
  sessions[slot].authed = sessions[slot].welcomed = false;
}

bool cli_authed(int slot) { return slot >= 0 && slot < SLOTS && sessions[slot].authed; }
bool cli_known(int slot) { return slot >= 0 && slot < SLOTS && sessions[slot].welcomed; }
uint32_t cli_gen() { return gen; }
bool cli_wifi() { return cli_slot() == CLI_TCP_SLOT; }
uint16_t cli_mtu() { int slot = cli_slot(); return slot < 0 ? 23 : mtu(slot); }

void cli_drop() {
  int slot = cli_slot();
  if (slot >= 0) drop(slot);
  for (int i = 0; i < SLOTS; i++)  // and its idle way back
    if (i != slot && sessions[i].welcomed) drop(i);
}

int cli_slot() {
  for (int i = 0; i < SLOTS; i++)
    if (sessions[i].authed) return i;
  return -1;
}

bool cli_send_ctrl(char kind, const char *text, size_t n, uint32_t timeout_ms) {
  int slot = cli_slot();
  if (slot < 0) return false;
  Session &s = sessions[slot];
  uint8_t *buf = ctrlBuf, *out = ctrlOut;
  size_t room = mtu(slot) - 3 - CLI_OVERHEAD - 1;
  if (room > TEXT_CAP - 1) room = TEXT_CAP - 1;
  if (n > room) n = room;
  buf[0] = (uint8_t)kind;
  memcpy(buf + 1, text, n);
  size_t len = cli_seal(s.b2c, CLI_DATA, 'B', s.txCtrl + 1, buf, n + 1, out);
  if (!len || !notify(slot, out, len, false, timeout_ms)) {
    drop(slot);  // the CLI would see a counter gap anyway
    return false;
  }
  s.txCtrl++;
  return true;
}

size_t cli_video_room() {
  int slot = cli_slot();
  if (slot < 0 || !linkUp(slot)) return 0;
  return mtu(slot) - 3 - CLI_OVERHEAD - 3;  // ATT header, the seal, the chunk's own header (VCHUNK_HDR)
}

bool cli_send_video(const uint8_t *chunk, size_t n) {
  int slot = cli_slot();
  if (slot < 0) return false;
  Session &s = sessions[slot];
  uint8_t *out = videoOut;
  if (n > VIDEO_CAP - CLI_OVERHEAD) return false;
  size_t len = cli_seal(s.b2c, CLI_VIDEO, 'V', s.txVideo + 1, chunk, n, out);
  s.txVideo++;  // a chunk that fails is a gap the CLI handles like a seq gap
  return len && notify(slot, out, len, true, VIDEO_TIMEOUT_MS);
}

void cli_audio(bool on) { audioOn = on; }
bool cli_audio_wanted() { return audioOn && cli_slot() == CLI_TCP_SLOT; }

bool cli_send_audio(const uint8_t *rec, size_t n) {
  int slot = cli_slot();
  if (slot != CLI_TCP_SLOT || !audioOut || n + CLI_OVERHEAD > AUDIO_CAP) return false;
  Session &s = sessions[slot];
  size_t len = cli_seal(s.b2c, CLI_AUDIO, 'A', s.txAudio + 1, rec, n, audioOut);
  s.txAudio++;  // a record that fails is a seq gap for the CLI
  return len && notify(slot, audioOut, len, true, VIDEO_TIMEOUT_MS);
}
