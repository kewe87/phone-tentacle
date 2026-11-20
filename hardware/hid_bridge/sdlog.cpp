// See sdlog.h.
#include "sdlog.h"
#include <FS.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include "board_hw.h"
#include "logbuf.h"
#include "netcfg.h"
#include "video_link.h"
#include "wallclock.h"
#include "src/setup/board_opts.h"
#include "src/video/gop.h"
#include "src/video/tsmux.h"

static const int QUEUE_LINES = 48, SDLOG_LINE_MAX = 192;
static const uint32_t FLUSH_MS = 2000;
static const char *DIR_PATH = "/tentacle";

struct Line { char text[SDLOG_LINE_MAX]; };
static QueueHandle_t queue;
static volatile int state = SDLOG_NONE;
static volatile uint32_t asked, applied;  // sdlog_reconfigure() calls, and how many of them apply() has seen
static volatile uint32_t dropped;
static char path[32];
static unsigned long number;  // sNNNNN: the log's and the video's
static File file;
static bool mounted;

// ---- The screen: video_link.cpp's SD sink hands over each access unit on its task (video_sd_begin) -------------
static const size_t TS_BUF = 16 * 1024;             // packets gathered for one write (internal RAM: the card's DMA)
static const uint64_t TS_PART_MAX = 3900ull << 20;  // FAT32 ends at 4 GB - 1
static SemaphoreHandle_t vlock;  // the video file and its buffer: the SD sink's task, the writer task (open, close)
static File tsFile;
static char tsPath[40];
static uint8_t *tsBuf;
static size_t tsFill;
static uint64_t tsPartBytes, tsBytes;
static int tsPart;
static bool tsFailed;
static volatile bool tsOpen;
static volatile uint32_t tsGen;  // bumps with every new file: the sink starts over from a whole chain
static uint32_t tsLastFlush;
static tsmux mux;
static uint8_t cfg[GOP_CFG_MAX];
static size_t cfgLen;

const char *sdlog_state_name() {
  switch (state) {
    case SDLOG_OFF: return "off";
    case SDLOG_NOCARD: return "nocard";
    case SDLOG_REC: return "rec";
    case SDLOG_ERR: return "err";
    default: return "none";
  }
}
int sdlog_state() { return state; }
bool sdlog_video() { return state == SDLOG_REC && tsOpen; }
const char *sdlog_file() { return state != SDLOG_REC ? "" : tsOpen ? tsPath : path; }

static bool mount() {
  if (mounted) return true;
  SD_MMC.setPins(WS_SD_CLK, WS_SD_CMD, WS_SD_D0);
  mounted = SD_MMC.begin("/sdcard", true /* 1 bit */, false /* never format */);
  return mounted;
}

// Writes the gathered packets; vlock held. On the 3.9 GB mark the stream goes on in the next part.
static void tsWrite() {
  if (!tsFill || tsFailed) return;
  if (tsPartBytes + tsFill > TS_PART_MAX) {
    tsFile.close();
    snprintf(tsPath, sizeof tsPath, "%s/s%05lu_%d.ts", DIR_PATH, number, ++tsPart);
    tsFile = SD_MMC.open(tsPath, FILE_WRITE);
    tsPartBytes = 0;
    if (!tsFile) {
      tsFailed = true;
      return;
    }
  }
  if (tsFile.write(tsBuf, tsFill) != tsFill) tsFailed = true;  // the card was pulled, or is full
  tsPartBytes += tsFill;
  tsBytes += tsFill;
  tsFill = 0;
}

static void tsPacket(void *, const uint8_t *pkt) {
  if (tsFill + TS_PACKET > TS_BUF) tsWrite();
  memcpy(tsBuf + tsFill, pkt, TS_PACKET);
  tsFill += TS_PACKET;
}

static size_t sdRoom() { return tsOpen && !tsFailed ? 1 : 0; }
static uint32_t sdGen() { return tsGen; }

static bool sdAu(const uint8_t *p, size_t n, uint8_t flags, uint32_t ts) {
  xSemaphoreTake(vlock, portMAX_DELAY);
  bool ok = tsOpen && !tsFailed;
  if (ok && (flags & VF_CONFIG)) {
    cfgLen = n <= sizeof cfg ? n : 0;
    if (cfgLen) memcpy(cfg, p, n);
  } else if (ok) {
    tsmux_au(&mux, p, n, flags & VF_IDR, cfg, cfgLen, ts);
    ok = !tsFailed;
  }
  xSemaphoreGive(vlock);
  return ok;
}

// vlock held.
static void videoClose(const char *why) {
  if (!tsOpen) return;
  tsWrite();
  tsFile.close();
  tsOpen = false;
  board_log("sdlog: screen recording %s (%s, %llu MB)", why, tsPath, (unsigned long long)(tsBytes >> 20));
}

static void videoOpen() {
  if (!tsBuf) tsBuf = (uint8_t *)heap_caps_malloc(TS_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);  // the card's DMA
  if (!tsBuf) tsBuf = (uint8_t *)heap_caps_malloc(TS_BUF, MALLOC_CAP_SPIRAM);
  if (!tsBuf || !video_sd_begin(sdRoom, sdAu, sdGen)) {
    board_log("sdlog: no memory for the screen recording");
    return;
  }
  xSemaphoreTake(vlock, portMAX_DELAY);
  tsPart = 1;
  snprintf(tsPath, sizeof tsPath, "%s/s%05lu.ts", DIR_PATH, number);
  tsFile = SD_MMC.open(tsPath, FILE_WRITE);
  tsFill = 0;
  tsPartBytes = tsBytes = 0;
  tsFailed = !tsFile;
  tsmux_init(&mux, tsPacket, NULL);
  cfgLen = 0;
  tsLastFlush = millis();
  tsOpen = (bool)tsFile;
  tsGen = tsGen + 1;
  xSemaphoreGive(vlock);
  if (tsOpen) {
    board_log("sdlog: recording the screen to %s (from the last keyframe on)", tsPath);
    video_sd_kick();
  } else board_log("sdlog: cannot create %s", tsPath);
}

static void unmount() {
  xSemaphoreTake(vlock, portMAX_DELAY);
  videoClose("stopped");
  xSemaphoreGive(vlock);
  if (file) file.close();
  if (mounted) SD_MMC.end();
  mounted = false;
}

// The next free number: /tentacle/sNNNNN.log (and .ts).
static bool openNext() {
  if (!SD_MMC.exists(DIR_PATH) && !SD_MMC.mkdir(DIR_PATH)) return false;
  unsigned long top = 0;
  File dir = SD_MMC.open(DIR_PATH);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    unsigned long n;
    const char *name = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
    if (sscanf(name, "s%lu", &n) == 1 && n > top) top = n;
    f.close();
  }
  dir.close();
  number = top + 1;
  snprintf(path, sizeof path, "%s/s%05lu.log", DIR_PATH, number);
  file = SD_MMC.open(path, FILE_APPEND);
  return (bool)file;
}

// The setting and the card: start, stop or retry. Only the writer task calls it.
static void apply() {
  board_opts o;
  cfg_opts(&o);
  if (o.sdlog == BO_SD_OFF) {
    if (state == SDLOG_REC) board_log("sdlog: stopped (%s)", path);
    unmount();
    state = SDLOG_OFF;
    return;
  }
  if (state != SDLOG_REC) {
    if (!mount()) {
      state = SDLOG_NOCARD;
      board_log("sdlog: no card (or not FAT32)");
      return;
    }
    if (!openNext()) {
      unmount();
      state = SDLOG_ERR;
      board_log("sdlog: cannot write to the card");
      return;
    }
    state = SDLOG_REC;
    file.printf("# Tentacle session log, board %s, %s\n", board_name().c_str(), board_hw_name());
    board_log("sdlog: recording to %s (%llu MB card)", path, (unsigned long long)(SD_MMC.cardSize() >> 20));
  }
  if (o.sdlog == BO_SD_VIDEO && !tsOpen) videoOpen();
  else if (o.sdlog != BO_SD_VIDEO && tsOpen) {
    xSemaphoreTake(vlock, portMAX_DELAY);
    videoClose("stopped");
    xSemaphoreGive(vlock);
  }
}

// The screen's file: flush now and then; a failed write stops it (the log goes on if it can).
static void videoTick() {
  if (!tsOpen) return;
  xSemaphoreTake(vlock, portMAX_DELAY);
  if (!tsFailed && millis() - tsLastFlush >= FLUSH_MS) {
    tsWrite();
    tsFile.flush();
    tsLastFlush = millis();
  }
  if (tsFailed) videoClose("failed: card pulled or full?");
  xSemaphoreGive(vlock);
}

static void writer(void *) {
  Line l;
  uint32_t lastFlush = millis();
  apply();
  for (;;) {
    uint32_t a = asked;
    if (a != applied) {
      apply();
      applied = a;
    }
    bool got = xQueueReceive(queue, &l, pdMS_TO_TICKS(500)) == pdTRUE;
    if (state != SDLOG_REC) continue;  // the queue only fills while recording; a leftover line goes
    videoTick();
    uint32_t d = dropped;
    if (d) {
      dropped = 0;
      file.printf("# %lu lines dropped (card too slow)\n", (unsigned long)d);
    }
    if (got && file.print(l.text) == 0) {  // the card was pulled, or is full
      unmount();
      state = SDLOG_ERR;
      board_log("sdlog: write failed, stopped (card pulled or full?)");
      continue;
    }
    if (millis() - lastFlush >= FLUSH_MS) {
      file.flush();
      lastFlush = millis();
    }
  }
}

void sdlog_begin() {
  if (board_hw() != BOARD_WS_EPD154) return;  // no slot
  queue = xQueueCreateWithCaps(QUEUE_LINES, sizeof(Line), MALLOC_CAP_SPIRAM);  // 9 KB, not from internal RAM
  vlock = xSemaphoreCreateMutex();
  if (!queue || !vlock) return;
  state = SDLOG_OFF;
  xTaskCreatePinnedToCore(writer, "sdlog", 4096, NULL, 1, NULL, 0);
}

void sdlog_reconfigure() {
  if (!queue) return;
  uint32_t a = ++asked;
  for (int i = 0; i < 40 && (int32_t)(applied - a) < 0; i++) delay(50);  // at most 2 s: the reply shows the outcome
}

static void push(const char *text) {
  Line l;
  char when[32];
  if (!clock_local(when, sizeof when)) {  // not set yet: seconds since boot
    uint32_t ms = millis();
    snprintf(when, sizeof when, "+%lu.%03lus", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  }
  snprintf(l.text, sizeof l.text, "%s %s\n", when, text);
  if (xQueueSend(queue, &l, 0) != pdTRUE) dropped = dropped + 1;
}

void sdlog_line(const char *line) {
  if (state != SDLOG_REC || !queue) return;
  push(line);
}

void sdlog_input(char who, const char *line, const char *reply) {
  if (state != SDLOG_REC || !queue) return;
  char text[SDLOG_LINE_MAX - 16];
  const char *src = who == 'c' ? "cli" : who == 'w' ? "web" : who == 's' ? "serial" : "?";
  if (!strncmp(line, "type ", 5))  // never the text itself: it may be a password
    snprintf(text, sizeof text, "> %s type [%u chars] -> %.40s", src, (unsigned)strlen(line + 5), reply);
  else snprintf(text, sizeof text, "> %s %.100s -> %.40s", src, line, reply);
  push(text);
}
