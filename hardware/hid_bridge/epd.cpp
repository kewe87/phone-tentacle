// SSD1681 driver for the 1.54" panel, see epd.h. The panel's own waveforms (OTP) are used: full refresh 0xF7,
// partial 0xFC against the previous frame in RAM 0x26 (as GxEPD2's GxEPD2_154_D67 does).
#include "epd.h"
#include <SPI.h>
#include "board_hw.h"

static const uint32_t BUSY_FULL_MS = 4000, BUSY_PART_MS = 2000;
static const int EPD_FULL_EVERY = 30;  // partial refreshes in a row before a full one (ghosting)

static SPIClass spi(FSPI);
static uint8_t *pending, *drawing;  // in PSRAM: only this board needs them
static volatile bool havePending, pendingFull, busy;
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static bool poweredOn;  // the panel's analog part is on (after a partial refresh)

static void cmd(uint8_t c) {
  digitalWrite(WS_EPD_DC, LOW);
  digitalWrite(WS_EPD_CS, LOW);
  spi.transfer(c);
  digitalWrite(WS_EPD_CS, HIGH);
}

static void data(const uint8_t *d, size_t n, bool invert = false) {
  digitalWrite(WS_EPD_DC, HIGH);
  digitalWrite(WS_EPD_CS, LOW);
  for (size_t i = 0; i < n; i++) spi.transfer(invert ? (uint8_t)~d[i] : d[i]);
  digitalWrite(WS_EPD_CS, HIGH);
}

static void data1(uint8_t d) { data(&d, 1); }

static bool waitIdle(uint32_t max_ms) {  // BUSY is high while the panel works
  uint32_t t = millis();
  delay(1);
  while (digitalRead(WS_EPD_BUSY) == HIGH) {
    if (millis() - t > max_ms) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

static bool initPanel() {
  digitalWrite(WS_EPD_RST, LOW);
  delay(10);
  digitalWrite(WS_EPD_RST, HIGH);
  delay(10);
  if (!waitIdle(500)) return false;
  cmd(0x12);  // software reset
  if (!waitIdle(500)) return false;
  cmd(0x01);  // driver output: 200 gate lines
  const uint8_t drv[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8, 0x00};
  data(drv, 3);
  cmd(0x11);  // data entry: x and y increment
  data1(0x03);
  cmd(0x44);  // RAM x: bytes 0..24
  const uint8_t xr[] = {0x00, EPD_W / 8 - 1};
  data(xr, 2);
  cmd(0x45);  // RAM y: 0..199
  const uint8_t yr[] = {0x00, 0x00, (EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8};
  data(yr, 4);
  cmd(0x3C);  // border: follow LUT
  data1(0x05);
  cmd(0x18);  // internal temperature sensor
  data1(0x80);
  poweredOn = false;
  return waitIdle(500);
}

static void writeRam(uint8_t reg, const uint8_t *buf) {
  cmd(0x4E);
  data1(0x00);
  cmd(0x4F);
  const uint8_t y0[] = {0x00, 0x00};
  data(y0, 2);
  cmd(reg);
  data(buf, EPD_BYTES, true);  // u8g2: 1 = black; the panel: 1 = white
}

static void refresh(const uint8_t *buf, bool full) {
  if (full) {
    writeRam(0x26, buf);  // the "previous" frame too, so the next partial refresh compares against this one
    writeRam(0x24, buf);
    cmd(0x22);
    data1(0xF7);  // clock, analog on, load temperature + LUT (mode 1), display, analog + clock off
    cmd(0x20);
    waitIdle(BUSY_FULL_MS);
    poweredOn = false;
    return;
  }
  if (!poweredOn) {
    cmd(0x22);
    data1(0xE0);  // clock + analog on
    cmd(0x20);
    waitIdle(BUSY_PART_MS);
    poweredOn = true;
  }
  cmd(0x3C);  // border: keep (a partial refresh would flash it otherwise)
  data1(0x80);
  writeRam(0x24, buf);
  cmd(0x22);
  data1(0xFC);  // display with LUT mode 2 (partial), stay powered
  cmd(0x20);
  waitIdle(BUSY_PART_MS);
  writeRam(0x26, buf);  // now the previous frame for the next one
  cmd(0x3C);
  data1(0x05);
}

static void epdTask(void *) {
  int partials = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      bool full;
      xSemaphoreTake(lock, portMAX_DELAY);
      if (!havePending) {
        busy = false;
        xSemaphoreGive(lock);
        break;
      }
      memcpy(drawing, pending, EPD_BYTES);
      full = pendingFull;
      havePending = pendingFull = false;
      xSemaphoreGive(lock);
      if (partials >= EPD_FULL_EVERY) full = true;
      partials = full ? 0 : partials + 1;
      spi.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
      refresh(drawing, full);
      spi.endTransaction();
    }
  }
}

bool epd_begin() {
  pinMode(WS_EPD_CS, OUTPUT);
  digitalWrite(WS_EPD_CS, HIGH);
  pinMode(WS_EPD_DC, OUTPUT);
  pinMode(WS_EPD_RST, OUTPUT);
  digitalWrite(WS_EPD_RST, HIGH);
  pinMode(WS_EPD_BUSY, INPUT);
  delay(20);  // board_hw_begin() switched the panel's power on
  spi.begin(WS_EPD_SCK, -1, WS_EPD_MOSI, -1);
  spi.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  bool ok = initPanel();
  spi.endTransaction();
  if (!ok) return false;
  pending = (uint8_t *)heap_caps_malloc(EPD_BYTES, MALLOC_CAP_SPIRAM);
  drawing = (uint8_t *)heap_caps_malloc(EPD_BYTES, MALLOC_CAP_SPIRAM);
  if (!pending || !drawing) return false;
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(epdTask, "epd", 3072, NULL, 1, &task, 0);
  return true;
}

void epd_show(const uint8_t *buf, bool full) {
  if (!task) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(pending, buf, EPD_BYTES);
  havePending = true;
  pendingFull |= full;
  busy = true;
  xSemaphoreGive(lock);
  xTaskNotifyGive(task);
}

bool epd_busy() { return busy; }
