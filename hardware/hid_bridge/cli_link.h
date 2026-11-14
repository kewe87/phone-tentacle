// The computer's link (esp32agent CLI, docs/cli-protocol.md): pairing, session keys and sealed frames, over the one
// connection cli_tcp.h holds (Wi-Fi, TCP 7202). One computer at a time.
#pragma once
#include <Arduino.h>

void cli_begin();                                         // loads the paired clients from NVS
void cli_on_frame(const uint8_t *p, size_t n);            // a frame from the computer (cli_tcp's task)
void cli_reset();                                         // the connection closed
bool cli_up();                                            // a session is up: its first sealed frame verified
uint32_t cli_gen();                                       // bumps when a session authenticates (video starts over)
void cli_forget_all();                                    // unpair: every client must pair again
// Pairing (cli_link.cpp, "Pairing"): only while the pairing window is open (button.h: a double press of BOOT),
// and then confirmed with the BOOT button.
void cli_pair_poll();                                     // cli_tcp's task, a few times a second: button, timeouts
bool cli_pair_open();                                     // a request is open (its link must stay up)
bool cli_in_use();                                        // a session is up, or was 10 s ago: no pairing now
int cli_paired();                                         // paired computers (at most 4)
void cli_board_id(uint8_t out[6]);                        // the id the CLI knows the board by (as esp_read_mac ESP_MAC_BT gives it)
// Sealed board → CLI frames. Control (replies, status) only from link.cpp's TX task; video only from the video task:
// each stream's counter then grows in send order.
bool cli_send_ctrl(char kind, const char *text, size_t n, uint32_t timeout_ms);
bool cli_send_video(const uint8_t *chunk, size_t n);
// Phone audio records (audio_link.h) for a CLI that asked (`listen raw on`), only from audio_link's task: stream
// 'A', its own counter. Off again with every new session.
void cli_audio(bool on);
bool cli_audio_wanted();
bool cli_send_audio(const uint8_t *rec, size_t n);
size_t cli_video_room();                                  // payload bytes per video chunk; 0 = no session
