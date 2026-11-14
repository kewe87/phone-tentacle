// The BOOT button and the status LED. A small task samples the button every 10 ms, so a press counts even while
// loop() is busy with a long HID command.
//   short press: accepts an open confirmation (below); otherwise, in a SAFETY_CONTROLS build, toggles "stopped"
//   held 10 s:   button_take_long() turns true once (netcfg.cpp forgets the Wi-Fi and opens the setup hotspot)
// LED: quick blink while a confirmation waits, flicker while the button is held towards 10 s; otherwise solid =
// stopped, 0.5 Hz heartbeat = running (SAFETY_CONTROLS), or off (--no-safety).
#pragma once
#include <Arduino.h>
#include "src/ui/confirm.h"

void button_begin();
bool stop_active();        // all HID output is suppressed (always false without SAFETY_CONTROLS)
bool button_take_long();   // the button was held 10 s since the last call

// "Press BOOT to confirm": one request at a time, shown on the display (two lines of up to 21 characters) and by the
// LED. kind: CONFIRM_WIFI, CONFIRM_PAIR. Safe to call from any task.
//   confirm_request()  opens it; false while another kind is waiting. The same kind may ask again (restarts).
//   confirm_state()    CF_WAITING, then CF_ACCEPTED (pressed in time) or CF_EXPIRED; these two stay until
//                      confirm_clear(). CF_IDLE: this kind has no request.
//   confirm_clear()    the result was read, or the request is withdrawn.
bool confirm_request(int kind, const char *line1, const char *line2, uint32_t timeout_ms);
int confirm_state(int kind);
void confirm_clear(int kind);
// The waiting request for the display: false if none.
bool confirm_open(char line1[CF_TEXT], char line2[CF_TEXT], uint32_t *left_ms);
