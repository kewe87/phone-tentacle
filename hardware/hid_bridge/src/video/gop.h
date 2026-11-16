// GOP buffer (the access units since the latest IDR, kept as a ring) and the video chunker.
// Plain C with caller-provided memory so it can be unit-tested on the host.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// video chunk flags (docs/cli-protocol.md, "Video")
// VF_BEHIND (0.2.3): more units were waiting on the board behind this one when it went out (a replay, not the live edge)
enum { VF_START = 1, VF_END = 2, VF_IDR = 4, VF_CONFIG = 8, VF_BENCH = 16, VF_SNAP = 32, VF_BEHIND = 64 };
#define VCHUNK_HDR 3  // seq u16 LE + flags u8
#define GOP_CFG_MAX 512

// The chain of access units since the latest IDR, numbered from the IDR (0) on. When the arena or the AU list is
// full the oldest are dropped, never the newest. While AU 0 is kept the chain is whole: a decoder can start from it
// (a new viewer, a snap). Once it lost its start, a stream whose decoder already has the AUs before some number k
// goes on from k, as long as AU k is kept; a new viewer needs the next IDR. iOS sends an IDR at the start of
// mirroring and when the picture's format changes (a codec packet), so one chain may last the whole session.
typedef struct {
  uint32_t off, len, ts;
  uint32_t at;  // bytes of the chain before this AU (gop_bytes_from)
} gop_au;

typedef struct {
  uint8_t *arena;
  size_t cap, used;  // used: bytes of the AUs kept
  gop_au *aus;       // AU k at aus[k % max]
  int n, max;        // n AUs kept: first .. first + n - 1
  uint32_t first;    // the oldest AU kept; 0: the chain is whole
  uint32_t total;    // bytes since the IDR, the dropped ones too
  uint32_t idr_ts;   // AU 0's timestamp: names the chain for a stream that resumes
  uint8_t cfg[GOP_CFG_MAX];  // SPS+PPS, Annex-B
  size_t cfg_len;
  uint32_t cfg_ts;
  bool wait_idr;       // start, or an AU larger than the arena: drop until the next IDR
  uint32_t gen;        // bumps with every IDR (a new chain)
  uint32_t overflows;  // chains that lost their start, and AUs that did not fit at all
} gop_buf;

void gop_init(gop_buf *g, uint8_t *arena, size_t cap, gop_au *aus, int max);
void gop_set_config(gop_buf *g, const uint8_t *annexb, size_t len, uint32_t ts);
// Appends one Annex-B access unit; an IDR starts a new chain. Returns the AU's number in its chain, or -1 if it was
// dropped (no IDR yet, or larger than the whole arena).
int gop_push(gop_buf *g, const uint8_t *annexb, size_t len, bool idr, uint32_t ts);
static inline uint32_t gop_end(const gop_buf *g) { return g->first + (uint32_t)g->n; }  // the next AU's number
static inline bool gop_whole(const gop_buf *g) { return !g->wait_idr && g->n > 0 && g->first == 0; }
// AU k of the chain, NULL if it is not kept (dropped already, or not there yet). Its bytes: g->arena + au->off.
const gop_au *gop_at(const gop_buf *g, uint32_t k);
// Where a stream goes on whose decoder already has AUs 0 .. n-1 of the chain whose IDR is stamped idr_ts: n, or -1
// if that chain is gone (a newer IDR, a reboot), AU n was dropped already, or n is past the end.
int gop_resume_at(const gop_buf *g, uint32_t idr_ts, uint32_t n);
size_t gop_bytes_from(const gop_buf *g, uint32_t k);  // bytes of AU k and the AUs after it (all kept from first)
// True if the Annex-B data contains an IDR slice (NAL type 5).
bool annexb_has_idr(const uint8_t *p, size_t len);
// Rewrites AVCC (4-byte big-endian length prefixes) in place into Annex-B start codes.
// Returns false if the lengths do not tile the buffer exactly.
bool avcc_to_annexb(uint8_t *p, size_t len);

// Splits one access unit into notifies: [seq u16 LE][flags][payload], the START chunk
// carrying a u32 LE timestamp before the data.
typedef struct {
  const uint8_t *data;
  size_t len, pos;
  uint32_t ts;
  uint8_t flags;
  bool started;
} vchunk;

void vchunk_begin(vchunk *c, const uint8_t *data, size_t len, uint8_t flags, uint32_t ts);
// Writes the next chunk into out (room for VCHUNK_HDR + max_payload bytes); 0 when done.
size_t vchunk_next(vchunk *c, uint8_t *out, size_t max_payload, uint16_t seq);

#ifdef __cplusplus
}
#endif
