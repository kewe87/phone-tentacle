// Fuzz + regression harness for the in-place binary-plist reader (src/bplist/bplist.c).
//
//   libFuzzer (CI):   clang -fsanitize=fuzzer,address,undefined bplist_fuzz.c bplist.c -o bplist_fuzz
//   portable repro:   cc -DFUZZ_STANDALONE bplist_fuzz.c bplist.c -o repro   (then ./repro [files...])
// (add -I<dir of bplist.h> to either; see the Makefile targets `fuzz` and `repro`.)
//
// The reader works in place on attacker bytes (AirPlay/RTSP bodies), so it must never read out of
// bounds for ANY input. The standalone build also runs two built-in regression inputs that must be
// rejected without crashing — one of them is the oversized-trailer case fixed in bplist.c.
#include "bplist.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Objects may reference each other (and themselves), so a plain depth cap still allows an
// exponential walk (an array of N self-references visits N^depth nodes). Cap the total work per input.
static long g_budget;

static void walk(const bp_reader *r, int ref, int depth) {
  if (depth > 32 || --g_budget < 0) return;  // defuse reference cycles and fan-out blow-up
  switch (bp_type(r, ref)) {
    case BP_UINT: { uint64_t u; bp_get_uint(r, ref, &u); break; }
    case BP_BOOL: { bool b; bp_get_bool(r, ref, &b); break; }
    case BP_REAL: { double d; bp_get_real(r, ref, &d); break; }
    case BP_DATA: { const uint8_t *p; size_t n; bp_get_data(r, ref, &p, &n); break; }
    case BP_STRING: { char buf[256]; bp_get_string(r, ref, buf, sizeof buf); break; }
    case BP_ARRAY: {
      int n = bp_count(r, ref);
      for (int i = 0; i < n && i < 4096; i++) walk(r, bp_array_get(r, ref, i), depth + 1);
      break;
    }
    case BP_DICT: {
      bp_count(r, ref);
      bp_dict_get(r, ref, "key");   // exercises key iteration + value read over the dict body
      bp_dict_get(r, ref, "");
      break;
    }
    default: break;
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bp_reader r;
  int top = bp_open(&r, data, size);
  g_budget = 100000;
  if (top >= 0) walk(&r, top, 0);
  return 0;
}

#ifdef FUZZ_STANDALONE
#include <stdio.h>
#include <stdlib.h>

static void run_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "skip %s (open failed)\n", path); return; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return; }
  uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  LLVMFuzzerTestOneInput(buf, got);
  free(buf);
}

int main(int argc, char **argv) {
  bp_reader rr;
  // 1) A valid bplist built by the writer must round-trip through the reader cleanly.
  bp_writer w;
  bp_writer_init(&w);
  int d = bp_w_dict(&w);
  bp_w_set(&w, d, "n", bp_w_uint(&w, 42));
  bp_w_set(&w, d, "s", bp_w_string(&w, "hello"));
  size_t vn = 0;
  uint8_t *v = bp_w_finish(&w, &vn);
  bp_writer_free(&w);
  if (v) { LLVMFuzzerTestOneInput(v, vn); free(v); }

  // 2) Oversized-trailer attack (regression for the bplist overflow fix): off/ref size 1, but
  //    count and offset-table offset set to 2^64-1. bp_open must reject it (return -1), not wrap
  //    its bounds checks and walk off the buffer.
  uint8_t bad[40];
  memset(bad, 0, sizeof bad);
  memcpy(bad, "bplist00", 8);
  uint8_t *t = bad + sizeof(bad) - 32;  // the 32-byte trailer
  t[6] = 1;                              // off_size
  t[7] = 1;                              // ref_size
  memset(t + 8, 0xff, 8);                // count = 2^64-1
  memset(t + 24, 0xff, 8);               // offset-table offset = 2^64-1
  if (bp_open(&rr, bad, sizeof bad) >= 0) { fprintf(stderr, "FAIL: oversized trailer accepted\n"); return 1; }
  LLVMFuzzerTestOneInput(bad, sizeof bad);  // and must not crash either way

  for (int i = 1; i < argc; i++) run_file(argv[i]);
  puts("ok: no crash; oversized trailer rejected");
  return 0;
}
#endif
