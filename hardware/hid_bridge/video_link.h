// Mirrored H.264 from AirPlay to the computer's session (cli_link.h): keeps the current GOP in PSRAM and sends it
// on `snap`, or every access unit while `stream on`. Also runs `bench` and computes the video stats.
#pragma once
#include <Arduino.h>

const size_t VIDEO_PAYLOAD = 1380;  // chunk payload per record: header, seal and this stay under one IP packet

bool video_begin();
void video_on_config(const uint8_t *sps_pps, size_t len);  // AirPlay callbacks (mirror task)
void video_on_au(const uint8_t *au, size_t len, bool idr);
void video_on_lost();  // AirPlay dropped a video packet for good (status "lost")
void video_stream(bool on);                       // `stream on|off`; off again when the session ends
// `stream on at IDR_TS N`: on, going on after the first N AUs of the GOP whose IDR is stamped IDR_TS (the computer's
// decoder kept them across a reconnect). False: that GOP is gone, the stream starts from the IDR as with `stream on`.
bool video_stream_at(uint32_t idrTs, uint32_t n);
void video_snap(const String &id);                // replies "<id> ok" when the GOP is sent
void video_bench(const String &id, uint32_t kb);  // replies "<id> ok <ms>"

struct VideoStats {
  uint32_t kbps, idr_ms, gop_bytes;
  uint32_t age_ms;  // since the last frame from AirPlay (0: none yet); grows on a still screen too
  float fps;
  uint32_t overflows;  // times the GOP filled up since boot
  bool frozen;         // GOP full and the stream not going on from it: no new frame until the next IDR (#25)
  uint32_t lost;       // video packets dropped for good since boot: the picture may not decode until an IDR
  uint32_t backlog;    // bytes the computer's stream has still to send from the GOP/tail (catching up; 0: live)
};
VideoStats video_stats();
String video_debug();              // `vdbg`: the tail and the stream's state, for a frozen screen
