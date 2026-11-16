// The phone's mouse: absolute (the default) or relative, as the setting `abs` says at boot (netcfg.h,
// cfg_abs_active()). One image for both: the same calls either way, plus moveTo() / glideTo() when absolute.
//
// Absolute: iOS maps X/Y straight to screen fractions while AssistiveTouch is on, so no homing or calibration. It
// ignores X/Y in a report that changes a button: move in one report, wait, then press (spikes/abs-touch/README.md).
// Relative: the classic mouse (buttons, X, Y, wheel, horizontal pan); the CLI calibrates it.
//
// Why both descriptors have one length: USB starts before setup() (CDC on boot), and the configuration descriptor
// carries the report descriptor's length then, before NVS can be read. The report descriptor itself is fetched
// later, when the phone asks (_onGetDescriptor(), on the USB task): only then is the setting read. So the shorter
// of the two gets redundant Usage Page (Generic Desktop) items in front, which change nothing. The absolute one,
// the one tested on the iPhone, is the longer and goes out byte for byte as the ABS_POINTER build sent it.
#pragma once
#include "USBHID.h"
#include "USBHIDMouse.h"  // MOUSE_LEFT and friends only: no USBHIDMouse is made
#include "netcfg.h"

class HidPointer : public USBHIDDevice {
public:
  static const uint16_t FULL = 32767;
  // Relative moves (`move`, `home`) in screen units per mouse unit when absolute, about what a relative mouse does.
  static const int MOVE_SCALE = 50;
  // A relative move longer than the screen ends at its edge anyway; bounding it keeps a wild `move` (a paced loop
  // of 8 ms reports) far under the 8 s watchdog.
  static const long MAX_MOVE = FULL / MOVE_SCALE + 1;

  HidPointer() { hid.addDevice(this, DESC_LEN); }  // must be global: USB starts before setup()
  void begin() { hid.begin(); }
  bool absolute() const { return cfg_abs_active(); }

  void moveTo(long x, long y) {
    x_ = constrain(x, 0, FULL);
    y_ = constrain(y, 0, FULL);
    sendAbs(0);
  }
  // Paced, for a drag: a jump is a flick to iOS, and the list flies. At a relative mouse's speed it scrolls as far
  // as it is dragged.
  // Bounded for the 8 s watchdog whatever the caller sends: the target is clamped to the screen, the step has a
  // floor (MIN_STEP: at most FULL / MIN_STEP + 1 = 513 reports, ~4.1 s at 8 ms) and the reports a cap (MAX_REPORTS:
  // a longer glide takes bigger steps). The default step (245) crosses the screen in 134 reports, ~1.1 s.
  static const int MIN_STEP = 64;
  static const long MAX_REPORTS = 250;  // ~2 s at 8 ms
  void glideTo(long x, long y, int step, int ms) {
    step = max(step, MIN_STEP);
    x = constrain(x, 0, FULL);
    y = constrain(y, 0, FULL);
    long x0 = x_, y0 = y_;
    long n = min(max(labs(x - x0), labs(y - y0)) / step + 1, MAX_REPORTS);
    for (long i = 1; i <= n; i++) {
      moveTo(x0 + (x - x0) * i / n, y0 + (y - y0) * i / n);
      delay(ms);
    }
  }
  void move(int dx, int dy, int8_t wheel = 0, int8_t pan = 0) {
    if (!absolute()) {
      sendRel(dx, dy, wheel, pan);
      return;
    }
    if (dx || dy) moveTo(x_ + dx * MOVE_SCALE, y_ + dy * MOVE_SCALE);
    if (wheel) sendAbs(wheel);  // the absolute mouse has no horizontal wheel
  }
  void press(uint8_t b) { buttons_ |= b; sendNone(); }
  void release(uint8_t b) { buttons_ &= ~b; sendNone(); }
  bool held() const { return buttons_; }

  uint16_t _onGetDescriptor(uint8_t *buffer) override {
    bool a = absolute();
    const uint8_t *d = a ? ABS_DESC : REL_DESC;
    size_t n = a ? sizeof ABS_DESC : sizeof REL_DESC, pad = DESC_LEN - n, i = 0;
    if (pad & 1) {  // a 3-byte Usage Page (0x0001), then 2-byte ones
      buffer[i++] = 0x06;
      buffer[i++] = 0x01;
      buffer[i++] = 0x00;
    }
    while (i < pad) {
      buffer[i++] = 0x05;
      buffer[i++] = 0x01;
    }
    memcpy(buffer + pad, d, n);
    return DESC_LEN;
  }

private:
  static const uint8_t ABS_ID = 10;                  // USBHID.h takes 1-6 for its own devices
  static const uint8_t REL_ID = HID_REPORT_ID_MOUSE;  // as the core's USBHIDMouse
  static constexpr uint8_t ABS_DESC[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,  // Generic Desktop, Mouse, Application
    0x85, ABS_ID,
    0x09, 0x01, 0xA1, 0x00,              //   Pointer, Physical
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,  //     Buttons 1-3
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x95, 0x05, 0x81, 0x03,              //     padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,  //     X, Y
    0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,  // Wheel
    0xC0, 0xC0,
  };
  // The fields of TinyUSB's TUD_HID_REPORT_DESC_MOUSE (the core's USBHIDMouse): buttons 1-5, X, Y, wheel, AC Pan.
  static constexpr uint8_t REL_DESC[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,  // Generic Desktop, Mouse, Application
    0x85, REL_ID,
    0x09, 0x01, 0xA1, 0x00,              //   Pointer, Physical
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,  //     Buttons 1-5
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,  //     padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,  // X, Y, Wheel: -127..127, relative
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,  // Consumer: AC Pan, the same
    0xC0, 0xC0,
  };
  static constexpr size_t LONGER = sizeof ABS_DESC > sizeof REL_DESC ? sizeof ABS_DESC : sizeof REL_DESC;
  static constexpr size_t DIFF = sizeof ABS_DESC > sizeof REL_DESC ? sizeof ABS_DESC - sizeof REL_DESC
                                                                     : sizeof REL_DESC - sizeof ABS_DESC;
  static constexpr size_t DESC_LEN = LONGER + (DIFF == 1 ? 2 : 0);  // one byte can not be padded: both get some
  static_assert(sizeof ABS_DESC >= sizeof REL_DESC, "the tested absolute descriptor should go out unpadded");

  void sendAbs(int8_t wheel) {
    uint8_t r[6] = {buttons_, (uint8_t)x_, (uint8_t)(x_ >> 8), (uint8_t)y_, (uint8_t)(y_ >> 8), (uint8_t)wheel};
    hid.SendReport(ABS_ID, r, sizeof r);
  }
  void sendRel(int dx, int dy, int8_t wheel, int8_t pan) {
    uint8_t r[5] = {buttons_, (uint8_t)(int8_t)constrain(dx, -127, 127), (uint8_t)(int8_t)constrain(dy, -127, 127),
                    (uint8_t)wheel, (uint8_t)pan};
    hid.SendReport(REL_ID, r, sizeof r);
  }
  void sendNone() {
    if (absolute()) sendAbs(0);
    else sendRel(0, 0, 0, 0);
  }

  USBHID hid;
  uint8_t buttons_ = 0;
  uint16_t x_ = 0, y_ = 0;
};
