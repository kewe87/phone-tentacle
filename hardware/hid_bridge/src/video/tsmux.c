// See tsmux.h. ISO/IEC 13818-1: PAT on PID 0, PMT on PID 0x1000, H.264 (stream type 0x1B) and PCR on PID 0x100.
#include "tsmux.h"
#include <string.h>

#define PID_PMT 0x1000
#define PID_VID 0x0100
#define PCR_LEAD_90K 9000  // the PTS runs 100 ms ahead of the PCR: the decoder's buffer time

uint32_t tsmux_crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= (uint32_t)*p++ << 24;
    for (int i = 0; i < 8; i++) c = c & 0x80000000u ? (c << 1) ^ 0x04C11DB7u : c << 1;
  }
  return c;
}

void tsmux_init(tsmux *m, void (*out)(void *, const uint8_t *), void *ctx) {
  memset(m, 0, sizeof *m);
  m->out = out;
  m->ctx = ctx;
}

// A section (PAT or PMT) in one packet: header, pointer field, the section, its CRC, 0xFF to the end.
static void section(tsmux *m, uint16_t pid, uint8_t *cc, const uint8_t *sec, size_t n) {
  uint8_t p[TS_PACKET];
  memset(p, 0xFF, sizeof p);
  p[0] = 0x47;
  p[1] = 0x40 | (uint8_t)(pid >> 8);  // payload unit start
  p[2] = (uint8_t)pid;
  p[3] = 0x10 | (*cc & 0x0F);  // payload only
  *cc = (uint8_t)(*cc + 1);
  p[4] = 0;  // pointer field
  memcpy(p + 5, sec, n);
  uint32_t crc = tsmux_crc32(sec, n);
  p[5 + n] = (uint8_t)(crc >> 24);
  p[6 + n] = (uint8_t)(crc >> 16);
  p[7 + n] = (uint8_t)(crc >> 8);
  p[8 + n] = (uint8_t)crc;
  m->out(m->ctx, p);
}

static void tables(tsmux *m) {
  // section_length counts from after itself to the end of the CRC
  const uint8_t pat[] = {0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0x00, 0x00,  // table 0, length, TS id 1, version 0
                         0x00, 0x01, 0xE0 | (PID_PMT >> 8), PID_PMT & 0xFF};  // program 1 -> PMT
  section(m, 0, &m->cc_pat, pat, sizeof pat);
  const uint8_t pmt[] = {0x02, 0xB0, 18, 0x00, 0x01, 0xC1, 0x00, 0x00,  // table 2, length, program 1
                         0xE0 | (PID_VID >> 8), PID_VID & 0xFF,          // PCR PID
                         0xF0, 0x00,                                     // no program info
                         0x1B, 0xE0 | (PID_VID >> 8), PID_VID & 0xFF, 0xF0, 0x00};  // H.264 on PID_VID
  section(m, PID_PMT, &m->cc_pmt, pmt, sizeof pmt);
}

static void put_ts33(uint8_t *b, uint8_t prefix, uint64_t t) {  // a PTS: 5 bytes with marker bits
  b[0] = (uint8_t)(prefix << 4 | ((t >> 29) & 0x0E) | 1);
  b[1] = (uint8_t)(t >> 22);
  b[2] = (uint8_t)(((t >> 14) & 0xFE) | 1);
  b[3] = (uint8_t)(t >> 7);
  b[4] = (uint8_t)(((t << 1) & 0xFE) | 1);
}

// The PES (header + AUD + config + au) cut into packets. The PES is never built whole: its parts are walked.
typedef struct {
  const uint8_t *part[4];
  size_t len[4], i, off;
} parts;

static size_t take(parts *s, uint8_t *dst, size_t want) {
  size_t got = 0;
  while (got < want && s->i < 4) {
    size_t left = s->len[s->i] - s->off;
    if (!left) {
      s->i++;
      s->off = 0;
      continue;
    }
    size_t n = left < want - got ? left : want - got;
    memcpy(dst + got, s->part[s->i] + s->off, n);
    got += n;
    s->off += n;
  }
  return got;
}

size_t tsmux_au(tsmux *m, const uint8_t *au, size_t len, bool idr, const uint8_t *cfg, size_t cfg_len, uint32_t ms) {
  if (!m->started) {
    if (!idr) return 0;  // a recording starts on a keyframe
    m->started = true;
    m->t0_ms = ms;
  }
  size_t count = 0;
  if (idr) {
    tables(m);
    count += 2;
  }
  uint64_t pcr = (uint64_t)(uint32_t)(ms - m->t0_ms) * 90;
  uint64_t pts = pcr + PCR_LEAD_90K;
  uint8_t pes[14] = {0x00, 0x00, 0x01, 0xE0, 0x00, 0x00,  // video stream, length 0 (unbounded: allowed for video)
                     0x80, 0x80, 5};                       // PTS only, 5 header bytes
  put_ts33(pes + 9, 0x2, pts);
  static const uint8_t aud[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};  // access unit delimiter
  parts s = {{pes, aud, idr ? cfg : NULL, au}, {sizeof pes, sizeof aud, idr && cfg ? cfg_len : 0, len}, 0, 0};
  size_t total = s.len[0] + s.len[1] + s.len[2] + s.len[3];
  bool first = true;
  while (total) {
    uint8_t p[TS_PACKET];
    p[0] = 0x47;
    p[1] = (uint8_t)((first ? 0x40 : 0) | (PID_VID >> 8));
    p[2] = PID_VID & 0xFF;
    size_t af = 0;  // adaptation field bytes, its length byte included
    uint8_t afb[TS_PACKET];
    if (first) {  // the PCR (and the random access flag on a keyframe)
      afb[0] = 7;
      afb[1] = (uint8_t)(0x10 | (idr ? 0x40 : 0));
      afb[2] = (uint8_t)(pcr >> 25);
      afb[3] = (uint8_t)(pcr >> 17);
      afb[4] = (uint8_t)(pcr >> 9);
      afb[5] = (uint8_t)(pcr >> 1);
      afb[6] = (uint8_t)((pcr & 1) << 7 | 0x7E);
      afb[7] = 0;
      af = 8;
    }
    size_t room = TS_PACKET - 4 - af;
    if (total < room) {  // the last packet: stuffing in the adaptation field fills it up
      size_t pad = room - total;
      if (!af) {
        afb[0] = 0;
        af = 1;
        pad--;
        if (pad) {
          afb[1] = 0x00;  // no flags
          af = 2;
          pad--;
        }
      }
      memset(afb + af, 0xFF, pad);
      af += pad;
      afb[0] = (uint8_t)(af - 1);
      room = TS_PACKET - 4 - af;
    }
    p[3] = (uint8_t)((af ? 0x30 : 0x10) | (m->cc_vid & 0x0F));
    m->cc_vid = (uint8_t)(m->cc_vid + 1);
    memcpy(p + 4, afb, af);
    size_t got = take(&s, p + 4 + af, room);
    total -= got;
    m->out(m->ctx, p);
    count++;
    first = false;
  }
  return count;
}
