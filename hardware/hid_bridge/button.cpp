// BOOT button, status LED and the confirmation request, see button.h. Pins are overridable for non-XIAO boards.
#include "button.h"

#ifndef STOP_BTN_PIN
#define STOP_BTN_PIN 0  // BOOT button on XIAO ESP32-S3 / DevKitC (free as an input after boot)
#endif
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 21  // XIAO ESP32-S3 user LED; -1: no LED
#endif
#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW 1
#endif

const uint32_t HOLD_HINT_MS = 2000;  // held this long: the LED flickers until the 10 s are up

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;  // guards everything below
static btn_state btn;
static confirm_slot slot;
static bool stopped, stoppedBefore, toggled, longPending;

static void buttonTask(void *) {
  for (;;) {
    uint32_t now = millis();
    bool pressed = digitalRead(STOP_BTN_PIN) == LOW;
    portENTER_CRITICAL(&mux);
    int ev = btn_step(&btn, pressed, now);
    cf_tick(&slot, now);
    if (ev == BTN_PRESS) {
      toggled = false;
      if (!cf_press(&slot, now)) {  // a press that confirms never toggles the stop
#if SAFETY_CONTROLS
        stoppedBefore = stopped;
        stopped = !stopped;  // at once, not on release: it is the emergency stop
        toggled = true;
#endif
      }
    } else if (ev == BTN_LONG) {
      longPending = true;
      if (toggled) stopped = stoppedBefore;  // the long press is not also a stop toggle
    }
    bool waiting = slot.state == CF_WAITING;
    bool holding = btn.down && !btn.long_fired && btn_held_ms(&btn, now) >= HOLD_HINT_MS;
    bool stop = stopped;
    portEXIT_CRITICAL(&mux);

    if (STATUS_LED_PIN >= 0) {
      bool on;
      if (holding) on = (now / 50) & 1;
      else if (waiting) on = (now / 125) & 1;
      else if (SAFETY_CONTROLS) on = stop || ((now / 1000) & 1);
      else on = false;
      digitalWrite(STATUS_LED_PIN, (on ^ (bool)STATUS_LED_ACTIVE_LOW) ? HIGH : LOW);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void button_begin() {
  btn_init(&btn);
  cf_init(&slot);
  pinMode(STOP_BTN_PIN, INPUT_PULLUP);
  if (STATUS_LED_PIN >= 0) pinMode(STATUS_LED_PIN, OUTPUT);
  xTaskCreatePinnedToCore(buttonTask, "button", 2560, NULL, 2, NULL, 1);
}

bool stop_active() {
  portENTER_CRITICAL(&mux);
  bool s = stopped;
  portEXIT_CRITICAL(&mux);
  return s;
}

bool button_take_long() {
  portENTER_CRITICAL(&mux);
  bool l = longPending;
  longPending = false;
  portEXIT_CRITICAL(&mux);
  return l;
}

bool confirm_request(int kind, const char *line1, const char *line2, uint32_t timeout_ms) {
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  bool ok = cf_request(&slot, kind, line1, line2, timeout_ms, now);
  portEXIT_CRITICAL(&mux);
  return ok;
}

int confirm_state(int kind) {
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  cf_tick(&slot, now);
  int s = cf_state(&slot, kind);
  portEXIT_CRITICAL(&mux);
  return s;
}

void confirm_clear(int kind) {
  portENTER_CRITICAL(&mux);
  cf_clear(&slot, kind);
  portEXIT_CRITICAL(&mux);
}

bool confirm_open(char line1[CF_TEXT], char line2[CF_TEXT], uint32_t *left_ms) {
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  cf_tick(&slot, now);
  bool waiting = slot.state == CF_WAITING;
  if (waiting) {
    memcpy(line1, slot.l1, CF_TEXT);
    memcpy(line2, slot.l2, CF_TEXT);
    if (left_ms) *left_ms = cf_left_ms(&slot, now);
  }
  portEXIT_CRITICAL(&mux);
  return waiting;
}
