// The public half of the key that signs Tentacle firmware (Ed25519, src/ota/ota_sig.h). A board takes a firmware
// update over Wi-Fi only when it is signed with the matching private key; a TENTACLE_DEBUG build takes unsigned
// images too. Made with `tentacle keygen DIR`; the private key never goes into this repository.
//
// The release key (2026-11-25; it replaced the development key of 2026-11-16). Its private half is the release
// workflow's secret TENTACLE_SIGNING_KEY (CONTRIBUTING.md, "Releasing").
#pragma once
#include <stdint.h>

static const uint8_t OTA_RELEASE_PUBKEY[32] = {
    0x8c, 0xe6, 0x21, 0x6b, 0x55, 0x4c, 0x38, 0xf3,
    0x30, 0x13, 0xaa, 0xad, 0x5a, 0x94, 0x4d, 0x59,
    0x94, 0x83, 0xa5, 0xd8, 0x23, 0x01, 0x6d, 0x16,
    0x9f, 0x04, 0xf5, 0x6f, 0xe8, 0xce, 0x84, 0x36};
