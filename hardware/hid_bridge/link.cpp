// Command, reply and status queues, see link.h. Replies and status go through a queue drained by one TX task, so
// loop() and the status task never wait on a socket, and control frames leave in the order of their counters
// (cli_link.h: control only from this task).
#include "link.h"
#include <atomic>
#include <esp_heap_caps.h>
#include "cli_link.h"

const size_t CMD_MAX = 512;
const size_t TEXT_MAX = 514;               // one reply or status (cli_link.cpp's TEXT_CAP leaves room for it)
const uint32_t REPLY_TIMEOUT_MS = 3000;
const uint32_t STATUS_TIMEOUT_MS = 500;    // a newer status replaces it a second later anyway

struct Text {
  uint16_t len;
  char s[TEXT_MAX];
};

static QueueHandle_t cmdQueue, replyQueue;
static SemaphoreHandle_t statusLock;
static Text statusText;
static bool statusDirty;
static std::atomic<int> pending;  // replies queued or sending + a status waiting

static void txTask(void *) {
  static Text t;
  for (;;) {
    if (xQueueReceive(replyQueue, &t, pdMS_TO_TICKS(20)) == pdTRUE) {
      cli_send_ctrl('r', t.s, t.len, REPLY_TIMEOUT_MS);
      pending--;
    }
    xSemaphoreTake(statusLock, portMAX_DELAY);
    bool due = statusDirty;
    if (due) t = statusText;
    statusDirty = false;
    xSemaphoreGive(statusLock);
    if (due) {
      cli_send_ctrl('s', t.s, t.len, STATUS_TIMEOUT_MS);
      pending--;
    }
  }
}

static void toText(Text &t, const String &s) {
  t.len = s.length() < TEXT_MAX ? s.length() : TEXT_MAX;
  memcpy(t.s, s.c_str(), t.len);
}

// Queue storage in PSRAM (the two below hold ~7 KB); only the control block stays in internal RAM.
static QueueHandle_t psramQueue(UBaseType_t n, UBaseType_t size) {
  StaticQueue_t *q = (StaticQueue_t *)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL);
  uint8_t *buf = (uint8_t *)heap_caps_malloc(n * size, MALLOC_CAP_SPIRAM);
  if (q && buf) return xQueueCreateStatic(n, size, buf, q);
  free(q);
  free(buf);
  return xQueueCreate(n, size);
}

void link_begin() {
  cmdQueue = psramQueue(6, CMD_MAX);
  replyQueue = psramQueue(8, sizeof(Text));
  statusLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(txTask, "link_tx", 3072, NULL, 3, NULL, 1);
}

void link_push_cmd(const char *line, size_t n) {
  char buf[CMD_MAX] = {0};
  if (!n) return;
  memcpy(buf, line, n < CMD_MAX - 1 ? n : CMD_MAX - 1);
  xQueueSend(cmdQueue, buf, 0);  // full queue: the sender times out and retries
}

bool link_pop_cmd(String &line, uint32_t wait_ms) {
  char buf[CMD_MAX];
  if (xQueueReceive(cmdQueue, buf, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;
  line = buf;
  return true;
}

void link_reply(const String &line) {
  if (!cli_up()) return;
  Text t;
  toText(t, line);
  pending++;
  if (xQueueSend(replyQueue, &t, pdMS_TO_TICKS(200)) != pdTRUE) pending--;  // 8 replies stuck: the link is gone
}

void link_status(const String &full, const String &compact) {
  if (!cli_up()) return;
  xSemaphoreTake(statusLock, portMAX_DELAY);
  toText(statusText, full.length() <= TEXT_MAX ? full : compact);
  if (!statusDirty) pending++;
  statusDirty = true;
  xSemaphoreGive(statusLock);
}

void link_flush() {
  for (int i = 0; i < 100 && pending > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
}
