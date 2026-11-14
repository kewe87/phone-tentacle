// Host tests for pairing over Wi-Fi (src/cli/pair_gate.c): the PAIR frame's two forms, the code's text, the name
// for the display, and the gate (one request, never while the board is in use, a growing lock after a refusal).
// No crypto here: test_main.c checks the commit and the code against cli/testdata/vectors.json.
// Alone: gcc -DTEST_PAIR_MAIN -o test_pair test_pair.c ../hid_bridge/src/cli/pair_gate.c && ./test_pair
#include <stdio.h>
#include <string.h>
#include "../hid_bridge/src/cli/pair_gate.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_parse(void) {
  uint8_t body[48 + 32 + 40];
  for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i + 1);
  pair_req r;
  // the old form: key and id only
  CHECK(pair_parse(body, 48, &r) && r.c_pub == body && r.client_id == body + 32 && !r.commit && !r.name[0]);
  // with a commitment, without a name
  CHECK(pair_parse(body, 80, &r) && r.commit == body + 48 && !r.name[0]);
  // with a name: not terminated in the frame, terminated here
  memcpy(body + 80, "Kevins-PC", 9);
  CHECK(pair_parse(body, 89, &r) && r.commit == body + 48 && !strcmp(r.name, "Kevins-PC"));
  // the longest name
  memset(body + 80, 'x', 32);
  CHECK(pair_parse(body, 112, &r) && strlen(r.name) == 32);
  // lengths that are neither form
  CHECK(!pair_parse(body, 0, &r) && !pair_parse(body, 47, &r) && !pair_parse(body, 49, &r));
  CHECK(!pair_parse(body, 79, &r) && !pair_parse(body, 113, &r));
  // a NUL inside the name ends it
  memcpy(body + 80, "ab\0cd", 5);
  CHECK(pair_parse(body, 85, &r) && !strcmp(r.name, "ab"));
}

static void test_code(void) {
  char code[7];
  const uint8_t a[4] = {0x80, 0x20, 0x88, 0x02};  // the vector's HMAC start: 2149615618
  pair_code_text(a, code);
  CHECK(!strcmp(code, "615618"));
  const uint8_t zero[4] = {0, 0, 0, 0};
  pair_code_text(zero, code);
  CHECK(!strcmp(code, "000000"));  // always six digits
  const uint8_t small[4] = {0, 0, 0, 42};
  pair_code_text(small, code);
  CHECK(!strcmp(code, "000042"));
  const uint8_t max[4] = {0xff, 0xff, 0xff, 0xff};  // 4294967295
  pair_code_text(max, code);
  CHECK(!strcmp(code, "967295"));
}

static void test_name(void) {
  char out[22];
  pair_show_name("Kevins-PC", out, sizeof out);
  CHECK(!strcmp(out, "Kevins-PC"));
  pair_show_name("", out, sizeof out);
  CHECK(!strcmp(out, "computer"));
  pair_show_name(NULL, out, sizeof out);
  CHECK(!strcmp(out, "computer"));
  pair_show_name("B\xc3\xbcro\n\x1b[2J", out, sizeof out);  // UTF-8 and control bytes never reach the display
  CHECK(!strcmp(out, "B??ro??[2J"));
  pair_show_name("0123456789012345678901234567890", out, sizeof out);
  CHECK(strlen(out) == 21);
  char tiny[4];
  pair_show_name("", tiny, sizeof tiny);
  CHECK(!strcmp(tiny, "com"));
}

static void test_gate(void) {
  pair_gate g;
  uint32_t wait = 99, t = 1000;
  pg_init(&g);
  CHECK(pg_lock_left(&g, t) == 0 && !pg_is(&g, 2, PG_KEYED));

  // a session is up (or was a moment ago): refused at once, nothing opens, nothing is locked
  CHECK(pg_begin(&g, 2, true, t, &wait) == PG_IN_USE && g.state == PG_IDLE && pg_lock_left(&g, t) == 0);

  // the normal run: PAIR, nonce in time, BOOT
  CHECK(pg_begin(&g, 2, false, t, &wait) == PG_OK && pg_is(&g, 2, PG_KEYED) && wait == 0);
  CHECK(!pg_wait(&g, 1, t + 10));                 // not the link that asked
  CHECK(pg_wait(&g, 2, t + 100) && pg_is(&g, 2, PG_WAITING));
  CHECK(!pg_wait(&g, 2, t + 200));                // a second nonce is not a second request
  // only ONE open request: a second PAIR, from another link or the same, is refused and changes nothing
  CHECK(pg_begin(&g, 1, false, t + 300, &wait) == PG_OPEN && pg_is(&g, 2, PG_WAITING));
  CHECK(pg_begin(&g, 2, false, t + 300, &wait) == PG_OPEN && pg_is(&g, 2, PG_WAITING));
  CHECK(pg_begin(&g, 1, true, t + 300, &wait) == PG_OPEN);
  pg_end(&g, true, t + 5000);
  CHECK(g.state == PG_IDLE && pg_lock_left(&g, t + 5000) == 0 && g.fails == 0);
  CHECK(pg_begin(&g, 1, false, t + 5001, &wait) == PG_OK);  // paired: the next may ask at once

  // the nonce never comes
  CHECK(!pg_keyed_late(&g, t + 5001 + PAIR_KEYED_MS - 1) && pg_keyed_late(&g, t + 5001 + PAIR_KEYED_MS));
  CHECK(!pg_wait(&g, 1, t + 5001 + PAIR_KEYED_MS));  // too late
  t += 5001 + PAIR_KEYED_MS;
  pg_end(&g, false, t);
  // ... which locks: 5 s, then 10, 20, ... 300 at most
  CHECK(pg_begin(&g, 1, false, t + 1, &wait) == PG_LOCKED && wait == PAIR_LOCK_MIN_MS - 1 && g.state == PG_IDLE);
  CHECK(pg_begin(&g, 1, false, t + PAIR_LOCK_MIN_MS - 1, &wait) == PG_LOCKED && wait == 1);
  // in use outranks the lock in the answer, and neither opens anything
  CHECK(pg_begin(&g, 1, true, t + 1, &wait) == PG_IN_USE);
  uint32_t expect = PAIR_LOCK_MIN_MS;
  for (int i = 0; i < 9; i++) {
    t += expect;
    CHECK(pg_begin(&g, 1, false, t, &wait) == PG_OK);  // exactly when the lock ends
    CHECK(pg_wait(&g, 1, t));
    t += PAIR_CONFIRM_MS;
    pg_end(&g, false, t);  // not confirmed again
    expect = expect * 2 > PAIR_LOCK_MAX_MS ? PAIR_LOCK_MAX_MS : expect * 2;
    CHECK(pg_lock_left(&g, t) == expect);
    CHECK(pg_begin(&g, 2, false, t + expect - 1, &wait) == PG_LOCKED && wait == 1);
  }
  CHECK(expect == PAIR_LOCK_MAX_MS);
  // a refused request during the lock does not make it longer
  CHECK(pg_lock_left(&g, t + 1000) == expect - 1000);
  // pg_end without an open request is nothing
  pg_end(&g, false, t + 2000);
  CHECK(pg_lock_left(&g, t + 2000) == expect - 2000);

  // a long quiet time forgives: the lock starts small again
  t += expect;
  CHECK(pg_lock_left(&g, t) == 0);
  CHECK(pg_begin(&g, 1, false, t + PAIR_FORGIVE_MS, &wait) == PG_OK && g.fails == 0);
  pg_end(&g, false, t + PAIR_FORGIVE_MS + 10);
  CHECK(pg_lock_left(&g, t + PAIR_FORGIVE_MS + 10) == PAIR_LOCK_MIN_MS);

  // success clears the count
  pg_init(&g);
  CHECK(pg_begin(&g, 2, false, 0, &wait) == PG_OK);
  pg_end(&g, false, 10);
  CHECK(pg_begin(&g, 2, false, 10 + PAIR_LOCK_MIN_MS, &wait) == PG_OK);
  pg_end(&g, true, 20000);
  CHECK(pg_begin(&g, 2, false, 20001, &wait) == PG_OK);
  pg_end(&g, false, 20002);
  CHECK(pg_lock_left(&g, 20002) == PAIR_LOCK_MIN_MS);

  // across the millis() wrap
  pg_init(&g);
  t = 0xfffffff0u;
  CHECK(pg_begin(&g, 2, false, t, &wait) == PG_OK);
  CHECK(!pg_keyed_late(&g, t + 100) && pg_keyed_late(&g, t + PAIR_KEYED_MS));
  CHECK(pg_wait(&g, 2, t + 100));
  pg_end(&g, false, t + 200);  // wraps to 184
  CHECK(pg_lock_left(&g, t + 200 + 1000) == PAIR_LOCK_MIN_MS - 1000);
  CHECK(pg_begin(&g, 2, false, t + 200 + PAIR_LOCK_MIN_MS, &wait) == PG_OK);
}

int test_pair(void) {
  test_parse();
  test_code();
  test_name();
  test_gate();
  return fails;
}

#ifdef TEST_PAIR_MAIN
int main(void) {
  test_pair();
  printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
  return fails != 0;
}
#endif
