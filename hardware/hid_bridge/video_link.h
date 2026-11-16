// Mirrored H.264 from AirPlay to the computer's session (cli_link.h): keeps the chain since the last IDR in PSRAM (a
// ring, src/video/gop.h) and sends it on `snap`, or every access unit while `stream on`. Also runs `bench` and
// computes the video stats. The web interface's page (web_server.cpp) gets the same stream on a task of its own.
#pragma once
#include <Arduino.h>
#include "src/web/web_ctl.h"

const size_t VIDEO_PAYLOAD = 1380;  // chunk payload per record: header, seal and this stay under one IP packet

bool video_begin();
void video_on_config(const uint8_t *sps_pps, size_t len);  // AirPlay callbacks (mirror task)
void video_on_au(const uint8_t *au, size_t len, bool idr);
void video_on_lost();  // AirPlay dropped a video packet for good (status "lost")
void video_stream(bool on);                       // `stream on|off`; off again when the session ends
// `stream on at IDR_TS N`: on, going on with AU N of the chain whose IDR is stamped IDR_TS (the computer's decoder
// kept AUs 0 .. N-1 across a reconnect). False: that chain is gone or AU N was dropped; the stream starts from the
// next whole chain as with `stream on`.
bool video_stream_at(uint32_t idrTs, uint32_t n);
void video_snap(const String &id);                // replies "<id> ok" when the chain is sent
void video_bench(const String &id, uint32_t kb);  // replies "<id> ok <ms>"

struct VideoStats {
  uint32_t kbps, idr_ms, gop_bytes;
  uint32_t age_ms;  // since the last frame from AirPlay (0: none yet); grows on a still screen too
  float fps;
  uint32_t overflows;  // times a chain lost its start (the buffer was full) since boot
  bool frozen;         // the chain lost its start and the stream is not on it: no new frame until the next IDR (#25)
  uint32_t lost;       // video packets dropped for good since boot: the picture may not decode until an IDR
  uint32_t backlog;    // bytes the computer's stream has still to send from the buffer (catching up; 0: live)
};
VideoStats video_stats();
String video_debug();              // `vdbg`: the chain's and the streams' state, for a frozen screen

// ---- The web interface's stream: the same chunks (src/video/gop.h, vchunk) to one page. Its own task and buffers
// (512 KB PSRAM), made on first use. room: payload it takes now (0: no page); send: one chunk; gen: bumps with every
// new page (it starts over with config + chain, unless it resumes). Like the computer's, it never skips ahead.
bool video_web_begin(size_t (*room)(), bool (*send)(const uint8_t *p, size_t n), uint32_t (*gen)(), size_t payload);
void video_web_stream(bool on);
// `stream on at IDR_TS N` from a page that kept its decoder across a reconnect (see video_stream_at).
bool video_web_stream_at(uint32_t idrTs, uint32_t n);
// What the page's picture waits for (src/web/web_ctl.h: VW_OK, VW_KEYFRAME, VW_FROZEN); waitedMs: since when.
int video_web_state(uint32_t *waitedMs);
