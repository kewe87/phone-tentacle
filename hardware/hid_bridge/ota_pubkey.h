// The public half of the key that signs Tentacle firmware (Ed25519, src/ota/ota_sig.h). A board takes a firmware
// update over Wi-Fi only when it is signed with the matching private key; a TENTACLE_DEBUG build takes unsigned
// images too. Made with `tentacle keygen DIR`; the private key never goes into this repository.
//
// This is a DEVELOPMENT key (2026-11-16). Replace it with the release key before the first signed GitHub release
// (the private key then lives only in the release workflow's secrets).
#pragma once
#include <stdint.h>

static const uint8_t OTA_RELEASE_PUBKEY[32] = {
    0xa8, 0x23, 0xce, 0xff, 0x12, 0xc4, 0x59, 0x59,
    0x80, 0x62, 0x65, 0x67, 0xb3, 0x3f, 0x7e, 0xa6,
    0x45, 0xfa, 0x2e, 0x66, 0xc2, 0x88, 0x50, 0x6a,
    0x47, 0x69, 0x7b, 0xa1, 0x03, 0x78, 0x98, 0x5b};
