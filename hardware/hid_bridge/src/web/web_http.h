// Small HTTP helpers for the web interface (web_server.cpp), plain C for the host tests: the session cookie, the
// Host header (for the port-80 redirect and the Origin check).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The value of cookie `name` in a Cookie header ("a=1; tsid=abc"). False if absent or longer than cap - 1.
bool wh_cookie(const char *header, const char *name, char *out, size_t cap);
// A Host header made safe to echo in a Location header: letters, digits, '.', '-', ':' and '[' ']' (IPv6) only,
// the port dropped. False (out empty) if anything else is in it.
bool wh_host(const char *host, char *out, size_t cap);
// A state-changing request comes from our own page: its Origin is https://<Host> (the port, if any, as in Host).
// A missing Origin counts as foreign: every browser we support sends one on POST and on a WebSocket.
bool wh_same_origin(const char *origin, const char *host);

#ifdef __cplusplus
}
#endif
