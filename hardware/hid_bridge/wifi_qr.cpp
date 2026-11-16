// The setup hotspot as a Wi-Fi QR code, see wifi_qr.h.
#include "wifi_qr.h"
#include <qrcode.h>

// Version 6 (41 x 41) at the lowest error correction holds 134 bytes: the longest name (32) and password (63) with
// a few escapes. Tentacle-1A2B with its 12-character password needs version 3 (29 x 29).
static const int QR_MAX_VERSION = 6;
static const int QR_MAX = 17 + 4 * QR_MAX_VERSION;
static uint8_t bits[(QR_MAX * QR_MAX + 7) / 8];
static int size;

// esp_qrcode_generate() hands the code to a callback and frees it afterwards: keep a copy of the modules.
static void keep(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  if (n > QR_MAX) return;
  memset(bits, 0, sizeof bits);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(q, x, y)) bits[(y * n + x) / 8] |= 0x80 >> ((y * n + x) % 8);
  size = n;
}

// The Wi-Fi QR format escapes \ ; , : and " with a backslash.
static void escTo(String &o, const String &s) {
  for (char c : s) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
    o += c;
  }
}

bool wifi_qr_make(const String &ssid, const String &pass) {
  String text = "WIFI:T:WPA;S:";
  escTo(text, ssid);
  text += ";P:";
  escTo(text, pass);
  text += ";;";
  size = 0;
  esp_qrcode_config_t cfg = {};
  cfg.display_func = keep;
  cfg.max_qrcode_version = QR_MAX_VERSION;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;  // the biggest modules: the OLED is small
  return esp_qrcode_generate(&cfg, text.c_str()) == ESP_OK && size > 0;
}

int wifi_qr_size() { return size; }

bool wifi_qr_dark(int x, int y) {
  if (x < 0 || y < 0 || x >= size || y >= size) return false;
  return bits[(y * size + x) / 8] & (0x80 >> ((y * size + x) % 8));
}

String wifi_qr_hex() {
  String o;
  for (int y = 0; y < size; y++) {
    if (y) o += ',';
    for (int x = 0; x < size; x += 4) {
      int h = 0;
      for (int i = 0; i < 4; i++) h = h << 1 | wifi_qr_dark(x + i, y);
      o += "0123456789abcdef"[h];
    }
  }
  return o;
}
