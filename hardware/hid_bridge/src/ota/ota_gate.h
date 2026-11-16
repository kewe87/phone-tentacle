// Firmware update over the CLI's session (docs/cli-protocol.md, "Firmware update"), as a plain state machine: what
// `update begin` asks for, the order of the image's chunks, the deadlines. The flash writes, the SHA-256 and the
// signature check are the caller's (ota.cpp, ota_sig.h). Times are millis(); they may wrap.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_CHUNK_MAX 4096        // image bytes per 'u' record (the CLI sends this much, the last one less)
#define OTA_STALL_MS 15000        // a transfer with no chunk this long is given up
#define OTA_VERSION_MAX 24        // the version the CLI names for the display

enum { OG_IDLE = 0, OG_RECEIVING, OG_DONE };

typedef struct {
  int state;
  uint32_t size, got;     // the image's size, bytes written so far
  uint32_t since;         // when the state began or the last chunk came
  uint8_t sha[32];        // what the CLI says the image's SHA-256 is (and what the signature covers)
  char version[OTA_VERSION_MAX];  // printable ASCII, for the display and the log
  char why[64];           // why the last update ended without success ("" if none did)
} ota_gate;

typedef struct {
  uint32_t size;
  uint8_t sha[32];
  bool has_sig;           // false: the CLI sent "-" (an unsigned image)
  uint8_t sig[64];        // Ed25519, see ota_sig.h
  char version[OTA_VERSION_MAX];
} ota_begin_req;

void og_init(ota_gate *g);
// "SIZE SHA256HEX SIG [VERSION]" (the arguments of `update begin`); SIG is 128 hex digits or "-" (none). False on
// anything else.
bool og_parse_begin(const char *arg, ota_begin_req *r);
// Starts an update of r->size bytes (at most max_size); its signature was checked already (ota_sig.h). NULL:
// started; else why not (another update runs, too big, ...). A new begin clears the last failure.
const char *og_begin(ota_gate *g, const ota_begin_req *r, uint32_t max_size, uint32_t now);
// One chunk at offset off of n bytes: true if it is the next one and fits. False: the update failed (g->why), the
// caller aborts the flash write. Chunks while nothing is being received are refused too.
bool og_chunk(ota_gate *g, uint32_t off, size_t n, uint32_t now);
void og_written(ota_gate *g, size_t n);                // the chunk og_chunk() allowed is in flash
bool og_complete(const ota_gate *g);                   // every byte is in
bool og_stalled(const ota_gate *g, uint32_t now);      // OG_RECEIVING and no chunk for OTA_STALL_MS
void og_fail(ota_gate *g, const char *why);            // back to OG_IDLE, why kept for `update state`
void og_done(ota_gate *g);                             // verified, boot partition set: the board restarts
bool og_busy(const ota_gate *g);                       // OG_RECEIVING
// For the display: version as printable ASCII (other bytes '?'), at most OTA_VERSION_MAX - 1.
void og_clean_version(const char *in, char out[OTA_VERSION_MAX]);

#ifdef __cplusplus
}
#endif
