// Starts ESP-IDF's HTTPS server for the web interface (web_server.cpp). In C: HTTPD_SSL_CONFIG_DEFAULT() is a C
// initializer.
#include "web_https.h"
#include <esp_https_server.h>

httpd_handle_t web_https_start(const char *crt_pem, const char *key_pem, const web_https_opts *o) {
  httpd_ssl_config_t c = HTTPD_SSL_CONFIG_DEFAULT();
  c.servercert = (const uint8_t *)crt_pem;
  c.servercert_len = strlen(crt_pem) + 1;  // PEM: with its NUL
  c.prvtkey_pem = (const uint8_t *)key_pem;
  c.prvtkey_len = strlen(key_pem) + 1;
  c.port_secure = o->port;
  c.tls_handshake_timeout_ms = o->handshake_ms;
  c.httpd.task_priority = o->priority;
  c.httpd.stack_size = o->stack;
  c.httpd.core_id = o->core;
  c.httpd.max_open_sockets = o->sockets;
  c.httpd.max_uri_handlers = o->handlers;
  c.httpd.max_req_hdr_len = o->header_max;
  c.httpd.lru_purge_enable = true;
  c.httpd.recv_wait_timeout = 10;
  c.httpd.send_wait_timeout = 5;
  c.httpd.ctrl_port = o->ctrl_port;
  c.httpd.global_user_ctx = o->ctx;
  httpd_handle_t h = NULL;
  if (httpd_ssl_start(&h, &c) != ESP_OK) return NULL;
  return h;
}

void web_https_stop(httpd_handle_t h) {
  if (h) httpd_ssl_stop(h);
}
