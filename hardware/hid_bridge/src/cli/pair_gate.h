// Pairing over Wi-Fi with the BOOT button (docs/cli-protocol.md, "Pairing"): the PAIR frame's layout, the 6-digit
// code's text, and the gate that lets one request at a time through. Plain C without crypto, so it is unit-tested
// on the host; the key exchange is cli_proto.c's, the button and the display are button.h's.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_PUB_LEN 32
#define PAIR_ID_LEN 16
#define PAIR_COMMIT_LEN 32
#define PAIR_NONCE_LEN 16
#define PAIR_NAME_MAX 32

// ---- The PAIR frame's body: c_pub[32] client_id[16], then (Wi-Fi) commit[32] name[0..32].
typedef struct {
  const uint8_t *c_pub, *client_id;
  const uint8_t *commit;          // NULL: the old 48-byte form (no code, no button: refused over Wi-Fi)
  char name[PAIR_NAME_MAX + 1];   // the computer's name as sent, "" if none
} pair_req;

bool pair_parse(const uint8_t *body, size_t n, pair_req *r);  // false: a length that is neither form
// The code both sides show: the first 4 bytes of HMAC-SHA256(K, "code" || n_c), big-endian, mod 1 000 000.
void pair_code_text(const uint8_t mac[4], char out[7]);
// The computer's name for the display, whose font is ASCII: other bytes become '?', "computer" if empty.
void pair_show_name(const char *name, char *out, size_t cap);

// ---- The gate. One request at a time, never while a paired computer uses the board, and after a request that
// ended without the button a lock that doubles (5 s ... 5 min), so a stranger on the Wi-Fi cannot keep the LED
// blinking and the button waiting. Times are millis(); they may wrap.
#define PAIR_KEYED_MS 5000      // PAIR_KEY sent: this long for the computer's nonce
#define PAIR_CONFIRM_MS 30000   // code shown: this long to press BOOT
#define PAIR_QUIET_MS 10000     // a session's last frame was this long ago, or pairing is refused (board in use)
#define PAIR_LOCK_MIN_MS 5000
#define PAIR_LOCK_MAX_MS 300000
#define PAIR_FORGIVE_MS 600000  // this long without a failed request: the lock starts small again

enum { PG_IDLE = 0, PG_KEYED, PG_WAITING };
enum { PG_OK = 0, PG_OPEN, PG_IN_USE, PG_LOCKED };  // pg_begin: why not

typedef struct {
  int state, owner;        // owner: the link that asked
  uint32_t at;             // when the state began
  uint8_t fails;           // requests in a row that ended without the button
  bool locked;
  uint32_t lock_at, lock_ms;
} pair_gate;

void pg_init(pair_gate *g);
// A PAIR frame from owner. in_use: a session is up, or was PAIR_QUIET_MS ago. PG_OK: the gate is owner's (PG_KEYED).
// PG_LOCKED: *wait_ms is how long still.
int pg_begin(pair_gate *g, int owner, bool in_use, uint32_t now, uint32_t *wait_ms);
bool pg_is(const pair_gate *g, int owner, int state);
bool pg_wait(pair_gate *g, int owner, uint32_t now);    // the nonce came in time: PG_KEYED -> PG_WAITING
bool pg_keyed_late(const pair_gate *g, uint32_t now);   // PG_KEYED for longer than PAIR_KEYED_MS
void pg_end(pair_gate *g, bool paired, uint32_t now);   // back to PG_IDLE; not paired: the lock starts
uint32_t pg_lock_left(pair_gate *g, uint32_t now);      // 0: not locked

#ifdef __cplusplus
}
#endif
