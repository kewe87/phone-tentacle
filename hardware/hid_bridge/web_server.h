// The web interface (docs/manual.md, "Web interface"): https://<board>/ shows the mirrored phone and drives it
// with the mouse and the keyboard, plus the board's settings and a firmware update. HTTPS on port 443 with the
// board's own certificate (src/web/web_cert.h), one password (set on the Wi-Fi setup page, src/web/web_auth.h),
// one signed-in browser at a time; port 80 only points to it. Only while the board is a station in a Wi-Fi (the
// setup hotspot keeps its plain page on port 80).
//
// The page's input becomes the firmware's own HID commands (run() in hid_bridge.ino, src/web/web_input.h), queued
// here and run by loop() between the computer's commands. One drives at a time (src/web/web_ctl.h): while the page
// has control, the computer's HID commands get "err busy: ..."; the stop toggle (BOOT) stops the page's input too.
#pragma once
#include <Arduino.h>

void web_begin();                             // setup(), after net_begin() and video_begin(): the web task
bool web_loop();                              // loop(): runs one queued command of the page; true if it did
uint32_t web_wait_ms(uint32_t most);          // how long loop() may wait for the computer's next command
const char *web_cli_gate(const char *line);   // a computer's HID command: NULL lets it through, else the reply
const char *web_pw_request(const char *pw);  // `webpw PASSWORD` from a paired computer: stored once BOOT confirms it
bool web_in_use();                          // the page has control (button.cpp: BOOT is only the stop button)
