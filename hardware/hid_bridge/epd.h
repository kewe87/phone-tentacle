// The Waveshare ESP32-S3-ePaper-1.54's panel: 200x200, black/white, SSD1681 (board_hw.h). A refresh takes ~0.5 s
// (partial) to ~2 s (full) with the panel's BUSY line high; a task of its own does it, so loop() and the HID
// commands never wait for the panel.
#pragma once
#include <Arduino.h>

#define EPD_W 200
#define EPD_H 200
#define EPD_BYTES (EPD_W / 8 * EPD_H)  // 5000: rows of 25 bytes, MSB = left pixel, 1 = black (u8g2's layout)

bool epd_begin();  // starts the panel and its task; false if the panel does not answer (BUSY stuck)
// Shows buf (copied): only the newest frame waits, an older one not yet drawn is dropped. full: a full refresh
// (flashes, clears ghosting); otherwise a partial one. Every EPD_FULL_EVERY partial refreshes one is full anyway.
void epd_show(const uint8_t *buf, bool full);
bool epd_busy();  // a frame is being drawn or waits
