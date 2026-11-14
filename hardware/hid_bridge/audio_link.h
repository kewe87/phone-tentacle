// AirPlay audio for a computer that asked (`listen raw on`, docs/cli-protocol.md, "Phone audio"): records like video
// chunks, [seq u16 LE][flags][payload] with AUDIO_REC set in flags and their own seq. Payload: [u32 LE board ms]
// [u32 LE RTP ts][AAC-ELD frame]; the first frame of a segment carries VF_START, the segment's end is an empty
// VF_END record. They go out sealed on the session's stream 'A' (cli_link.h).
#pragma once
#include <Arduino.h>

const uint8_t AUDIO_REC = 0x80;
const size_t AUDIO_PAYLOAD = 1380;  // largest record payload: times + frame

void audio_on_frame(int event, const uint8_t *frame, size_t len, uint32_t ts);  // airplay_cfg.on_audio; never blocks
uint32_t audio_drops();  // records lost because the queue was full or the send failed
