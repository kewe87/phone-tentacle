// Firmware signature check, see ota_sig.h.
#include "ota_sig.h"
#include <string.h>
#include <sodium.h>

void ota_sig_message(const uint8_t sha[32], uint8_t out[OTA_SIG_MSG_LEN]) {
  memcpy(out, OTA_SIG_LABEL, OTA_SIG_LABEL_LEN);
  memcpy(out + OTA_SIG_LABEL_LEN, sha, 32);
}

const char *ota_sig_check(const ota_begin_req *r, const uint8_t pubkey[32], bool allow_unsigned) {
  if (!r->has_sig) return allow_unsigned ? NULL : "unsigned firmware: this board takes only signed Tentacle releases";
  if (sodium_init() < 0) return "no crypto";
  uint8_t msg[OTA_SIG_MSG_LEN];
  ota_sig_message(r->sha, msg);
  if (crypto_sign_verify_detached(r->sig, msg, sizeof msg, pubkey) != 0)
    return "bad signature: not signed with this board's release key";
  return NULL;
}
