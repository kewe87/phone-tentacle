// CLI link over Wi-Fi, see cli_tcp.h. Reads run on this file's task and hand each frame to cli_on_frame(); the same
// task runs the pairing (cli_pair_poll()). Sends come from cli_link.cpp (this task, the TX and video tasks), under
// a lock.
#include "cli_tcp.h"
#include <esp_heap_caps.h>
#include "lwip/sockets.h"
#include "cli_link.h"
#include "dbg.h"
#include "logbuf.h"

const int SEND_TIMEOUT_MS = 2000;
const int IDLE_TIMEOUT_MS = 6000;  // the CLI sends a keepalive every 2 s; a computer gone off Wi-Fi sends no FIN
const size_t FRAME_MAX = 600;      // largest frame from the computer (cli_link.cpp's TEXT_CAP + seal)
const int POLL_MS = 200;           // how often the reader looks at the pairing and the deadlines below
// One computer at a time: a connection that has neither a session nor an open pairing request by then is closed,
// so a stranger on the Wi-Fi cannot hold the board's only link with keepalives.
const uint32_t UNAUTH_MS = 10000;
const uint32_t KEEPALIVE_MS = 2000;  // ours, while a pairing request waits for the button (no status goes out then)

static int listener = -1;
static volatile int client = -1;
static SemaphoreHandle_t lock;  // client + writes (a record goes out whole)
static uint8_t *rx, *tx;        // PSRAM
static uint32_t winStart, winBytes;
static volatile uint32_t kbps;

static bool readAll(int fd, uint8_t *p, size_t n) {
  while (n) {
    int k = recv(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= k;
  }
  return true;
}

static void serve(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  timeval stx = {SEND_TIMEOUT_MS / 1000, (SEND_TIMEOUT_MS % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stx, sizeof stx);
  timeval srx = {IDLE_TIMEOUT_MS / 1000, (IDLE_TIMEOUT_MS % 1000) * 1000};  // for a record that stops half-way
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &srx, sizeof srx);
  xSemaphoreTake(lock, portMAX_DELAY);
  client = fd;
  xSemaphoreGive(lock);
  sockaddr_in peer = {};
  socklen_t plen = sizeof peer;
  getpeername(fd, (sockaddr *)&peer, &plen);
  board_log("cli: wifi link up (%s)", inet_ntoa(peer.sin_addr));
  uint32_t lastRx = millis(), since = lastRx, lastKeep = lastRx;
  const char *why = "closed by the computer";
  for (;;) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    timeval tv = {0, POLL_MS * 1000};
    int r = select(fd + 1, &rf, NULL, NULL, &tv);
    if (r < 0) break;
    if (r > 0) {
      uint8_t hdr[2];
      if (!readAll(fd, hdr, 2)) break;
      size_t n = hdr[0] | hdr[1] << 8;
      if (n > FRAME_MAX || (n && !readAll(fd, rx, n))) break;
      lastRx = millis();
      if (n) cli_on_frame(CLI_TCP_SLOT, rx, n);
    }
    cli_pair_poll();  // the BOOT button's answer to an open request, or its timeout
    uint32_t now = millis();
    if (now - lastRx >= (uint32_t)IDLE_TIMEOUT_MS) { why = "quiet for 6 s"; break; }
    if (cli_authed(CLI_TCP_SLOT) || cli_pair_open()) since = now;
    else if (now - since >= UNAUTH_MS) { why = "no session within 10 s"; break; }
    if (cli_pair_open() && now - lastKeep >= KEEPALIVE_MS) {
      lastKeep = now;
      cli_tcp_send(NULL, 0, 0);
    }
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  client = -1;
  xSemaphoreGive(lock);
  cli_reset(CLI_TCP_SLOT);
  close(fd);
  board_log("cli: wifi link down (%s)", why);
}

static void serverTask(void *) {
  for (;;) {
    int fd = accept(listener, NULL, NULL);
    if (fd >= 0) serve(fd);  // one computer at a time; a new one waits until this one is gone
  }
}

bool cli_tcp_begin() {
  lock = xSemaphoreCreateMutex();
  rx = (uint8_t *)heap_caps_malloc(FRAME_MAX + 2 + CLI_TCP_MTU, MALLOC_CAP_SPIRAM);
  if (!rx) rx = (uint8_t *)malloc(FRAME_MAX + 2 + CLI_TCP_MTU);
  if (!rx) return false;
  tx = rx + FRAME_MAX;
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return false;
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CLI_TCP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listener, (sockaddr *)&addr, sizeof addr) < 0 || listen(listener, 1) < 0) return false;
  // 8 KB of stack: pairing's X25519 runs on this task
  return xTaskCreatePinnedToCore(serverTask, "cli_tcp", 8192, NULL, 3, NULL, 1) == pdPASS;
}

bool cli_tcp_up() { return client >= 0; }

bool cli_tcp_send(const uint8_t *p, size_t n, uint32_t) {
  if (n > CLI_TCP_MTU) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = client >= 0;
  if (ok) {
    tx[0] = (uint8_t)n;  // length and frame in one send: one segment per record
    tx[1] = (uint8_t)(n >> 8);
    if (n) memcpy(tx + 2, p, n);
    for (size_t off = 0; ok && off < n + 2;) {
      int k = send(client, tx + off, n + 2 - off, 0);
      ok = k > 0;
      off += ok ? k : 0;
    }
    if (!ok) shutdown(client, SHUT_RDWR);  // stalled (SO_SNDTIMEO) or gone: the reader sees it and closes
  }
  xSemaphoreGive(lock);
  if (ok) {
    uint32_t now = millis();
    winBytes += n + 2;
    if (now - winStart >= 1000) {
      kbps = winBytes * 8 / (now - winStart);
      winBytes = 0;
      winStart = now;
    }
  }
  return ok;
}

void cli_tcp_drop() {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (client >= 0) shutdown(client, SHUT_RDWR);
  xSemaphoreGive(lock);
}

uint32_t cli_tcp_kbps() { return millis() - winStart > 2000 ? 0 : kbps; }
