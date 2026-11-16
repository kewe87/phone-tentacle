// The web interface's input: see web_input.h.
#include "web_input.h"
#include "../hid/kbd_map.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Width / height of phone and tablet screens (portrait), as in cli/internal/daemon/area.go.
static const double ASPECTS[] = {9 / 19.5, 9.0 / 16, 3.0 / 4, 1640.0 / 2360, 1488.0 / 2266};

static double aspect_miss(int dx, int dy) {
  if (dx <= 0 || dy <= 0) return 1e9;
  double a = (double)(dx < dy ? dx : dy) / (dx < dy ? dy : dx), miss = 1e9;
  for (size_t i = 0; i < sizeof ASPECTS / sizeof ASPECTS[0]; i++) {
    double m = fabs(a - ASPECTS[i]) / ASPECTS[i];
    if (m < miss) miss = m;
  }
  return miss;
}

static bool pair(int a, int b, int size) {
  int tol = size / 200 > 2 ? size / 200 : 2;
  return a >= 2 && b >= 2 && abs(a - b) <= tol && a + b < size / 2;
}

wi_rect wi_screen(int w, int h, const int bars[4]) {
  wi_rect full = {0, 0, w, h}, r = full;
  if (w <= 0 || h <= 0) return full;
  if (pair(bars[0], bars[2], w)) r.x0 = bars[0], r.x1 = w - bars[2];
  if (pair(bars[1], bars[3], h)) r.y0 = bars[1], r.y1 = h - bars[3];
  bool changed = r.x0 || r.y0 || r.x1 != w || r.y1 != h;
  bool wide = r.x0 >= w * 3 / 100 || r.y0 >= h * 3 / 100;  // the receiver's own size has a phone's aspect too
  if (changed && aspect_miss(r.x1 - r.x0, r.y1 - r.y0) < 0.008 && (aspect_miss(w, h) >= 0.008 || wide)) return r;
  return full;
}

static int unit(double f) {
  if (!(f > 0)) f = 0;  // NaN too
  if (f > 1) f = 1;
  return (int)lround(f * WI_ABS_FULL);
}

void wi_to_abs(wi_rect r, double x, double y, int *ax, int *ay) {
  int dx = r.x1 - r.x0, dy = r.y1 - r.y0;
  *ax = unit(dx > 0 ? (x - r.x0) / dx : 0);
  *ay = unit(dy > 0 ? (y - r.y0) / dy : 0);
}

void wi_init(wi_state *s) { memset(s, 0, sizeof *s); }

// ---- Messages ---------------------------------------------------------------------------------------------------
typedef struct {
  char (*out)[WI_CMD_MAX];
  int max, n;
  bool full;
} sink;

static void put(sink *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sink *k, const char *fmt, ...) {
  if (k->n >= k->max) {
    k->full = true;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(k->out[k->n++], WI_CMD_MAX, fmt, ap);
  va_end(ap);
}

static bool word_is(const char *msg, const char *w, const char **rest) {
  size_t n = strlen(w);
  if (strncmp(msg, w, n) || (msg[n] && msg[n] != ' ')) return false;
  *rest = msg[n] ? msg + n + 1 : msg + n;
  return true;
}

// Text as `type WORD` commands, spaces / newlines / tabs as keys of their own: inside a `type` line iOS dropped every
// space (cli/internal/pointer/pointer.go, Type). Characters the layout can not type are skipped (the board's `type`
// would refuse the whole word); the others go as UTF-8, never cut inside a character.
static void text(sink *k, const char *t, int layout) {
  char word[WI_WORD_MAX + 1];
  size_t w = 0;
  for (const char *p = t;;) {
    const char *at = p;
    uint32_t cp = kbd_utf8_next(&p);
    char c = cp < 0x80 ? (char)cp : 0x7f;
    bool sep = !cp || cp == ' ' || cp == '\n' || cp == '\t';
    kbd_key_t keys[2];
    bool printable = cp > 0x20 && cp != 0x7f && cp != 0xFFFD && kbd_map_keys(layout, cp, keys) > 0;
    size_t len = (size_t)(p - at);
    if (printable && w + len > WI_WORD_MAX) {  // full: this character starts the next word
      word[w] = 0;
      put(k, "type %s", word);
      w = 0;
    }
    if (printable) {
      memcpy(word + w, at, len);
      w += len;
    }
    if (w && sep) {
      word[w] = 0;
      put(k, "type %s", word);
      w = 0;
    }
    if (!cp) break;
    if (c == ' ') put(k, "key space");
    else if (c == '\n') put(k, "key enter");
    else if (c == '\t') put(k, "key tab");
  }
}

static bool combo_ok(const char *c) {
  size_t n = strlen(c);
  if (!n || n > 40) return false;
  for (size_t i = 0; i < n; i++)
    if (c[i] <= 0x20 || c[i] >= 0x7f) return false;
  return true;
}

int wi_message(wi_state *s, const char *msg, char out[][WI_CMD_MAX], int max, int *n, const char **why) {
  sink k = {out, max, 0, false};
  const char *rest;
  double x = 0, y = 0, d = 0;
  int a[4];
  *n = 0;
  *why = "unknown message";
  if (word_is(msg, "size", &rest)) {
    if (sscanf(rest, "%d %d", &a[0], &a[1]) != 2 || a[0] < 16 || a[1] < 16 || a[0] > 8192 || a[1] > 8192) {
      *why = "usage: size W H";
      return WI_ERR;
    }
    if (a[0] != s->w || a[1] != s->h) memset(s->bars, 0, sizeof s->bars);  // measured on the old size
    s->w = a[0], s->h = a[1];
    return WI_NONE;
  }
  if (word_is(msg, "bars", &rest)) {
    if (sscanf(rest, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) != 4 || a[0] < 0 || a[1] < 0 || a[2] < 0 || a[3] < 0) {
      *why = "usage: bars L T R B";
      return WI_ERR;
    }
    memcpy(s->bars, a, sizeof a);
    return WI_NONE;
  }
  if (word_is(msg, "key", &rest)) {
    if (!combo_ok(rest)) {
      *why = "usage: key COMBO";
      return WI_ERR;
    }
    put(&k, "key %s", rest);
  } else if (word_is(msg, "text", &rest)) {
    if (strlen(rest) > WI_TEXT_MAX) {
      *why = "text too long";
      return WI_ERR;
    }
    text(&k, rest, s->layout);
  } else if (word_is(msg, "btn", &rest)) {
    // System gestures on the absolute pointer, as fractions of the screen (portrait).
    if (!strcmp(rest, "home")) put(&k, "homescreen");
    else if (!strcmp(rest, "keyboard")) put(&k, "key eject");  // shows / hides the on-screen keyboard
    else if (!strcmp(rest, "switcher")) {  // up from the home bar, then rest: the app switcher
      put(&k, "abs 16384 32700");
      put(&k, "press");
      put(&k, "abs 16384 19500");  // the board's own glide: slow on purpose
      put(&k, "wait 450");
      put(&k, "release");
    } else if (!strcmp(rest, "control") || !strcmp(rest, "notifications")) {  // down from the top edge, a flick
      int ax = !strcmp(rest, "control") ? 30800 : 9000;
      put(&k, "abs %d 120", ax);
      put(&k, "press");
      put(&k, "abs %d 20000 %d", ax, WI_FLICK_STEP);
      put(&k, "release");
    } else {
      *why = "usage: btn home|switcher|control|notifications|keyboard";
      return WI_ERR;
    }
    s->down = false;
  } else {
    // Pointer messages: they need the frame's size, to know where on the phone the point is.
    bool down = word_is(msg, "down", &rest), move = !down && word_is(msg, "move", &rest);
    bool up = !down && !move && word_is(msg, "up", &rest), lng = !down && !move && !up && word_is(msg, "long", &rest);
    bool wheel = !down && !move && !up && !lng && word_is(msg, "wheel", &rest);
    if (!down && !move && !up && !lng && !wheel) return WI_ERR;
    int got = sscanf(rest, "%lf %lf %lf", &x, &y, &d);
    if (got < (wheel ? 3 : 2) || !isfinite(x) || !isfinite(y) || !isfinite(d)) {
      *why = "usage: down|move|up|long X Y, wheel X Y D";
      return WI_ERR;
    }
    if (s->w <= 0 || s->h <= 0) {
      *why = "no video yet: the frame size is unknown";
      return WI_ERR;
    }
    wi_rect r = wi_screen(s->w, s->h, s->bars);
    int ax, ay;
    wi_to_abs(r, x, y, &ax, &ay);
    if (down) {
      put(&k, "abs %d %d", ax, ay);
      put(&k, "press");
      s->down = true;
    } else if (move) {
      if (!s->down || (ax == s->ax && ay == s->ay)) return WI_NONE;  // hovering moves nothing: a glide costs time
      put(&k, "abs %d %d %d", ax, ay, WI_DRAG_STEP);
    } else if (up) {
      if (s->down && (ax != s->ax || ay != s->ay)) put(&k, "abs %d %d %d", ax, ay, WI_DRAG_STEP);
      put(&k, "release");
      s->down = false;
    } else if (lng) {
      put(&k, "abs %d %d", ax, ay);
      put(&k, "press_for %d", WI_LONG_MS);
      s->down = false;
    } else {  // wheel: a swipe that rests before it lets go, so iOS adds no momentum (pointer.go, Hold)
      if (s->down) return WI_NONE;
      int dy = r.y1 - r.y0, end = ay - (int)lround(d / (dy > 0 ? dy : 1) * WI_ABS_FULL);
      if (end < 0) end = 0;
      if (end > WI_ABS_FULL) end = WI_ABS_FULL;
      if (end == ay) return WI_NONE;
      put(&k, "abs %d %d", ax, ay);
      put(&k, "press");
      put(&k, "abs %d %d", ax, end);
      put(&k, "wait 250");
      put(&k, "release");
      ay = end;
    }
    s->ax = ax, s->ay = ay;
  }
  if (k.full) {
    *why = "too many commands";
    return WI_ERR;
  }
  *n = k.n;
  return k.n ? WI_INPUT : WI_NONE;
}

// ---- Queue --------------------------------------------------------------------------------------------------------
void wi_queue_init(wi_queue *q) { memset(q, 0, sizeof *q); }
void wi_queue_clear(wi_queue *q) { q->head = q->n = 0; }

bool wi_queue_push(wi_queue *q, const char *cmd, bool move) {
  if (move && q->n) {
    int last = (q->head + q->n - 1) % WI_QUEUE;
    if (q->move[last]) {  // a newer position replaces the one still waiting
      snprintf(q->cmd[last], WI_CMD_MAX, "%s", cmd);
      return true;
    }
  }
  if (q->n == WI_QUEUE) return false;
  int i = (q->head + q->n) % WI_QUEUE;
  snprintf(q->cmd[i], WI_CMD_MAX, "%s", cmd);
  q->move[i] = move;
  q->n++;
  return true;
}

bool wi_queue_pop(wi_queue *q, char out[WI_CMD_MAX]) {
  if (!q->n) return false;
  memcpy(out, q->cmd[q->head], WI_CMD_MAX);
  q->head = (q->head + 1) % WI_QUEUE;
  q->n--;
  return true;
}
