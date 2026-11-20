// See wallclock.h.
#include "wallclock.h"
#include <Wire.h>
#include <sys/time.h>
#include <time.h>
#include "board_hw.h"
#include "logbuf.h"

static const int64_t EPOCH_MIN = 1704067200;  // 2024-01-01: anything before is not a set clock
static const int64_t EPOCH_MAX = 4102444800;  // 2100-01-01: the RTC counts two-digit years
static const int64_t RTC_TRUST_MAX = 2208988800;  // 2040-01-01: an RTC beyond is taken as never set by us
static const uint8_t RTC_ADDR = 0x51;         // PCF85063
static volatile bool valid;
static volatile int offsetMin;

static uint8_t bcd(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static uint8_t unbcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

static bool rtcBus() {
  if (board_hw() != BOARD_WS_EPD154) return false;
  static bool begun;
  if (!begun) {
    Wire1.begin(WS_I2C_SDA, WS_I2C_SCL, 100000);
    Wire1.setTimeOut(20);
    begun = true;
  }
  return true;
}

// Registers 0x03 (RAM byte: the offset in quarter hours) .. 0x0A (years).
static bool rtcRead(uint8_t r[8]) {
  Wire1.beginTransmission(RTC_ADDR);
  Wire1.write(0x03);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom(RTC_ADDR, (uint8_t)8) != 8) return false;
  for (int i = 0; i < 8; i++) r[i] = Wire1.read();
  return true;
}

static bool rtcWrite(time_t utc, int offset_min) {
  struct tm t;
  gmtime_r(&utc, &t);
  Wire1.beginTransmission(RTC_ADDR);
  Wire1.write(0x03);
  Wire1.write((uint8_t)(int8_t)(offset_min / 15));
  Wire1.write(bcd(t.tm_sec));  // bit 7 (OS: the oscillator stopped) cleared with it
  Wire1.write(bcd(t.tm_min));
  Wire1.write(bcd(t.tm_hour));
  Wire1.write(bcd(t.tm_mday));
  Wire1.write(t.tm_wday);
  Wire1.write(bcd(t.tm_mon + 1));
  Wire1.write(bcd(t.tm_year % 100));
  return Wire1.endTransmission() == 0;
}

static void setSystem(int64_t epoch, int usec) {
  struct timeval tv = {(time_t)epoch, usec};
  settimeofday(&tv, NULL);
}

void clock_begin() {
  if (!rtcBus()) return;
  uint8_t r[8];
  if (!rtcRead(r)) {
    board_log("clock: the RTC does not answer");
    return;
  }
  if (r[1] & 0x80) {  // OS: never set, or its power was gone
    board_log("clock: not set (the computer sets it when it connects)");
    return;
  }
  struct tm t = {};
  t.tm_sec = unbcd(r[1] & 0x7F);
  t.tm_min = unbcd(r[2] & 0x7F);
  t.tm_hour = unbcd(r[3] & 0x3F);
  t.tm_mday = unbcd(r[4] & 0x3F);
  t.tm_mon = unbcd(r[6] & 0x1F) - 1;
  t.tm_year = unbcd(r[7]) + 100;
  int64_t epoch = (int64_t)mktime(&t);  // the system's TZ is UTC: mktime is timegm here
  if (epoch < EPOCH_MIN || epoch >= RTC_TRUST_MAX) {  // another firmware's leftover (one came with 2056)
    board_log("clock: the RTC's time is implausible (the computer sets it when it connects)");
    return;
  }
  offsetMin = (int8_t)r[0] * 15;
  setSystem(epoch, 0);
  valid = true;
  char now[32];
  clock_local(now, sizeof now);
  board_log("clock: %s from the RTC (UTC%+d min)", now, offsetMin);
}

bool clock_set(int64_t epoch, int offset_min, const char *src) {
  if (epoch < EPOCH_MIN || epoch >= EPOCH_MAX || offset_min < -720 || offset_min > 840 || offset_min % 15) return false;
  bool jump = !valid;
  if (valid) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t d = (int64_t)tv.tv_sec - epoch;
    jump = d > 2 || d < -2 || offset_min != offsetMin;
  }
  setSystem(epoch, 0);
  offsetMin = offset_min;
  valid = true;
  bool rtc = rtcBus() && rtcWrite((time_t)epoch, offset_min);
  if (jump) {  // only a real change is logged: every session and page sets it
    char now[32];
    clock_local(now, sizeof now);
    board_log("clock: %s (UTC%+d min), set by %s%s", now, offset_min, src, rtc ? ", kept in the RTC" : "");
  }
  return true;
}

bool clock_valid() { return valid; }

bool clock_local(char *out, size_t cap) {
  if (!valid) return false;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t local = tv.tv_sec + offsetMin * 60;
  struct tm t;
  gmtime_r(&local, &t);
  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
           t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
  return true;
}

String clock_command(const String &arg, const char *src) {
  String a = arg;
  a.trim();
  if (!a.length()) {
    if (!valid) return "ok unset";
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return "ok " + String((long long)tv.tv_sec) + " " + String((int)offsetMin);
  }
  long long epoch = 0;
  int offset = 0;
  if (sscanf(a.c_str(), "%lld %d", &epoch, &offset) < 1) return "err usage: time [EPOCH [OFFSET_MIN]]";
  if (!clock_set(epoch, offset, src)) return "err bad time: EPOCH 2024-2099 (UTC s), OFFSET_MIN -720..840 in 15s";
  return "ok";
}
