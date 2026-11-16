// Which firmware images the board takes over the network (docs/cli-protocol.md, "Firmware update"): those signed
// with the release key, Ed25519 (libsodium, as the AirPlay pairing uses it). A TENTACLE_DEBUG build takes unsigned
// ones too; it is never handed out.
//   signed message = "tentacle-fw-v1" || SHA-256(image)       (14 + 32 bytes)
//   signature      = Ed25519(release key, signed message)      (64 bytes, `tentacle sign`: FILE.bin.sig)
// `update begin` announces the image's SHA-256 and the signature; the board checks the signature before it takes a
// byte, and that the image it got has that SHA-256 before it switches the boot partition.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ota_gate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SIG_LABEL "tentacle-fw-v1"
#define OTA_SIG_LABEL_LEN 14
#define OTA_SIG_MSG_LEN (OTA_SIG_LABEL_LEN + 32)

void ota_sig_message(const uint8_t sha[32], uint8_t out[OTA_SIG_MSG_LEN]);
// NULL: take the image (a valid signature by pubkey; or none at all and allow_unsigned). Else why not. A signature
// that is there but wrong is refused also when unsigned images are allowed.
const char *ota_sig_check(const ota_begin_req *r, const uint8_t pubkey[32], bool allow_unsigned);

#ifdef __cplusplus
}
#endif
