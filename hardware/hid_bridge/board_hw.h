// Which board the firmware runs on. One release image serves every board; the board is recognised at boot:
//   BOARD_GENERIC     Seeed XIAO ESP32-S3 or any other ESP32-S3 board (status OLED on I2C optional)
//   BOARD_WS_EPD154   Waveshare ESP32-S3-ePaper-1.54 (V2: ESP32-S3-PICO-1-N8R8): 1.54" e-paper 200x200 (SSD1681),
//                     microSD slot (SDMMC, 1 bit), power latch for its battery, RTC + SHTC3 on I2C 47/48.
// Recognised by the chip's package (PICO-1) and its RTC (0x51) or SHTC3 (0x70) answering on GPIO47/48, so a
// different PICO-1 board is not taken for it.
#pragma once
#include <Arduino.h>

enum { BOARD_GENERIC = 0, BOARD_WS_EPD154 = 1 };

// Waveshare ESP32-S3-ePaper-1.54 V2 pins (Waveshare's examples, user_config.h / sdcard_bsp.cpp).
#define WS_EPD_BUSY 8
#define WS_EPD_RST 9
#define WS_EPD_DC 10
#define WS_EPD_CS 11
#define WS_EPD_SCK 12
#define WS_EPD_MOSI 13
#define WS_EPD_PWR 6    // low: the panel is powered
#define WS_AUDIO_PWR 42 // low: the codec's rail is powered
#define WS_VBAT_PWR 17  // high: on battery the board keeps itself switched on
#define WS_PA_EN 46     // high: the speaker amplifier is on (kept low)
#define WS_I2C_SDA 47
#define WS_I2C_SCL 48
#define WS_SD_CLK 39
#define WS_SD_CMD 41
#define WS_SD_D0 40
#define WS_BAT_ADC 4    // battery through 2:1 (setting `battery 4`, `battery_div 2`)

void board_hw_begin();     // first thing in setup(): recognises the board and sets its power pins
int board_hw();            // BOARD_*
const char *board_hw_name();  // "generic" or "waveshare-epd154", for the log and the status
int board_hw_probe();         // for the boot log: -1 not a PICO-1, else bit 0 RTC, bit 1 SHTC3 answered
