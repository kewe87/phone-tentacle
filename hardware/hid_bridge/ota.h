// Firmware update over the computer's session (docs/cli-protocol.md, "Firmware update"): `update begin|state|end|
// abort` commands, the image in 'u' records on stream C, written to the other OTA slot with esp_ota_*. Only images
// signed with the release key (ota_pubkey.h, src/ota/ota_sig.h) are taken; a TENTACLE_DEBUG build takes unsigned
// ones too. Only a paired computer's session can send one. After the restart the new firmware is on trial: kept
// once a session came up or after 30 s; a restart before that boots the previous one again (the bootloader's
// rollback, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in the Arduino core's bootloader).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef ARDUINO
#include <Arduino.h>
void ota_setup();                                    // setup(): the lock, and whether this boot is on trial
String ota_command(const String &arg, const String &id);  // loop(): `update ...` from the session; "" when it
                                                     // replied itself (end: then it restarts the board)
void ota_poll();                                     // loop(): deadlines, the session, the trial
String ota_json();                                   // the state for `dbg state`
#endif

// Called by cli_link.cpp on cli_tcp's task (the simulated board has its own).
void ota_chunk(const uint8_t *p, size_t n);          // one 'u' record after its kind byte: offset u32 LE, bytes
void ota_link_down();                                // the computer's link closed: a running update stops
