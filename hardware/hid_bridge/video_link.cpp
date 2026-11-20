// Mirrored H.264 from AirPlay to the computer, see video_link.h.
#include "video_link.h"
#include <esp_heap_caps.h>
#include "link.h"
#include "cli_link.h"
#include "src/video/gop.h"
#include "src/airplay/airplay.h"
#include "logbuf.h"

// The chain since the latest IDR, kept as a ring (src/video/gop.h). iOS sends an IDR at the start of mirroring and
// when the picture's format changes (a codec packet: the phone turned in an app that turns along); iOS 26 maybe
// never else, so one chain may run for the whole session. While it is whole, a stream (a page that joins, the
// computer's) starts from its IDR; once its start is dropped, the streams on it go on (minutes of lag fit), a new one
// waits for the next IDR (status fz). One ring of 4.5 MB: what the GOP (4 MB) and the tail ring behind a full GOP
// (512 KB) took before; that GOP stayed frozen and useless once the tail lost its first AU, and the tail held only
// ~20 s at 8 fps (2026-11-16: "GOP full (1225 AUs, 4073 KB)" after 150 s, then the page stayed black). About 1.6 MB
// PSRAM stays free while streaming (TLS record buffers, the mirror's packet copies).
#ifndef GOP_CAP_KB
#define GOP_CAP_KB 4608  // flash.sh --small-gop: 256, to see a chain lose its start within minutes
#endif
const size_t GOP_CAP = GOP_CAP_KB * 1024;
const int MAX_AUS = 12288;              // 16 bytes each; ~13 min at 15 fps of tiny P-frames (a still screen)
const size_t SCRATCH_CAP = 512 * 1024;  // largest access unit we forward

struct Req {
  char id[16];
  uint32_t kb;
  bool bench;
};

// A receiver of the video: the computer's session (cli_link.cpp), or the web interface's page (web_server.cpp). Each
// keeps its place in the chain and its chunk numbering, and runs on its own task with its own buffers: a slow one
// never holds up the AirPlay receiver or the other.
struct Sink {
  size_t (*room)();                          // chunk payload it takes now; 0: nobody to send to
  bool (*send)(const uint8_t *p, size_t n);  // one chunk
  uint32_t (*sessGen)();                     // bumps with every new receiver (session, page): it starts over
  const char *name;                          // for the log
  // Whole access units instead of chunks (the microSD recording, sdlog.h): p, n, VF_CONFIG / VF_IDR, the timestamp.
  // room() then only says whether it takes any (1) or not (0).
  bool (*au)(const uint8_t *p, size_t n, uint8_t flags, uint32_t ts);
  uint8_t *chunk;                            // VCHUNK_HDR + its largest payload
  uint8_t *scratch;  // the AU being sent
  uint8_t cfg[GOP_CFG_MAX];
  QueueHandle_t reqs;
  TaskHandle_t task;
  volatile bool streaming;  // the computer asked for `stream on`
  uint16_t seq;             // chunk numbers
  // The chain it sends in order, AU after AU (UINT32_MAX: none; it starts at the next whole one). It never skips
  // ahead: a P-frame needs the one before, and the next IDR may never come.
  volatile uint32_t txGen = UINT32_MAX;
  uint32_t next;                  // the number of the AU it sends next
  uint32_t badGen = UINT32_MAX;   // a chain with an AU too large to send: not again
  uint32_t cliGen;                // the receiver it streams to (sessGen()): a new one starts over
  // `stream on at`: the receiver's decoder kept AUs 0 .. resumeN-1 of chain resumeGen across a reconnect; the stream
  // goes on there instead of resending the chain from its IDR (minutes over a weak link, or impossible once the
  // chain lost its start)
  volatile bool resume;
  uint32_t resumeGen, resumeN;
};

static gop_buf gop;
// the session's, the web's (webSink.task stays NULL until a page first asks), the microSD card's (sdSink.task stays
// NULL until a recording of the screen starts)
static Sink sink, webSink, sdSink;
static Sink *const sinks[] = {&sink, &webSink, &sdSink};
static SemaphoreHandle_t lock;
static uint8_t cliChunk[VCHUNK_HDR + VIDEO_PAYLOAD];
static uint32_t winStart, winBytes, winFrames, lastIdr;
static volatile uint32_t lastAu;
static volatile uint32_t statKbps, statIdrMs, statAt;
static volatile float statFps;
static wv_watch webWatch;  // video_web_state(): the web task only

// The session's chunk room (0: no session to send to).
static size_t cliRoom() { return min(cli_video_room(), VIDEO_PAYLOAD); }

// One access unit as chunks. A failed chunk still consumes its seq, so the receiver sees a gap.
static bool sendAu(Sink &s, const uint8_t *p, size_t n, uint8_t flags, uint32_t ts) {
  if (s.au) return s.au(p, n, flags, ts);
  size_t room = s.room();
  if (room < 5) return false;
  vchunk c;
  vchunk_begin(&c, p, n, flags, ts);
  uint16_t &seq = s.seq;
  for (size_t len; (len = vchunk_next(&c, s.chunk, room, seq)) > 0;) {
    bool ok = s.send(s.chunk, len);
    seq++;
    if (!ok) return false;
  }
  return true;
}

enum { CP_GONE = -1, CP_BIG = -2 };
// Copies AU k of chain gen into the sink's scratch: 1, or 0 if it has not come yet, CP_GONE if it is gone (a new
// chain, or dropped: the stream fell a whole buffer behind), CP_BIG if it is too large to send. more: newer AUs wait
// behind it (VF_BEHIND: the receiver is still catching up).
static int copyAu(Sink &s, uint32_t gen, uint32_t k, gop_au *au, bool *more) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const gop_au *a = gop.gen == gen ? gop_at(&gop, k) : NULL;
  int r = gop.gen != gen || k < gop.first ? CP_GONE : !a ? 0 : a->len > SCRATCH_CAP ? CP_BIG : 1;
  if (r == 1) {
    *au = *a;
    memcpy(s.scratch, gop.arena + a->off, a->len);
    *more = k + 1 < gop_end(&gop);
  }
  xSemaphoreGive(lock);
  return r;
}

// The chain's state for a snap: 1 whole (a decoder can start from it), 0 none yet, -1 it lost its start.
static int chainState() {
  xSemaphoreTake(lock, portMAX_DELAY);
  int st = gop_whole(&gop) ? 1 : !gop.wait_idr && gop.n && gop.first ? -1 : 0;
  xSemaphoreGive(lock);
  return st;
}

static uint32_t chainGen() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t g = gop.gen;
  xSemaphoreGive(lock);
  return g;
}

// True while there is no chain, waiting for the next IDR (boot, or an AU too big for the buffer).
// Only meaningful while AirPlay is streaming: wait_idr starts true at boot before any mirror (#25).
static bool waitingIdrNow() {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool w = gop.wait_idr;
  xSemaphoreGive(lock);
  return w;
}

// Why the session's stream last stopped following its chain, and when (vdbg): r a new session, n not streaming /
// no room, c/g a failed config / AU send, b it fell a whole buffer behind, z an AU too large to send.
static char lastWhy = '-';
static uint32_t lastWhyAt, behindBreaks;

// The sink leaves its chain; it starts again from the next whole one.
static void unfollow(Sink &s, char why) {
  if (&s == &sink && s.txGen != UINT32_MAX) lastWhy = why, lastWhyAt = millis();  // vdbg is the session's
  s.txGen = UINT32_MAX;
}

// A whole chain to start a stream or a snap from: its generation, its end, its SPS/PPS. False: none.
static bool startable(uint32_t *gen, uint32_t *end, uint8_t *cfg, size_t *cfg_len, uint32_t *cfg_ts) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = gop_whole(&gop) && gop.cfg_len > 0;
  *gen = gop.gen;
  *end = gop_end(&gop);
  *cfg_len = ok ? gop.cfg_len : 0;
  *cfg_ts = gop.cfg_ts;
  if (ok) memcpy(cfg, gop.cfg, gop.cfg_len);
  xSemaphoreGive(lock);
  return ok;
}

static String doSnap(Sink &s) {
  uint8_t *cfg = s.cfg;
  if (!s.room()) return "err video not subscribed";
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t gen, cfg_ts, n;
    size_t cfg_len;
    if (!startable(&gen, &n, cfg, &cfg_len, &cfg_ts)) {
      if (chainState() < 0) return "err video frozen (restart screen mirroring)";
      // wait_idr is true from boot; only surface the keyframe wait once mirroring is actually up.
      if (waitingIdrNow() && airplay_state() == AIRPLAY_STREAMING) return "err waiting for keyframe";
      return "err no video (start screen mirroring)";
    }
    if (!sendAu(s, cfg, cfg_len, VF_CONFIG | VF_SNAP, cfg_ts)) return "err video send failed";
    bool complete = true;
    for (uint32_t k = 0; k < n && complete; k++) {
      gop_au au;
      bool more;
      complete = copyAu(s, gen, k, &au, &more) == 1;
      uint8_t flags = VF_SNAP | (k == 0 ? VF_IDR : 0) | (k + 1 < n ? VF_BEHIND : 0);
      if (complete && !sendAu(s, s.scratch, au.len, flags, au.ts)) return "err video send failed";
    }
    if (complete) return "ok";  // else a new IDR replaced the chain mid-send, or the ring overtook it: once more
  }
  return "err video changing too fast";
}

static String doBench(Sink &s, uint32_t kb) {
  size_t room = s.room();
  if (!room) return "err video not subscribed";
  uint32_t start = millis();
  uint16_t &seq = s.seq;
  uint8_t *chunk = s.chunk;
  memset(chunk + VCHUNK_HDR, 0xA5, room);
  for (size_t left = kb * 1024; left;) {
    size_t n = min(left, room);
    chunk[0] = (uint8_t)seq;
    chunk[1] = (uint8_t)(seq >> 8);
    chunk[2] = VF_BENCH;
    bool ok = s.send(chunk, VCHUNK_HDR + n);
    seq++;
    if (!ok) return "err video send failed";
    left -= n;
  }
  return "ok " + String(millis() - start);
}

// Continuous mode: send every AU once and in order, from the chain's IDR when the stream starts (P-frames can't be
// skipped: each needs the one before, and the next IDR may be minutes away or never come). However far the
// receiver is behind, it keeps going; it only stops when an AU it needs was dropped (it fell a whole buffer behind)
// or a send failed (the receiver saw a gap), and then starts again from the next whole chain.
static void streamStep(Sink &s) {
  if (s.cliGen != s.sessGen()) {  // a new session or page: config and the whole chain (or `stream on at`)
    s.cliGen = s.sessGen();
    unfollow(s, 'r');
  }
  if (!s.streaming || !s.room()) {
    unfollow(s, 'n');  // starts over when streaming resumes
    return;
  }
  bool sendCfg = false;
  size_t cfg_len = 0;
  uint32_t cfg_ts = 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t gen = gop.gen, stop = gop_end(&gop);
  if (s.resume) {  // the receiver's decoder has AUs 0 .. resumeN-1 of this chain: no config, no IDR again
    s.resume = false;
    if (gen == s.resumeGen && gop_resume_at(&gop, gop.idr_ts, s.resumeN) >= 0) {
      s.txGen = gen;
      s.next = s.resumeN;
    }
  }
  if (s.txGen != gen && gen != s.badGen && gop_whole(&gop) && gop.cfg_len) {
    s.txGen = gen;
    s.next = 0;
    sendCfg = true;
    cfg_len = gop.cfg_len;
    cfg_ts = gop.cfg_ts;
    memcpy(s.cfg, gop.cfg, cfg_len);
  }
  bool on = s.txGen == gen;
  xSemaphoreGive(lock);
  if (!on) return;  // nothing to start from: the next IDR
  if (sendCfg && !sendAu(s, s.cfg, cfg_len, VF_CONFIG, cfg_ts)) {
    unfollow(s, 'c');
    return;
  }
  // up to the end as it was: a request (snap, bench) waits at most this long
  for (gop_au au; s.next < stop; s.next++) {
    bool more;
    int r = copyAu(s, gen, s.next, &au, &more);
    if (r == 0) return;
    if (r == CP_BIG) {
      s.badGen = gen;
      unfollow(s, 'z');
      board_log("video: an access unit over %u KB; the %s stream waits for the next keyframe", (unsigned)(SCRATCH_CAP / 1024), s.name);
      return;
    }
    if (r == CP_GONE) {
      bool behind = chainGen() == gen;
      unfollow(s, behind ? 'b' : 'r');
      if (behind) {
        if (&s == &sink) behindBreaks++;
        board_log("video: the %s stream fell a whole buffer (%u KB) behind; it waits for the next keyframe", s.name,
                  (unsigned)(GOP_CAP / 1024));
      }
      return;
    }
    if (!sendAu(s, s.scratch, au.len, (s.next == 0 ? VF_IDR : 0) | (more ? VF_BEHIND : 0), au.ts)) {
      unfollow(s, 'g');
      return;
    }
  }
}

static void videoTask(void *arg) {
  Sink &s = *(Sink *)arg;
  for (;;) {
    Req r;
    if (s.reqs && xQueueReceive(s.reqs, &r, 0) == pdTRUE) {
      String reply = String(r.id) + " " + (r.bench ? doBench(s, r.kb) : doSnap(s));
      board_log("video: %s", reply.c_str());  // the reply only goes to the session; mirror it in the log
      link_reply(reply);
    }
    streamStep(s);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
}

bool video_begin() {
  uint8_t *arena = (uint8_t *)heap_caps_malloc(GOP_CAP, MALLOC_CAP_SPIRAM);
  gop_au *aus = (gop_au *)heap_caps_malloc(MAX_AUS * sizeof(gop_au), MALLOC_CAP_SPIRAM);
  sink.scratch = (uint8_t *)heap_caps_malloc(SCRATCH_CAP, MALLOC_CAP_SPIRAM);
  sink.reqs = xQueueCreate(4, sizeof(Req));
  sink.room = cliRoom;
  sink.send = cli_send_video;
  sink.sessGen = cli_gen;
  sink.name = "computer's";
  sink.chunk = cliChunk;
  if (!arena || !aus || !sink.scratch || !sink.reqs) return false;
  gop_init(&gop, arena, GOP_CAP, aus, MAX_AUS);
  lock = xSemaphoreCreateMutex();
  if (xTaskCreatePinnedToCore(videoTask, "video", 6 * 1024, &sink, 2, &sink.task, 1) != pdPASS) return false;
  return true;
}

void video_on_config(const uint8_t *sps_pps, size_t len) {
  xSemaphoreTake(lock, portMAX_DELAY);
  gop_set_config(&gop, sps_pps, len, millis());
  xSemaphoreGive(lock);
}

void video_on_au(const uint8_t *au, size_t len, bool idr) {
  uint32_t now = millis();
  lastAu = now;
  winBytes += len;
  winFrames++;
  if (idr) {
    if (lastIdr) statIdrMs = now - lastIdr;
    lastIdr = now;
  }
  if (now - winStart >= 1000) {
    statKbps = winBytes * 8 / (now - winStart);
    statFps = winFrames * 1000.0f / (now - winStart);
    statAt = now;
    winBytes = winFrames = 0;
    winStart = now;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t ovf = gop.overflows;
  bool waited = gop.wait_idr;
  gop_push(&gop, au, len, idr, now);
  bool full = gop.overflows != ovf && !gop.wait_idr, lostIt = gop.wait_idr && !waited;
  uint32_t aus = gop_end(&gop), kb = gop.total / 1024;
  xSemaphoreGive(lock);
  if (full)
    board_log("video: buffer full (%lu AUs, %lu KB since the keyframe); streams on it go on, a new one waits for the "
              "next keyframe",
              (unsigned long)aus, (unsigned long)kb);
  if (lostIt) board_log("video: an access unit of %u KB does not fit; waiting for the next keyframe", (unsigned)(len / 1024));
  for (Sink *k : sinks)
    if (k->task) xTaskNotifyGive(k->task);
}

static volatile uint32_t lostPackets;
void video_on_lost() { lostPackets++; }

void video_stream(bool on) {
  sink.resume = false;
  sink.streaming = on;
}

static bool streamAt(Sink &s, uint32_t idrTs, uint32_t n) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int at = gop_resume_at(&gop, idrTs, n);
  if (at >= 0) {
    s.resumeGen = gop.gen;
    s.resumeN = (uint32_t)at;
  }
  s.resume = at >= 0;  // before streaming: the stream must not start from the IDR first
  xSemaphoreGive(lock);
  s.streaming = true;
  if (s.task) xTaskNotifyGive(s.task);
  return at >= 0;
}

bool video_stream_at(uint32_t idrTs, uint32_t n) { return streamAt(sink, idrTs, n); }

// Bytes the stream has still to send from the board's buffer (status vbl); must hold lock.
static uint32_t backlogLocked() {
  const Sink &s = sink;
  if (!s.streaming || !cli_up() || gop.wait_idr) return 0;
  if (s.txGen != gop.gen) return gop_whole(&gop) ? gop.used : 0;  // (re)starting: the whole chain
  return gop_bytes_from(&gop, s.next);
}

static void request(const String &id, bool bench, uint32_t kb) {
  Req r = {{0}, kb, bench};
  strncpy(r.id, id.c_str(), sizeof r.id - 1);
  if (!sink.reqs || !sink.task || xQueueSend(sink.reqs, &r, 0) != pdTRUE) link_reply(id + " err busy");
}

void video_snap(const String &id) { request(id, false, 0); }
void video_bench(const String &id, uint32_t kb) { request(id, true, kb); }

VideoStats video_stats() {
  bool fresh = millis() - statAt < 3000;  // mirroring stopped: report zeros, not stale numbers
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t gopBytes = gop.used, overflows = gop.overflows;
  bool frozen = !gop.wait_idr && gop.n && gop.first && sink.txGen != gop.gen;
  uint32_t backlog = backlogLocked();
  xSemaphoreGive(lock);
  uint32_t age = lastAu ? millis() - lastAu : 0;
  return {fresh ? statKbps : 0, statIdrMs, gopBytes, age, fresh ? statFps : 0, overflows, frozen, lostPackets, backlog};
}

String video_debug() {
  char b[112];
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(b, sizeof b, "wh%d fi%lu n%d wi%d fg%d nx%lu/%lu uf%c %lus fb%lu web fg%d nx%lu", gop_whole(&gop),
           (unsigned long)gop.first, gop.n, gop.wait_idr, sink.txGen == gop.gen, (unsigned long)sink.next,
           (unsigned long)gop_end(&gop), lastWhy, (unsigned long)(lastWhyAt ? (millis() - lastWhyAt) / 1000 : 0),
           (unsigned long)behindBreaks, webSink.txGen == gop.gen, (unsigned long)webSink.next);
  xSemaphoreGive(lock);
  return b;
}

// ---- The web interface's stream ----------------------------------------------------------------------------------
bool video_web_begin(size_t (*room)(), bool (*send)(const uint8_t *p, size_t n), uint32_t (*gen)(), size_t payload) {
  if (webSink.task) return true;
  if (!lock) return false;  // video_begin() failed: no PSRAM
  if (!webSink.scratch) webSink.scratch = (uint8_t *)heap_caps_malloc(SCRATCH_CAP, MALLOC_CAP_SPIRAM);
  if (!webSink.chunk) webSink.chunk = (uint8_t *)heap_caps_malloc(VCHUNK_HDR + payload, MALLOC_CAP_SPIRAM);
  if (!webSink.scratch || !webSink.chunk) return false;
  webSink.room = room;
  webSink.send = send;
  webSink.sessGen = gen;
  webSink.name = "web";
  webSink.cliGen = gen();
  wv_init(&webWatch);
  // Below the session's video task (2): the computer's stream comes first. Core 1, next to it.
  return xTaskCreatePinnedToCore(videoTask, "video_web", 4096, &webSink, 1, &webSink.task, 1) == pdPASS;
}

void video_web_stream(bool on) {
  webSink.resume = false;
  webSink.streaming = on;
  if (webSink.task) xTaskNotifyGive(webSink.task);
}

bool video_web_stream_at(uint32_t idrTs, uint32_t n) { return streamAt(webSink, idrTs, n); }

int video_web_state(uint32_t *waitedMs) {
  *waitedMs = 0;
  if (!lock) return VW_KEYFRAME;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool following = webSink.txGen == gop.gen, waitIdr = gop.wait_idr, whole = gop_whole(&gop);
  xSemaphoreGive(lock);
  uint32_t now = millis();
  int st = wv_update(&webWatch, following, waitIdr, whole, now);
  *waitedMs = wv_waited(&webWatch, now);
  return st;
}

// ---- The microSD card's recording -------------------------------------------------------------------------------
bool video_sd_begin(size_t (*room)(), bool (*au)(const uint8_t *, size_t, uint8_t, uint32_t), uint32_t (*gen)()) {
  if (sdSink.task) return true;
  if (!lock) return false;
  if (!sdSink.scratch) sdSink.scratch = (uint8_t *)heap_caps_malloc(SCRATCH_CAP, MALLOC_CAP_SPIRAM);
  if (!sdSink.scratch) return false;
  sdSink.room = room;
  sdSink.au = au;
  sdSink.sessGen = gen;
  sdSink.name = "SD card's";
  sdSink.cliGen = gen();
  sdSink.streaming = true;  // room() says whether a file is open
  // Below the computer's stream (2), next to the web's; the card's writes block this task only. More stack: FAT.
  return xTaskCreatePinnedToCore(videoTask, "video_sd", 6 * 1024, &sdSink, 1, &sdSink.task, 1) == pdPASS;
}

void video_sd_kick() {
  if (sdSink.task) xTaskNotifyGive(sdSink.task);
}
