// Who drives the phone, the computer's session (CLI) or the web interface, and what the web's picture waits for.
// Plain C state machines for the host tests (web_server.cpp, video_link.cpp). Times are millis(); they may wrap.
//
// Control (docs/manual.md, "Web interface"): one at a time.
//   - A page that connects gets control by itself if the computer's session sent no input for WC_CLI_IDLE_MS (or
//     there is none). Otherwise it watches, and takes control only when asked to ("Take control").
//   - While the page has control, input from the session is refused ("err busy: ..."); ping and the status still go.
//   - The page gives it back when asked ("Release"), when it closes, or after WC_WEB_IDLE_MS without input from it;
//     then it watches until asked again.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC_CLI_IDLE_MS 60000u        // the session's input this recent: it is still working, the page asks first
#define WC_WEB_IDLE_MS (5u * 60000u) // the page drove nothing this long: control goes back

typedef struct {
  bool connected;   // a page is connected (its WebSocket)
  bool control;     // ... and has control
  bool watching;    // it gave control back (or timed out): no automatic take-over until asked
  uint32_t web_at;  // the page's last input (or when it got control)
  uint32_t cli_at;  // the session's last input
  bool cli_seen;    // cli_at is set
} wc_state;

void wc_init(wc_state *c);
void wc_connect(wc_state *c, uint32_t now);      // a page connected (a new one replaces the old)
void wc_disconnect(wc_state *c);
void wc_take(wc_state *c, uint32_t now);         // "Take control"
void wc_release(wc_state *c);                    // "Release"
void wc_web_input(wc_state *c, uint32_t now);    // input from the page (only while it has control)
void wc_cli_input(wc_state *c, uint32_t now);    // input from the session (it was let through)
// Once a second and before every decision: takes control for a page that may, gives it back after the idle time.
// cli_up: a session is up. Returns true if control changed.
bool wc_tick(wc_state *c, bool cli_up, uint32_t now);
bool wc_cli_active(const wc_state *c, bool cli_up, uint32_t now);  // the session drove the phone lately

// ---- What the page's picture waits for. The web's stream sends every access unit of the chain since the last IDR
// in order (src/video/gop.h: a P-frame needs the one before), however far behind the browser is: it never skips
// ahead to the next IDR, because iOS may send none for the rest of the mirroring session. A browser that is behind
// decodes its way back to the live edge.
//   VW_OK        the stream follows the chain, or is about to start from it (the chain is whole)
//   VW_KEYFRAME  no IDR yet since mirroring started (or one AU did not fit at all)
//   VW_FROZEN    the chain lost its start and this page is not on it (it joined late, reconnected without its
//                decoder, or fell a whole buffer behind): only the next IDR helps (rotating the phone in an app that
//                turns, or restarting Screen Mirroring)
enum { VW_OK, VW_KEYFRAME, VW_FROZEN };
typedef struct {
  int state;
  uint32_t since;  // when it went into this state
} wv_watch;

void wv_init(wv_watch *w);
// following: the page's stream is on the current chain; wait_idr, whole: gop_buf's. Returns the state.
int wv_update(wv_watch *w, bool following, bool wait_idr, bool whole, uint32_t now);
uint32_t wv_waited(const wv_watch *w, uint32_t now);  // ms in this state (0 while VW_OK)

#ifdef __cplusplus
}
#endif
