#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The BOOT button and the "press the button to confirm" request, as plain state machines (the task, the pin and
// the LED are in button.cpp). Times are millis(); they may wrap.

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
enum { CONFIRM_NONE = 0, CONFIRM_WIFI = 1, CONFIRM_PAIR = 2 };       // who asks
enum { CF_IDLE = 0, CF_WAITING, CF_ACCEPTED, CF_EXPIRED };           // CF_ACCEPTED / CF_EXPIRED stay until cleared
#define CF_TEXT 22  // one display line (21 characters of the 6-pixel font)

typedef struct {
  int kind, state;
  uint32_t start, timeout;
  char l1[CF_TEXT], l2[CF_TEXT];  // what is to be confirmed, for the display
} confirm_slot;

void cf_init(confirm_slot *c);
// Opens a request. The same kind may ask again (new text, new timeout); false while another kind is waiting.
bool cf_request(confirm_slot *c, int kind, const char *l1, const char *l2, uint32_t timeout_ms, uint32_t now);
void cf_tick(confirm_slot *c, uint32_t now);         // CF_WAITING -> CF_EXPIRED once the time is up
bool cf_press(confirm_slot *c, uint32_t now);        // a press: true if it accepted an open request (it is used up)
int cf_state(const confirm_slot *c, int kind);       // CF_IDLE for a kind that did not ask
void cf_clear(confirm_slot *c, int kind);            // the asker read the result, or withdraws the request
uint32_t cf_left_ms(const confirm_slot *c, uint32_t now);  // 0 unless waiting

#ifdef __cplusplus
}
#endif
