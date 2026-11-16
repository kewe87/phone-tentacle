// The web interface's input (web_server.cpp), plain C for the host tests: where on the phone a point of the
// mirrored picture is, what a mouse / keyboard message from the page becomes (the firmware's own HID commands,
// run() in hid_bridge.ino), and the queue that carries them to loop().
//
// Messages from the page (one per WebSocket text frame; x, y in pixels of the video frame, docs/manual.md "Web
// interface"):
//   size W H              the decoded frame's size
//   bars L T R B          black rows / columns at its edges (left, top, right, bottom), as the page measured them
//   down X Y | move X Y | up X Y      the left button on the picture: press, drag, let go (a click is a tap)
//   long X Y              right click: a long press
//   wheel X Y D           the wheel over the picture: a swipe of D frame pixels (D > 0: the page moves up)
//   key COMBO             a key or a combination, e.g. enter, cmd+a, shift+tab, up
//   text TEXT             typed text (ASCII; anything else is skipped)
//   btn home|switcher|control|notifications|keyboard
// Commands out are the firmware's (`abs X Y [STEP]`, `press`, `press_for MS`, `release`, `key ...`, `type ...`,
// `homescreen`), plus `wait MS`, which the caller does itself.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WI_ABS_FULL 32767
#define WI_FLICK_STEP 4096  // per 8 ms report: system gestures need a flick (cli/internal/pointer)
#define WI_DRAG_STEP 1600   // following the mouse while it drags: keeps up with a hand, still a glide for lists
#define WI_LONG_MS 900
#define WI_CMD_MAX 96       // one command line, NUL included
#define WI_TEXT_MAX 512     // text per message
#define WI_WORD_MAX 80      // a `type` command carries at most this many characters

typedef struct {
  int x0, y0, x1, y1;  // [x0, x1) x [y0, y1)
} wi_rect;

// The phone screen inside a w x h frame whose black edges are bars (left, top, right, bottom): the same rules as
// the CLI's screenRect (cli/internal/daemon/area.go). Opposite bars count when both are there, about equal, and the
// picture inside has a phone's aspect; in a frame that has one itself only bars of 3% or more.
wi_rect wi_screen(int w, int h, const int bars[4]);
// A point of the frame (pixels) in `abs` units (0..WI_ABS_FULL) of the phone screen r, clamped to it.
void wi_to_abs(wi_rect r, double x, double y, int *ax, int *ay);

typedef struct {
  int w, h;      // frame size (0: none yet)
  int bars[4];
  bool down;     // the left button is held (down ... up)
  int ax, ay;    // where the pointer was sent last (abs units)
  int layout;    // the keyboard layout text is typed on (KBD_* of src/hid/kbd_map.h; 0, US, after wi_init)
} wi_state;

void wi_init(wi_state *s);

enum { WI_NONE = 0, WI_INPUT, WI_ERR };
// One message from the page. WI_INPUT: n commands in out (at most max); WI_NONE: it only set state (size, bars);
// WI_ERR: malformed or unknown (*why says what). Input before the frame size is known is an error.
int wi_message(wi_state *s, const char *msg, char out[][WI_CMD_MAX], int max, int *n, const char **why);

// ---- The queue to loop(): a ring of command lines. A drag move (`abs X Y STEP` while the button is held) that is
// still waiting is replaced by a newer one, so a slow phone never builds up a backlog of mouse moves.
#define WI_QUEUE 32
typedef struct {
  char cmd[WI_QUEUE][WI_CMD_MAX];
  bool move[WI_QUEUE];
  int head, n;
} wi_queue;

void wi_queue_init(wi_queue *q);
bool wi_queue_push(wi_queue *q, const char *cmd, bool move);  // move: from a `move` message. False: full (dropped)
bool wi_queue_pop(wi_queue *q, char out[WI_CMD_MAX]);
void wi_queue_clear(wi_queue *q);

#ifdef __cplusplus
}
#endif
