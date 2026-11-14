// AirPlay audio to the computer, see audio_link.h. The mirror task queues frames, audioTask seals and sends them:
// a stalled send must not hold up AirPlay.
#include "audio_link.h"
#include <esp_heap_caps.h>
#include "freertos/message_buffer.h"
#include "cli_link.h"
#include "logbuf.h"
#include "src/airplay/airplay.h"
#include "src/video/gop.h"

const size_t AUDIO_QUEUE = 16 * 1024;  // about 1 s of AAC-ELD
static MessageBufferHandle_t audioQ;
static volatile uint32_t audioDrops;
static uint16_t seq;
static bool started;  // the computer saw this segment's VF_START

// One queued frame ([event][u32 ms][u32 ts][frame]) as a record; 0 if there is nothing to send for it.
static size_t audioRecord(const uint8_t *q, size_t n, uint8_t *rec) {
  int event = q[0];
  if (event == AIRPLAY_AUDIO_FRAME && !started) event = AIRPLAY_AUDIO_START;  // it joined mid-segment
  if (event == AIRPLAY_AUDIO_END && !started) return 0;
  rec[0] = (uint8_t)seq;
  rec[1] = (uint8_t)(seq >> 8);
  rec[2] = AUDIO_REC | (event == AIRPLAY_AUDIO_START ? VF_START : event == AIRPLAY_AUDIO_END ? VF_END : 0);
  size_t len = VCHUNK_HDR;
  if (event != AIRPLAY_AUDIO_END) {
    memcpy(rec + len, q + 1, n - 1);
    len += n - 1;
  }
  if (event == AIRPLAY_AUDIO_START) board_log("audio: segment to the computer (seq %u)", (unsigned)seq);
  if (event == AIRPLAY_AUDIO_END)
    board_log("audio: segment to the computer ended (seq %u, %lu dropped so far)", (unsigned)seq, (unsigned long)audioDrops);
  seq++;
  started = event != AIRPLAY_AUDIO_END;
  return len;
}

static void audioTask(void *) {
  static uint8_t q[1 + 8 + AUDIO_PAYLOAD], rec[VCHUNK_HDR + 8 + AUDIO_PAYLOAD];
  for (;;) {
    size_t n = xMessageBufferReceive(audioQ, q, sizeof q, portMAX_DELAY);
    if (!n) continue;
    if (!cli_audio_wanted()) {
      started = false;
      audioDrops++;
      continue;
    }
    size_t len = audioRecord(q, n, rec);
    if (len && !cli_send_audio(rec, len)) audioDrops++;
  }
}

// Started with the first audio that is wanted, so a board that never sends any keeps the internal RAM.
static bool audioStart() {
  static bool tried, ok;
  if (tried) return ok;
  tried = true;
  static StaticMessageBuffer_t ctl;
  uint8_t *store = (uint8_t *)heap_caps_malloc(AUDIO_QUEUE + 1, MALLOC_CAP_SPIRAM);
  audioQ = store ? xMessageBufferCreateStatic(AUDIO_QUEUE, store, &ctl) : NULL;
  ok = audioQ && xTaskCreatePinnedToCore(audioTask, "audio", 3072, NULL, 2, NULL, 1) == pdPASS;
  if (!ok) board_log("audio: task failed to start");
  return ok;
}

void audio_on_frame(int event, const uint8_t *frame, size_t len, uint32_t ts) {
  if (!cli_audio_wanted()) return;
  if (len > AUDIO_PAYLOAD - 8 || !audioStart()) { audioDrops++; return; }  // a record must fit one TCP record
  uint8_t q[1 + 8 + AUDIO_PAYLOAD];
  uint32_t ms = millis();
  q[0] = (uint8_t)event;
  for (int i = 0; i < 4; i++) q[1 + i] = (uint8_t)(ms >> (8 * i)), q[5 + i] = (uint8_t)(ts >> (8 * i));
  size_t n = 1 + 8;
  if (event != AIRPLAY_AUDIO_END) {
    memcpy(q + n, frame, len);
    n += len;
  }
  if (xMessageBufferSend(audioQ, q, n, 0) != n) audioDrops++;
}

uint32_t audio_drops() { return audioDrops; }
