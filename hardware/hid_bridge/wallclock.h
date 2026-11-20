// The board's wall clock, for the session log's time stamps (sdlog.h). The board has no network time of its own:
// the computer sets it (`time EPOCH OFFSET`, sent by the CLI when its session starts, and by the web interface when
// its page opens). A board with an RTC (the Waveshare ESP32-S3-ePaper-1.54's PCF85063, board_hw.h) keeps it, with
// the offset in the RTC's RAM byte, and reads it back at boot.
#pragma once
#include <Arduino.h>

void clock_begin();  // setup(), after board_hw_begin(): the RTC's time, if it has a valid one
// Sets the clock: epoch = UTC seconds, offset = the computer's local time minus UTC in minutes (-720..840, a
// multiple of 15). src for the log ("cli", "web", "serial"). false: out of range (nothing changes).
bool clock_set(int64_t epoch, int offset_min, const char *src);
bool clock_valid();  // set since boot, or read from the RTC
// "2026-11-20 19:01:02.345" in the computer's local time; false (out untouched) while the clock is not set.
bool clock_local(char *out, size_t cap);
String clock_command(const String &arg, const char *src);  // `time [EPOCH [OFFSET]]`
