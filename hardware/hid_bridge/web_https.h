// ESP-IDF's HTTPS server, started from C (web_https.c).
#pragma once
#include <esp_http_server.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t port, ctrl_port;
  uint32_t handshake_ms;
  unsigned priority, stack;
  int core;
  uint16_t sockets, handlers, header_max;
  void *ctx;
} web_https_opts;

httpd_handle_t web_https_start(const char *crt_pem, const char *key_pem, const web_https_opts *o);  // NULL: failed
void web_https_stop(httpd_handle_t h);

#ifdef __cplusplus
}
#endif
