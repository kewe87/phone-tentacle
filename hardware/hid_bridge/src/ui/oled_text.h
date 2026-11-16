#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The status OLED's texts in English and German (status_display.cpp). Its fonts (u8g2 *_tr) hold ASCII only, so
// German is written without umlauts (ae, oe, ue, ss). A line holds 21 characters of the 6x12 font; the setup QR
// screen's right half 12 of the 5x7 font (15 of the 4x6 one).
#define OLED_COLS 21
#define OLED_QR_COLS 12

enum {
  OT_CONFIRM,    // a request waits for BOOT
  OT_LEFT,       // "%lu s left"
  OT_PAIR_OPEN,  // the pairing window is open
  OT_PAIR_RUN,   // "On the computer run:" (then "tentacle pair")
  OT_QR_TITLE,   // setup QR screen: title
  OT_QR_PASS,    // setup QR screen: "Password:"
  OT_SETUP_JOIN, // setup without the QR code: join this network
  OT_PASS,       // "PW %s"
  OT_NET,        // "net %s %s": mode, address
  OT_NO_WIFI,    // the stored Wi-Fi is out of reach: hold BOOT 10 s to set up a new one
  OT_PEERS,      // "peers %d  cli %d"
  OT_STOPPED,
  OT_RUNNING,
  OT_BAT,        // "%s  bat %d%%": state, percent
  OT_COUNT
};

// id's text for the language (src/setup/board_opts.h: BO_LANG_DE is German, anything else English).
const char *oled_text(int lang, int id);
// A confirmation request's line (src/ui/confirm.h, written in English by whoever asks) in the language; text it
// does not know (a computer's name, "Code 123456", a network's name) comes back as it is.
const char *oled_request(int lang, const char *en);
// UTF-8 for the ASCII font: German umlauts and ß as ae oe ue ss (Ae Oe Ue), € as EUR, other non-ASCII and control
// characters as '?', a broken sequence as one '?'. Always NUL-terminated; returns the length written.
size_t oled_ascii(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
