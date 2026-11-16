// Mirrored H.264 from AirPlay to the computer, see video_link.h.
#include "video_link.h"
#include <esp_heap_caps.h>
#include "link.h"
#include "cli_link.h"
#include "src/video/gop.h"
#include "src/airplay/airplay.h"
#include "logbuf.h"

// GOP since the latest IDR. A still screen gets an IDR only every few minutes (iOS 26: maybe once per mirroring
// session), so room for ~27 min of AUs at 5 fps (P-frames of a still screen are tiny). When it fills, it freezes
// (gop.h): a stream that is in step goes on from the tail below, everyone else needs the next IDR (status fz).
#ifndef GOP_CAP_KB
#define GOP_CAP_KB 4096  // flash.sh --small-gop: 256, to test a frozen GOP within minutes
#endif
const size_t GOP_CAP = GOP_CAP_KB * 1024;
const int MAX_AUS = 8192;
const size_t TAIL_CAP = 512 * 1024;     // the newest AUs after a frozen GOP (a ring), to go on from
const int TAIL_AUS = 1024;
const size_t SCRATCH_CAP = 512 * 1024;   // largest access unit we forward

struct Req {
  char id[16];
  uint32_t kb;
  bool bench;
};

// The receiver of the video: the computer's session (cli_link.cpp). It keeps its place in the GOP and its chunk
// numbering, and runs on its own task with its own buffers.
struct Sink {
  uint8_t *scratch;  // the AU being sent
  uint8_t cfg[GOP_CFG_MAX];
  QueueHandle_t reqs;
  TaskHandle_t task;
  volatile bool streaming;  // the computer asked for `stream on`
  uint16_t seq;             // chunk numbers
  uint32_t txGen = UINT32_MAX;
  int next, tailNext;
  bool catchingUp;
  volatile uint32_t followGen = UINT32_MAX;  // the generation it sends in order, AU after AU
  volatile bool onTail;                      // it goes on from the tail past a frozen GOP
  uint32_t cliGen;                           // the session it streams to (cli_gen()): a new one starts over
  // `stream on at`: the computer's decoder kept the first resumeN AUs of generation resumeGen across a reconnect;
  // the stream goes on there instead of resending the GOP from its IDR (minutes over a weak link)
  volatile bool resume;
  uint32_t resumeGen;
  int resumeN;
};

static gop_buf gop, tail;
static volatile bool tailLive;  // the tail holds every AU since the GOP froze (sent ones may be dropped)
static bool tailTrimmed;        // its oldest AUs were dropped (vdbg)
static Sink sink;
static SemaphoreHandle_t lock;
static uint8_t chunk[VCHUNK_HDR + VIDEO_PAYLOAD];
static uint32_t winStart, winBytes, winFrames, lastIdr;
static volatile uint32_t lastAu;
static volatile uint32_t statKbps, statIdrMs, statAt;
static volatile float statFps;

static void unfollow(Sink &s, char why);

// Chunk room (0: no session to send to) and send.
static size_t videoRoom() { return min(cli_video_room(), VIDEO_PAYLOAD); }
static bool videoSend(const uint8_t *p, size_t n) { return cli_send_video(p, n); }

// One access unit as chunks. A failed chunk still consumes its seq, so the receiver sees a gap.
static bool sendAu(Sink &s, const uint8_t *p, size_t n, uint8_t flags, uint32_t ts) {
  size_t room = videoRoom();
  if (room < 5) return false;
  vchunk c;
  vchunk_begin(&c, p, n, flags, ts);
  uint16_t &seq = s.seq;
  for (size_t len; (len = vchunk_next(&c, chunk, room, seq)) > 0;) {
    bool ok = videoSend(chunk, len);
    seq++;
    if (!ok) return false;
  }
  return true;
}

// Copies AU i of generation gen into the sink's scratch; false if the GOP moved on. more: newer AUs wait behind it
// (VF_BEHIND: the computer is still catching up).
static bool copyAu(Sink &s, uint32_t gen, int i, gop_au *au, bool *more) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = gop.gen == gen && i < gop.n && gop.aus[i].len <= SCRATCH_CAP;
  if (ok) {
    *au = gop.aus[i];
    memcpy(s.scratch, gop.arena + au->off, au->len);
    *more = i + 1 < gop.n || (gop.frozen && tailLive && s.onTail && s.tailNext < tail.n);
  }
  xSemaphoreGive(lock);
  return ok;
}

// Copies tail AU i into the sink's scratch; false if there is none (yet) or the tail broke off.
static bool copyTail(Sink &s, uint32_t gen, int i, gop_au *au, bool *more) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = gop.gen == gen && tailLive && s.onTail && i < tail.n && tail.aus[i].len <= SCRATCH_CAP;
  if (ok) {
    *au = tail.aus[i];
    memcpy(s.scratch, tail.arena + au->off, au->len);
    *more = i + 1 < tail.n;
  }
  xSemaphoreGive(lock);
  return ok;
}

static bool frozenNow() {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool f = gop.frozen;
  xSemaphoreGive(lock);
  return f;
}

// True while the GOP is empty waiting for the next IDR (boot, or an IDR too big for the buffer;
// an overflow with frames already buffered sets frozen instead). Distinct from frozen: mirroring may still be up, just no decodable snap yet (#25).
// Only meaningful while AirPlay is streaming — wait_idr starts true at boot before any mirror.
static bool waitingIdrNow() {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool w = gop.wait_idr && !gop.frozen;
  xSemaphoreGive(lock);
  return w;
}

// Why the sink last stopped following while it was in step, and when (vdbg): r a new session, n not
// streaming / no room, c/g/t a failed config / GOP / tail send, b fell a whole tail ring behind.
static char lastWhy = '-';
static uint32_t lastWhyAt, tailBreaks;

// The sink fell out of order: its frozen GOP can't go on, the next IDR has to come.
static void unfollow(Sink &s, char why) {
  if (s.followGen != UINT32_MAX) lastWhy = why, lastWhyAt = millis();
  s.followGen = UINT32_MAX;
  s.onTail = false;
}

// Snapshot of the GOP state: generation, AU count, SPS/PPS. A frozen GOP counts as none (it may end minutes
// ago), except for a stream that goes on from the tail.
static bool snapshot(uint32_t *gen, int *n, uint8_t *cfg, size_t *cfg_len, uint32_t *cfg_ts, const Sink *following) {
  xSemaphoreTake(lock, portMAX_DELAY);
  *gen = gop.gen;
  if (gop.frozen && !(following && tailLive && following->onTail && following->followGen == gop.gen)) {
    *n = 0;
    *cfg_len = 0;
    xSemaphoreGive(lock);
    return false;
  }
  *n = gop.wait_idr ? 0 : gop.n;
  *cfg_len = gop.cfg_len;
  *cfg_ts = gop.cfg_ts;
  memcpy(cfg, gop.cfg, gop.cfg_len);
  xSemaphoreGive(lock);
  return *n > 0 && *cfg_len > 0;
}

static String doSnap(Sink &s) {
  uint8_t *cfg = s.cfg;
  if (!videoRoom()) return "err video not subscribed";
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t gen, cfg_ts;
    size_t cfg_len;
    int n;
    if (!snapshot(&gen, &n, cfg, &cfg_len, &cfg_ts, NULL)) {
      if (frozenNow()) return "err video frozen (restart screen mirroring)";
      // wait_idr is true from boot; only surface the keyframe wait once mirroring is actually up.
      if (waitingIdrNow() && airplay_state() == AIRPLAY_STREAMING) return "err waiting for keyframe";
      return "err no video (start screen mirroring)";
    }
    if (!sendAu(s, cfg, cfg_len, VF_CONFIG | VF_SNAP, cfg_ts)) return "err video send failed";
    bool complete = true;
    for (int i = 0; i < n && complete; i++) {
      gop_au au;
      bool more;
      complete = copyAu(s, gen, i, &au, &more);
      uint8_t flags = VF_SNAP | (au.idr ? VF_IDR : 0) | (i + 1 < n ? VF_BEHIND : 0);
      if (complete && !sendAu(s, s.scratch, au.len, flags, au.ts)) return "err video send failed";
    }
    if (complete) return "ok";  // else a new IDR replaced the GOP mid-send: start over with it
  }
  return "err video changing too fast";
}

static String doBench(Sink &s, uint32_t kb) {
  size_t room = videoRoom();
  if (!room) return "err video not subscribed";
  uint32_t start = millis();
  uint16_t &seq = s.seq;
  memset(chunk + VCHUNK_HDR, 0xA5, room);
  for (size_t left = kb * 1024; left;) {
    size_t n = min(left, room);
    chunk[0] = (uint8_t)seq;
    chunk[1] = (uint8_t)(seq >> 8);
    chunk[2] = VF_BENCH;
    bool ok = videoSend(chunk, VCHUNK_HDR + n);
    seq++;
    if (!ok) return "err video send failed";
    left -= n;
  }
  return "ok " + String(millis() - start);
}

// Continuous mode: send every AU once, catching up from the GOP's IDR when streaming starts (P-frames
// can't be skipped: each needs the one before, and the next IDR may be minutes away). After a failed
// send, start over from the GOP's IDR. Past a frozen GOP, go on with the tail.
static void streamStep(Sink &s) {
  uint8_t *cfg = s.cfg;
  if (s.cliGen != cli_gen()) {  // a new session: resend config + whole GOP
    s.cliGen = cli_gen();
    s.txGen = UINT32_MAX;
  }
  if (s.txGen == UINT32_MAX) unfollow(s, 'r');
  if (!s.streaming || !videoRoom()) {
    s.txGen = UINT32_MAX;  // resend config + whole GOP when streaming resumes
    unfollow(s, 'n');
    return;
  }
  uint32_t gen, cfg_ts;
  size_t cfg_len;
  int n;
  if (!snapshot(&gen, &n, cfg, &cfg_len, &cfg_ts, &s)) return;
  if (s.resume) {  // the computer's decoder has the first resumeN AUs of this GOP: no config, no IDR again
    s.resume = false;
    if (gen == s.resumeGen && s.resumeN <= n) {
      s.txGen = s.followGen = gen;
      s.next = s.resumeN;
      s.tailNext = 0;
      s.catchingUp = true;
    }
  }
  if (gen != s.txGen) {
    s.txGen = gen;
    s.next = s.tailNext = 0;
    s.catchingUp = true;
    if (!sendAu(s, cfg, cfg_len, VF_CONFIG, cfg_ts)) { s.txGen = UINT32_MAX; unfollow(s, 'c'); return; }
    s.followGen = gen;
  }
  bool more;
  for (gop_au au; s.next < n && copyAu(s, gen, s.next, &au, &more); s.next++)
    if (!sendAu(s, s.scratch, au.len, (au.idr ? VF_IDR : 0) | (more ? VF_BEHIND : 0), au.ts)) { s.txGen = UINT32_MAX; unfollow(s, 'g'); return; }
  if (s.next < n) return;
  s.catchingUp = false;
  for (gop_au au; copyTail(s, gen, s.tailNext, &au, &more); s.tailNext++)
    if (!sendAu(s, s.scratch, au.len, more ? VF_BEHIND : 0, au.ts)) { s.txGen = UINT32_MAX; unfollow(s, 't'); return; }
}

static void videoTask(void *arg) {
  Sink &s = *(Sink *)arg;
  for (;;) {
    Req r;
    if (xQueueReceive(s.reqs, &r, 0) == pdTRUE) {
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
  uint8_t *tailArena = (uint8_t *)heap_caps_malloc(TAIL_CAP, MALLOC_CAP_SPIRAM);
  gop_au *tailAus = (gop_au *)heap_caps_malloc(TAIL_AUS * sizeof(gop_au), MALLOC_CAP_SPIRAM);
  sink.scratch = (uint8_t *)heap_caps_malloc(SCRATCH_CAP, MALLOC_CAP_SPIRAM);
  sink.reqs = xQueueCreate(4, sizeof(Req));
  if (!arena || !aus || !tailArena || !tailAus || !sink.scratch || !sink.reqs) return false;
  gop_init(&gop, arena, GOP_CAP, aus, MAX_AUS);
  gop_init(&tail, tailArena, TAIL_CAP, tailAus, TAIL_AUS);
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
  bool wasFrozen = gop.frozen, wasLive = tailLive;
  gop_push(&gop, au, len, idr, now);
  bool froze = gop.frozen && !wasFrozen;
  if (froze) {  // the tail takes over: a stream in step goes on from it
    gop_clear(&tail);
    sink.onTail = sink.followGen == gop.gen;
    sink.tailNext = 0;
    tailLive = true;
    tailTrimmed = false;
  }
  // A ring: full drops the oldest. It used to break once full unless the stream was on it at that moment, and a
  // stream off it for a second (a Wi-Fi hiccup) then found nothing to resume from: frozen until mirroring
  // restarted (2026-11-06, three times in a day).
  int dropped = gop.frozen && tailLive ? gop_append_ring(&tail, au, len, now) : 0;
  if (dropped < 0) tailLive = false;  // one AU larger than the whole tail
  if (dropped > 0) {
    tailTrimmed = true;
    if (sink.onTail && (sink.tailNext -= dropped) < 0) {  // it fell a whole ring behind: AUs it never sent are gone
      sink.tailNext = 0;
      sink.onTail = false;
      lastWhy = 'b', lastWhyAt = now;
    }
  }
  bool tailBroke = gop.frozen && !froze && wasLive && !tailLive;
  int aus = gop.n;
  uint32_t kb = gop.used / 1024;
  xSemaphoreGive(lock);
  if (froze) board_log("video: GOP full (%d AUs, %lu KB); the streams in step go on from the tail\n", aus,
                           (unsigned long)kb);
  if (tailBroke) tailBreaks++;
  if (tailBroke) board_log("video: a stream fell behind the tail; frozen until the next IDR");
  if (sink.task) xTaskNotifyGive(sink.task);
}

static volatile uint32_t lostPackets;
void video_on_lost() { lostPackets++; }

void video_stream(bool on) {
  sink.resume = false;
  sink.streaming = on;
}

bool video_stream_at(uint32_t idrTs, uint32_t n) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int at = gop_resume_at(&gop, idrTs, n);
  if (at >= 0) {
    sink.resumeGen = gop.gen;
    sink.resumeN = at;
  }
  sink.resume = at >= 0;
  xSemaphoreGive(lock);
  sink.streaming = true;
  return at >= 0;
}

// Bytes the stream has still to send from the board's buffers (status vbl); must hold lock.
static uint32_t backlogLocked() {
  const Sink &s = sink;
  if (!s.streaming || !cli_up() || gop.wait_idr) return 0;
  if (s.txGen != gop.gen) return gop.frozen ? 0 : gop.used;  // (re)starting: the whole GOP
  uint32_t b = gop_bytes_from(&gop, s.next);
  if (gop.frozen && tailLive && s.onTail) b += gop_bytes_from(&tail, s.tailNext);
  return b;
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
  bool frozen = gop.frozen && !(tailLive && sink.onTail);
  uint32_t backlog = backlogLocked();
  xSemaphoreGive(lock);
  uint32_t age = lastAu ? millis() - lastAu : 0;
  return {fresh ? statKbps : 0, statIdrMs, gopBytes, age, fresh ? statFps : 0, overflows, frozen, lostPackets, backlog};
}

String video_debug() {
  char b[96];
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(b, sizeof b, "fz%d tl%d tt%d tn%d ot%d fg%d nx%d/%d tx%d cu%d uf%c %lus tb%lu", gop.frozen, tailLive,
           tailTrimmed, tail.n, sink.onTail, sink.followGen == gop.gen, sink.next, gop.n, sink.tailNext,
           sink.catchingUp, lastWhy, (unsigned long)(lastWhyAt ? (millis() - lastWhyAt) / 1000 : 0),
           (unsigned long)tailBreaks);
  xSemaphoreGive(lock);
  return b;
}
