#pragma once
// A minimal MPEG transport stream muxer for one H.264 stream (the session recording on the microSD card, sdlog.h):
// PAT + PMT before every keyframe, one PES per access unit with its PTS, the PCR on the video PID. Annex-B in, 188-byte
// packets out through a callback. Plain C, so the host tests check it.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_PACKET 188

typedef struct {
  uint8_t cc_pat, cc_pmt, cc_vid;  // continuity counters
  uint32_t t0_ms;                  // the first access unit's time: PTS 0
  bool started;
  void (*out)(void *ctx, const uint8_t *pkt);  // one packet of TS_PACKET bytes
  void *ctx;
} tsmux;

void tsmux_init(tsmux *m, void (*out)(void *ctx, const uint8_t *pkt), void *ctx);
// One access unit (Annex-B) at ms (milliseconds, any origin, wraps after 49 days). cfg (SPS + PPS, Annex-B) goes in
// front of a keyframe, with PAT and PMT. Returns the packets written.
size_t tsmux_au(tsmux *m, const uint8_t *au, size_t len, bool idr, const uint8_t *cfg, size_t cfg_len, uint32_t ms);
uint32_t tsmux_crc32(const uint8_t *p, size_t n);  // MPEG-2 CRC (for the tests)

#ifdef __cplusplus
}
#endif
