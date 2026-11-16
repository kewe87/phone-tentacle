// Who drives the phone, the computer's session (CLI) or the web interface, and when the web's video gives up on a
// browser that falls behind. Plain C state machines for the host tests (web_server.cpp, video_link.cpp). Times are
// millis(); they may wrap.
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

// ---- Video for a browser that falls behind. The web's stream sends every access unit in order (a P-frame needs the
// one before), catching up from the GOP's IDR when it starts. Once it is in step, an access unit older than max_lag
// means the browser (or its Wi-Fi) is too slow for the stream: rather than fall further behind, it skips to the next
// IDR (a new GOP generation). The AirPlay receiver never waits for it either way.
typedef struct {
  bool skipping;      // waits for a GOP generation other than skip_gen
  uint32_t skip_gen;
  uint32_t since;     // when it began to wait
} wv_gate;

void wv_init(wv_gate *g);
// Before sending an access unit of GOP generation gen, stamped ts. catching_up: still sending the GOP's backlog
// (old by nature). False: skip it (and everything of this generation).
bool wv_send(wv_gate *g, uint32_t gen, uint32_t ts, bool catching_up, uint32_t now, uint32_t max_lag);
bool wv_waiting(wv_gate *g, uint32_t gen);              // skipping, and generation gen is still the one skipped
uint32_t wv_waited(const wv_gate *g, uint32_t now);     // ms since it began to wait (0 if not waiting)

#ifdef __cplusplus
}
#endif
