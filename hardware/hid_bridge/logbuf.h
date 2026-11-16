#pragma once
#include <Arduino.h>

// A board log line: printed to USB serial and kept in the RAM log the CLI reads with `logs`.
// Never log secrets (Wi-Fi password, pairing keys): the RAM log goes to anyone paired.
void board_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Only into the RAM log (for lines already printed elsewhere: the AirPlay receiver's).
void board_log_keep(const char *line);
// `logs <offset> [max chars]`: "ok <next> <end> <text>", '\n' escaped as \n; read until next == end.
String board_log_page(const String &arg);
// `logs follow` (debug builds, link.cpp): the escaped text from offset from on, at most max - 1 chars; returns
// where to read next (from itself when nothing new came). board_log_end(): where the next line will start.
uint32_t board_log_read(uint32_t from, char *out, size_t max);
uint32_t board_log_end();
