// The session recording on the microSD card (boards with a slot: the Waveshare ESP32-S3-ePaper-1.54, board_hw.h).
// Off unless the setting `sdlog` says log or video:
// - log: every board log line (logbuf.h) and every input the phone gets (drive() and the web interface: who sent
//   it, the command, the reply) goes into /tentacle/sNNNNN.log, a new file per boot. Typed text is never written,
//   only its length: it may be a password.
// - video: the log, and the mirrored screen as H.264 in an MPEG transport stream, /tentacle/sNNNNN.ts (plays in VLC,
//   ffmpeg, mpv; a cut-off file still plays). It holds everything the screen showed, typed passwords included as far
//   as the phone shows them. It starts at the chain's keyframe (video_link.h: possibly minutes back, when mirroring
//   was already running) and runs in real time. Over 3.9 GB (FAT32) it goes on in sNNNNN_2.ts, _3 ...: one stream,
//   cut; join the parts to play across (copy /b or cat).
// Small tasks write, so a slow card never holds up the HID commands or the mirroring; log lines that do not fit
// their queue are counted and noted as dropped.
#pragma once
#include <Arduino.h>

enum { SDLOG_NONE = 0, SDLOG_OFF, SDLOG_NOCARD, SDLOG_REC, SDLOG_ERR };

void sdlog_begin();        // setup(), after net_begin(): mounts the card if the board has a slot
void sdlog_reconfigure();  // the setting changed: waits (up to 2 s) until the writer took it up
int sdlog_state();         // SDLOG_*: NONE = no slot on this board
const char *sdlog_state_name();  // "none", "off", "nocard", "rec", "err"
const char *sdlog_file();        // the file being written: the .ts while recording the screen, else the .log ("": none)
bool sdlog_video();              // recording the screen (a .ts is open)
void sdlog_line(const char *line);  // any task, never blocks; ignored unless recording
void sdlog_input(char who, const char *line, const char *reply);  // drive(): one HID command
