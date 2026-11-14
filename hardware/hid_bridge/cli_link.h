// The computer's link (esp32agent CLI, docs/cli-protocol.md): pairing, session keys and sealed frames. The link
// is Wi-Fi (cli_tcp.h, slot CLI_TCP_SLOT); the BLE characteristics (ble_link.cpp, slots 0-1) still carry sessions
// but no pairing, and nothing needs them.
#pragma once
#include <Arduino.h>

void cli_begin();                                         // loads the paired clients from NVS
void cli_on_frame(int slot, const uint8_t *p, size_t n);  // a frame (cli_tcp task; NimBLE host task)
void cli_reset(int slot);                                 // that peer disconnected
bool cli_authed(int slot);                                // its first sealed frame verified
bool cli_known(int slot);                                 // a paired computer's link (its session may be elsewhere now)
int cli_slot();                                           // the authenticated CLI's slot, -1 if none
uint32_t cli_gen();                                       // bumps when a session authenticates (video starts over)
bool cli_wifi();                                          // that session runs over Wi-Fi (cli_tcp.h)
uint16_t cli_mtu();                                       // its link's MTU (23 without one)
void cli_drop();                                          // drops the CLI's links
void cli_forget_all();                                    // unpair: every client must pair again
// Pairing (cli_link.cpp, "Pairing"): only over Wi-Fi, confirmed with the BOOT button.
void cli_pair_poll();                                     // cli_tcp's task, a few times a second: button, timeouts
bool cli_pair_open();                                     // a request is open (its link must stay up)
int cli_paired();                                         // paired computers (at most 4)
void cli_board_id(uint8_t out[6]);                        // the id the CLI knows the board by (BT MAC)
// Sealed board → CLI frames. Control (replies, status) only from the BLE TX task; video only from the video task:
// each stream's counter then grows in send order.
bool cli_send_ctrl(char kind, const char *text, size_t n, uint32_t timeout_ms);
bool cli_send_video(const uint8_t *chunk, size_t n);
// Phone audio records (wifi_video.h's audio record) for a CLI that asked (`listen raw on`), only from wifi_video's
// audio task and only over Wi-Fi: stream 'A', its own counter. Off again with every new session.
void cli_audio(bool on);
bool cli_audio_wanted();
bool cli_send_audio(const uint8_t *rec, size_t n);
size_t cli_video_room();                                  // payload bytes per video chunk (as ble_video_room); 0 = no CLI
