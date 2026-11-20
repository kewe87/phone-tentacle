// The transport stream muxer of the session recording (src/video/tsmux.c): packet framing, continuity counters, the
// tables' CRC, the payload comes out unchanged.
// Alone: gcc -DTEST_TSMUX_MAIN -o test_tsmux test_tsmux.c ../hid_bridge/src/video/tsmux.c && ./test_tsmux
// With a file: ./test_tsmux in.h264 out.ts (Annex-B in, one AU per access unit delimiter or slice start; then ffprobe)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hid_bridge/src/video/tsmux.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t outbuf[64 * 1024];
static size_t outlen;
static void collect(void *ctx, const uint8_t *p) {
  (void)ctx;
  if (outlen + TS_PACKET <= sizeof outbuf) memcpy(outbuf + outlen, p, TS_PACKET);
  outlen += TS_PACKET;
}

// The video PID's payload bytes, PES headers left in.
static size_t payload(uint16_t want, uint8_t *dst, int *cc_err, int *starts) {
  size_t n = 0;
  int last = -1;
  for (size_t o = 0; o + TS_PACKET <= outlen; o += TS_PACKET) {
    const uint8_t *p = outbuf + o;
    uint16_t pid = (uint16_t)((p[1] & 0x1F) << 8 | p[2]);
    if (pid != want) continue;
    int cc = p[3] & 0x0F;
    if (last >= 0 && cc != ((last + 1) & 0x0F)) (*cc_err)++;
    last = cc;
    if (p[1] & 0x40) (*starts)++;
    size_t start = 4;
    if (p[3] & 0x20) start += 1 + p[4];
    memcpy(dst + n, p + start, TS_PACKET - start);
    n += TS_PACKET - start;
  }
  return n;
}

int test_tsmux(void) {
  int before = fails;
  tsmux m;
  tsmux_init(&m, collect, NULL);
  // the CRC of a known PAT (ffmpeg's: TS id 1, program 1 on PID 0x1000)
  const uint8_t pat[] = {0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xF0, 0x00};
  CHECK(tsmux_crc32(pat, sizeof pat) == 0x2AB104B2u);

  static const uint8_t cfg[] = {0, 0, 0, 1, 0x67, 1, 2, 3, 0, 0, 0, 1, 0x68, 4, 5};
  static uint8_t au[1000];
  for (size_t i = 0; i < sizeof au; i++) au[i] = (uint8_t)(i * 7 + 1);
  au[0] = 0, au[1] = 0, au[2] = 0, au[3] = 1, au[4] = 0x65;

  CHECK(tsmux_au(&m, au, sizeof au, false, cfg, sizeof cfg, 100) == 0);  // no keyframe yet: nothing
  CHECK(outlen == 0);
  size_t n1 = tsmux_au(&m, au, sizeof au, true, cfg, sizeof cfg, 1000);
  au[4] = 0x41;
  size_t n2 = tsmux_au(&m, au, 5, false, cfg, sizeof cfg, 1033);  // a tiny AU: one packet, stuffed
  size_t n3 = tsmux_au(&m, au, 156, false, cfg, sizeof cfg, 1066);  // fills the first packet exactly: no stuffing
  for (int i = 0; i < 20; i++) tsmux_au(&m, au, 300 + i * 13, false, NULL, 0, 1100 + i * 33);
  CHECK(n1 == 2 + 6);  // PAT, PMT, 14 + 6 + 15 + 1000 bytes in 176 + 184 * 5
  CHECK(n2 == 1);
  CHECK(n3 == 1);  // (i == 3 below: 183 bytes in the second packet, a stuffing field of one byte)
  CHECK(outlen % TS_PACKET == 0);
  for (size_t o = 0; o < outlen; o += TS_PACKET) CHECK(outbuf[o] == 0x47);
  // PAT first, its CRC checks out
  CHECK(outbuf[1] == 0x40 && outbuf[2] == 0x00 && outbuf[4] == 0);
  CHECK(tsmux_crc32(outbuf + 5, 3 + 13) == 0);  // a section with its CRC sums to 0
  CHECK(tsmux_crc32(outbuf + TS_PACKET + 5, 3 + 18) == 0);
  // PCR in the first video packet, random access flag on the keyframe; PCR 0 at the first AU
  const uint8_t *v = outbuf + 2 * TS_PACKET;
  CHECK((v[3] & 0x30) == 0x30 && v[4] == 7 && v[5] == 0x50);
  CHECK(v[6] == 0 && v[7] == 0 && v[8] == 0 && v[9] == 0);

  static uint8_t pl[64 * 1024];
  int ccerr = 0, starts = 0;
  size_t n = payload(0x100, pl, &ccerr, &starts);
  CHECK(ccerr == 0);
  CHECK(starts == 23);
  // the first PES: header with PTS 9000 (100 ms lead), AUD, config, the AU byte for byte
  CHECK(pl[0] == 0 && pl[1] == 0 && pl[2] == 1 && pl[3] == 0xE0 && pl[7] == 0x80 && pl[8] == 5);
  CHECK(pl[9] == 0x21 && pl[10] == 0 && pl[11] == 0x01 && pl[12] == 0x46 && pl[13] == 0x51);  // 9000 = 0x2328
  CHECK(memcmp(pl + 14, "\0\0\0\1\x09\xF0", 6) == 0);
  CHECK(memcmp(pl + 20, cfg, sizeof cfg) == 0);
  au[4] = 0x65;
  CHECK(memcmp(pl + 20 + sizeof cfg, au, sizeof au) == 0);
  // the second PES right after: no config on a non-keyframe
  size_t p2 = 20 + sizeof cfg + sizeof au;
  CHECK(n > p2 + 25 && memcmp(pl + p2, "\0\0\1\xE0", 4) == 0);
  CHECK(memcmp(pl + p2 + 14, "\0\0\0\1\x09\xF0\0\0\0\1\x41", 11) == 0);
  return fails - before;
}

#ifdef TEST_TSMUX_MAIN
static FILE *fout;
static void tofile(void *ctx, const uint8_t *p) { (void)ctx; fwrite(p, 1, TS_PACKET, fout); }

// The NAL units of a raw stream (3- or 4-byte start codes), grouped into access units: a new one begins at an AUD, an
// SPS or a slice with first_mb_in_slice 0 (first slice byte >= 0x80) after a slice. SPS/PPS also go into the config,
// as the firmware gets them apart from the AUs.
static size_t next_sc(const uint8_t *b, size_t len, size_t i) {
  for (; i + 3 <= len; i++)
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) return i;
  return len;
}

int main(int argc, char **argv) {
  if (argc == 3) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static uint8_t b[32 << 20];
    size_t len = fread(b, 1, sizeof b, f);
    fclose(f);
    fout = fopen(argv[2], "wb");
    tsmux m;
    tsmux_init(&m, tofile, NULL);
    static uint8_t cfg[256];
    size_t cfglen = 0, start = 0, frame = 0;
    int idr = 0, seen_slice = 0;
    for (size_t sc = next_sc(b, len, 0); sc < len;) {
      size_t nal = sc + 3, nxt = next_sc(b, len, nal);
      size_t from = sc > 0 && b[sc - 1] == 0 ? sc - 1 : sc;  // a 4-byte start code
      size_t end = nxt < len && b[nxt - 1] == 0 ? nxt - 1 : nxt;
      int t = b[nal] & 0x1F;
      int slice = t == 1 || t == 5;
      if (seen_slice && (t == 7 || t == 9 || (slice && b[nal + 1] >= 0x80))) {
        tsmux_au(&m, b + start, from - start, idr, cfg, cfglen, (uint32_t)(frame++ * 1000 / 30));
        start = from;
        idr = seen_slice = 0;
      }
      if (t == 7) cfglen = 0;
      if ((t == 7 || t == 8) && cfglen + 4 + (end - nal) <= sizeof cfg) {
        memcpy(cfg + cfglen, "\0\0\0\1", 4);
        memcpy(cfg + cfglen + 4, b + nal, end - nal);
        cfglen += 4 + end - nal;
      }
      if (slice) seen_slice = 1;
      if (t == 5) idr = 1;
      sc = nxt;
    }
    if (seen_slice) tsmux_au(&m, b + start, len - start, idr, cfg, cfglen, (uint32_t)(frame++ * 1000 / 30));
    fclose(fout);
    printf("%zu access units\n", frame);
    return 0;
  }
  int f = test_tsmux();
  printf(f ? "%d FAILED\n" : "all tests passed\n", f);
  return f != 0;
}
#endif
