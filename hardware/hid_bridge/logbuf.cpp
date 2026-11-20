#include "logbuf.h"
#include <stdarg.h>
#include "src/log/logring.h"
#include "sdlog.h"

static const size_t RING_SIZE = 8192;
static const size_t PAGE_MAX = 400;  // escaped text per reply; under the reply cap (514) with the header
static char ringBuf[RING_SIZE];
static logring ring = {ringBuf, RING_SIZE, 0, 0};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void board_log_keep(const char *line) {
  char stamped[300];
  uint32_t ms = millis();
  snprintf(stamped, sizeof stamped, "[%lu.%03lu] %s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), line);
  portENTER_CRITICAL(&mux);
  logring_add(&ring, stamped);
  portEXIT_CRITICAL(&mux);
  sdlog_line(line);  // the session log on the card stamps it itself
}

void board_log(const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  size_t n = strlen(line);
  if (n && line[n - 1] == '\n') line[--n] = 0;
#if TENTACLE_DEBUG
  uint32_t ms = millis();
  Serial.printf("[%lu.%03lu] %s\n", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), line);
#else
  Serial.println(line);
#endif
  board_log_keep(line);
}

String board_log_page(const String &arg) {
  static char page[PAGE_MAX + 1];
  unsigned long from = 0, max = 120;
  sscanf(arg.c_str(), "%lu %lu", &from, &max);
  if (max < 16) max = 16;
  if (max > PAGE_MAX) max = PAGE_MAX;
  portENTER_CRITICAL(&mux);
  uint32_t next = logring_read(&ring, from, page, max + 1), end = ring.end;
  portEXIT_CRITICAL(&mux);
  return String("ok ") + next + " " + end + " " + page;
}

uint32_t board_log_read(uint32_t from, char *out, size_t max) {
  portENTER_CRITICAL(&mux);
  uint32_t next = from == ring.end ? from : logring_read(&ring, from, out, max);
  portEXIT_CRITICAL(&mux);
  if (next == from) out[0] = 0;
  return next;
}

uint32_t board_log_end() {
  portENTER_CRITICAL(&mux);
  uint32_t end = ring.end;
  portEXIT_CRITICAL(&mux);
  return end;
}
