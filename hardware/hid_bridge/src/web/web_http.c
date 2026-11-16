// HTTP helpers, see web_http.h.
#include "web_http.h"
#include <ctype.h>
#include <string.h>

bool wh_cookie(const char *h, const char *name, char *out, size_t cap) {
  size_t n = strlen(name);
  if (!h || !n || !cap) return false;
  const char *p = h;
  while (*p) {
    while (*p == ' ' || *p == ';' || *p == '\t') p++;
    const char *end = strchr(p, ';');
    if (!end) end = p + strlen(p);
    if ((size_t)(end - p) > n && !strncmp(p, name, n) && p[n] == '=') {
      const char *v = p + n + 1;
      size_t len = (size_t)(end - v);
      while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
      if (len >= cap) return false;
      memcpy(out, v, len);
      out[len] = 0;
      return true;
    }
    p = end;
  }
  return false;
}

bool wh_host(const char *host, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!host || !*host || !cap) return false;
  size_t n = strlen(host);
  if (n >= cap) return false;
  bool v6 = host[0] == '[';
  size_t keep = n;
  for (size_t i = 0; i < n; i++) {
    char c = host[i];
    if (isalnum((unsigned char)c) || c == '.' || c == '-') continue;
    if (v6 && (c == '[' || c == ']' || c == ':')) continue;
    if (c == ':' && !v6) {  // host:port: the port goes
      keep = i;
      for (size_t j = i + 1; j < n; j++)
        if (!isdigit((unsigned char)host[j])) return false;
      break;
    }
    return false;
  }
  if (v6) {  // [::1]:8080: keep up to the bracket
    const char *b = strchr(host, ']');
    if (!b) return false;
    keep = (size_t)(b - host) + 1;
    for (size_t j = keep; j < n; j++)
      if (!(j == keep && host[j] == ':') && !isdigit((unsigned char)host[j])) return false;
  }
  if (!keep) return false;
  memcpy(out, host, keep);
  out[keep] = 0;
  return true;
}

bool wh_same_origin(const char *origin, const char *host) {
  if (!origin || !host || !*host || strncmp(origin, "https://", 8)) return false;
  return !strcmp(origin + 8, host);
}
