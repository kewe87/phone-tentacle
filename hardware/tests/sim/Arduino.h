// Host stand-ins for the Arduino bits cli_link.cpp uses (cli_sim.cpp).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
struct String : std::string {
  using std::string::string;
  String(const std::string &s) : std::string(s) {}
};
struct SerialStub {
  void println(const char *s) { fprintf(stderr, "%s\n", s); }
};
extern SerialStub Serial;
uint32_t millis();
// FreeRTOS: the sim is one thread, a lock is nothing.
typedef int SemaphoreHandle_t;
#define portMAX_DELAY 0
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return 1; }
inline void xSemaphoreTake(SemaphoreHandle_t, int) {}
inline void xSemaphoreGive(SemaphoreHandle_t) {}
