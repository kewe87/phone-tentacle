#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The BOOT button, the "press the button to confirm" request and the pairing window, as plain state machines (the
// task, the pin and the LED are in button.cpp). Times are millis(); they may wrap.

// ---- Button: debounced press edge, and one long-press event while it stays held.
#define BTN_DEBOUNCE_MS 30
#define BTN_LONG_MS 10000

enum { BTN_NONE = 0, BTN_PRESS, BTN_LONG };

typedef struct {
  bool raw, down, long_fired;
  uint32_t raw_at, down_at;
} btn_state;

void btn_init(btn_state *b);
int btn_step(btn_state *b, bool pressed, uint32_t now);  // BTN_PRESS once per press, BTN_LONG once after BTN_LONG_MS
uint32_t btn_held_ms(const btn_state *b, uint32_t now);  // 0 when up

// ---- Confirmation: one open request at a time; a short press accepts it, the timeout refuses it.
enum { CONFIRM_NONE = 0, CONFIRM_WIFI = 1, CONFIRM_PAIR = 2, CONFIRM_WEBPW = 3 };  // who asks (3: a new web password)
enum { CF_IDLE = 0, CF_WAITING, CF_ACCEPTED, CF_EXPIRED };           // CF_ACCEPTED / CF_EXPIRED stay until cleared
#define CF_TEXT 22  // one display line (21 characters of the 6-pixel font)
// A press this soon after a request opened was on its way before anyone could have seen the request: it is not an
// answer (it stays a normal press, the stop toggle). A request from the network must not catch a press meant to stop.
#define CF_ARM_MS 1000

typedef struct {
  int kind, state;
  uint32_t start, timeout;
  char l1[CF_TEXT], l2[CF_TEXT];  // what is to be confirmed, for the display
} confirm_slot;

void cf_init(confirm_slot *c);
// Opens a request. The same kind may ask again (new text, new timeout); false while another kind is waiting.
bool cf_request(confirm_slot *c, int kind, const char *l1, const char *l2, uint32_t timeout_ms, uint32_t now);
void cf_tick(confirm_slot *c, uint32_t now);         // CF_WAITING -> CF_EXPIRED once the time is up
bool cf_press(confirm_slot *c, uint32_t now);        // a press: true if it accepted an open request (it is used up);
                                                     // false also within CF_ARM_MS of the request
int cf_state(const confirm_slot *c, int kind);       // CF_IDLE for a kind that did not ask
void cf_clear(confirm_slot *c, int kind);            // the asker read the result, or withdraws the request
uint32_t cf_left_ms(const confirm_slot *c, uint32_t now);  // 0 unless waiting

// ---- Pairing window: a double press (two presses within PW_DOUBLE_MS) opens it for PW_OPEN_MS. Only while it is
// open does the board take a pairing request from the network (cli_link.cpp), so nothing on the Wi-Fi can make the
// display or the LED ask for the button by itself.
#define PW_DOUBLE_MS 500
#define PW_OPEN_MS 60000

enum { PW_SINGLE = 0, PW_DOUBLE };

typedef struct {
  bool first, open;  // first: the last press may still become a double press
  uint32_t first_at, open_at;
} pair_window;

void pw_init(pair_window *w);
// A press that answered no request. allowed: a double press may open the window now (nothing drives the phone and
// no request waits for the button). PW_SINGLE: an ordinary press; it is done at once (the stop toggle is never
// delayed). PW_DOUBLE: the second press of a double press: the window is open (if it was already, its time starts
// over), and the caller undoes what the first press did instead of acting on this one.
int pw_press(pair_window *w, bool allowed, uint32_t now);
void pw_forget(pair_window *w);                    // the last press can not start a double press (it confirmed, or is held)
void pw_tick(pair_window *w, uint32_t now);        // closes the window once its time is up
bool pw_is_open(const pair_window *w);
uint32_t pw_left_ms(const pair_window *w, uint32_t now);  // 0 unless open
void pw_close(pair_window *w);                     // paired, or a session came up

#ifdef __cplusplus
}
#endif
