// The optional 128x64 SSD1306 status OLED on I2C (docs/hardware-addons.md) and the battery gauge. Part of every
// build since 0.4.0: the setting `display` (netcfg.h) says auto (used when something answers at 0x3C or 0x3D at
// boot), on or off. The texts follow the setting `lang` (src/ui/oled_text.h).
#pragma once
#include <Arduino.h>

void display_begin();         // setup(), after net_begin(): looks for the OLED (a few ms when none is there)
void display_poll();          // loop(): redraws ~2 times a second
void display_reconfigure();   // a setting changed (any task): loop() looks again
bool display_present();       // the OLED is in use
int battery_percent();        // -1 without a battery pin (setting `battery`)
