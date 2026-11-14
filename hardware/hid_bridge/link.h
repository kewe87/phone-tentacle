// The queues between the computer's session (cli_link.cpp) and the rest of the firmware: commands in, replies and
// the status out. Callers never block on the network: one task (link.cpp) sends what is queued.
#pragma once
#include <Arduino.h>

// Where a command came from: the computer's session, or the USB serial port (debugging, recovery).
const char FROM_CLI = 'c', FROM_SERIAL = 's';

void link_begin();                                    // the queues and the sending task
void link_push_cmd(const char *line, size_t n);       // a command from the session (cli_link.cpp)
bool link_pop_cmd(String &line, uint32_t wait_ms);    // next command, oldest first (loop())
void link_reply(const String &line);                  // queued; dropped without a session
void link_status(const String &full, const String &compact);  // latest wins; compact if full is too long
void link_flush();                                    // waits (at most 1 s) until what is queued went out
