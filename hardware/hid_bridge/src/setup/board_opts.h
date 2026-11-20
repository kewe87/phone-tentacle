#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The board's hardware settings (one release image for every board; they used to be build switches). netcfg.cpp
// keeps them in NVS, the session's `config` command, `tentacle device set` and the web interface change them.
// Plain C, so the host tests check what is taken.
//
//   display      auto | on | off   the status OLED: auto = used if it answers on I2C at boot
//   abs          on | off          absolute pointer (the USB report descriptor: takes effect after a restart)
//   battery      none | GPIO       LiPo voltage through a divider on an ADC1 pin (1-10, not the OLED's 5 and 6)
//   battery_div  1.00-10.00        that divider's ratio (two equal resistors: 2)
//   lang         auto | en | de    the OLED's texts; auto: the web interface's language if one is set, else en
//   sdlog        off | log | video the session recording on the microSD card (boards with a slot, board_hw.h;
//                                  sdlog.h): the log, or the log and the mirrored screen. "on" means video.

enum { BO_DISPLAY_AUTO = 0, BO_DISPLAY_ON = 1, BO_DISPLAY_OFF = 2 };
enum { BO_LANG_AUTO = 0, BO_LANG_EN = 1, BO_LANG_DE = 2 };
enum { BO_SD_OFF = 0, BO_SD_LOG = 1, BO_SD_VIDEO = 2 };
#define BO_BAT_NONE (-1)
#define BO_DIV_DEFAULT 200  // hundredths
#define BO_DIV_MIN 100
#define BO_DIV_MAX 1000
#define BO_OLED_SDA 5  // XIAO ESP32-S3 D4 / D5: the OLED's I2C pins (docs/hardware-addons.md)
#define BO_OLED_SCL 6

typedef struct {
  int display;  // BO_DISPLAY_*
  int abs;      // 1 on, 0 off
  int bat_pin;  // a GPIO, or BO_BAT_NONE
  int bat_div;  // hundredths
  int lang;     // BO_LANG_*
  int sdlog;    // BO_SD_*
} board_opts;

// The defaults: what a board without these keys in NVS does (OLED auto, absolute pointer, no battery, 2:1, auto,
// no SD log).
void bo_defaults(board_opts *o);

enum { BO_OK = 0, BO_UNKNOWN_KEY, BO_BAD_VALUE };
// Sets one key from text, after checking it; on BO_BAD_VALUE / BO_UNKNOWN_KEY nothing changes.
int bo_set(board_opts *o, const char *key, const char *value);
// What `key` takes, for an error message ("auto|on|off"); NULL for an unknown key.
const char *bo_values(const char *key);

bool bo_bat_pin_ok(int pin);  // an ADC1 pin (GPIO1-10) that is not the OLED's
const char *bo_display_name(int display);
const char *bo_lang_name(int lang);
// The value of one key as text ("auto", "on", "none", "7", "2.00"): out holds at least 8 bytes. false: unknown key.
bool bo_get(const board_opts *o, const char *key, char *out, size_t cap);
// The keys, in the order the JSON lists them; NULL-terminated.
extern const char *const BO_KEYS[];

// The OLED's language: lang, or for BO_LANG_AUTO the web interface's ("de", "en", "": none set) else English.
int bo_oled_lang(int lang, const char *web_lang);  // BO_LANG_EN or BO_LANG_DE

#ifdef __cplusplus
}
#endif
