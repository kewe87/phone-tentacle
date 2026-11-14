// The CLI's link over Wi-Fi (docs/cli-protocol.md, "Wi-Fi link"): TCP port 7202, one computer at a time, the
// sealed frames in records [u16 LE length][frame]. A zero-length record is a keepalive (either way).
// Pairing happens here too, confirmed with the BOOT button; the frames go to cli_link.cpp.
// A connection without a session or an open pairing request is closed after 10 s.
#pragma once
#include <Arduino.h>

const uint16_t CLI_TCP_PORT = 7202;
const size_t CLI_TCP_MTU = 1460; // largest frame we send in one record (cli_link.cpp plans chunks with it)

bool cli_tcp_begin();                                            // listener + its task
bool cli_tcp_up();                                               // a computer is connected
bool cli_tcp_send(const uint8_t *p, size_t n, uint32_t timeout_ms);  // one record; false (and dropped) if it stalls;
                                                                 // n 0: a keepalive
void cli_tcp_drop();                                             // closes the connection
uint32_t cli_tcp_kbps();                                         // bytes out, last second
