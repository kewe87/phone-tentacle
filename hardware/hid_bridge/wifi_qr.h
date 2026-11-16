// The setup hotspot as a Wi-Fi QR code (WIFI:T:WPA;S:<ssid>;P:<pass>;;): a phone's camera joins from it without
// typing the password. Made once when the hotspot starts (netcfg.cpp), then shown on the OLED and sent in the
// `qr` field of the wifi-setup line (netcfg.h). Uses the esp32 core's qrcode component (Nayuki's QR Code generator).
#pragma once
#include <Arduino.h>

bool wifi_qr_make(const String &ssid, const String &pass);  // false: too long for the code (then there is none)
int wifi_qr_size();                                         // modules per side, 0 = no code
bool wifi_qr_dark(int x, int y);
// One hex string per row, the first module in the top bit, rows separated by ',' ("" = no code).
String wifi_qr_hex();
