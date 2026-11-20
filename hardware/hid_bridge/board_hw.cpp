// See board_hw.h.
#include "board_hw.h"
#include <Wire.h>
#include "esp_efuse.h"

static int hw = BOARD_GENERIC;
static int probe = -1;  // -1: not a PICO-1; else bit 0 RTC, bit 1 SHTC3 answered

static bool answers(TwoWire &w, uint8_t addr) {
  w.beginTransmission(addr);
  return w.endTransmission() == 0;
}

void board_hw_begin() {
  // ESP32-S3 package versions: 0 = the bare chip (QFN56, the XIAO's ESP32-S3R8), 1 = ESP32-S3-PICO-1 (LGA56).
  // Only a PICO-1 is probed further: on other boards GPIO47/48 are left alone.
  if (esp_efuse_get_pkg_ver() != 1) return;
  // The latch first: on battery the board would switch itself off as soon as PWR is let go.
  pinMode(WS_VBAT_PWR, OUTPUT);
  digitalWrite(WS_VBAT_PWR, HIGH);
  // The I2C parts (and their pull-ups) sit behind the switched rails: both on while asking.
  pinMode(WS_EPD_PWR, OUTPUT);
  digitalWrite(WS_EPD_PWR, LOW);
  pinMode(WS_AUDIO_PWR, OUTPUT);
  digitalWrite(WS_AUDIO_PWR, LOW);
  delay(20);
  Wire1.begin(WS_I2C_SDA, WS_I2C_SCL, 100000);
  Wire1.setTimeOut(20);
  bool rtc = answers(Wire1, 0x51), shtc3 = answers(Wire1, 0x70);
  Wire1.end();
  probe = (rtc ? 1 : 0) | (shtc3 ? 2 : 0);
  if (!rtc && !shtc3) {  // another PICO-1 board: leave its pins as they were
    pinMode(WS_VBAT_PWR, INPUT);
    pinMode(WS_EPD_PWR, INPUT);
    pinMode(WS_AUDIO_PWR, INPUT);
    return;
  }
  hw = BOARD_WS_EPD154;
  pinMode(WS_PA_EN, OUTPUT);  // the speaker amplifier off: nothing here plays sound
  digitalWrite(WS_PA_EN, LOW);
  // The panel's rail stays on (status_display.cpp drives it). The audio rail stays on as well: it may feed the
  // I2C pull-ups and the RTC; the speaker amplifier has its own enable.
}

int board_hw() { return hw; }
int board_hw_probe() { return probe; }
const char *board_hw_name() { return hw == BOARD_WS_EPD154 ? "waveshare-epd154" : "generic"; }
