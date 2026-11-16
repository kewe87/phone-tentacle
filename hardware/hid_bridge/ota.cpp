// Firmware update over the computer's session, see ota.h. The image comes in on cli_tcp's task (ota_chunk), the
// commands and the BOOT press on loop(); one lock keeps them apart.
#include "ota.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include "button.h"
#include "cli_link.h"
#include "link.h"
#include "logbuf.h"
#include "version.h"
#include "src/ota/ota_gate.h"

// The version in a form the CLI finds in an image file (`tentacle update` shows old -> new and refuses a file
// without it). Referenced below, so the linker keeps it.
extern "C" const char tentacle_fw_marker[] = FW_MARKER;

const uint32_t TRIAL_MS = 30000;  // a new firmware that ran this long without a restart is kept

static SemaphoreHandle_t lock;
static ota_gate gate;
static esp_ota_handle_t handle;           // 0: no flash write open
static const esp_partition_t *target;
static mbedtls_sha256_context sha;
static bool shaOn;
static bool onTrial;                      // this boot runs an image that is not marked good yet
static const char *rolledBack;            // the slot of an update that did not start properly ("" if none)
static uint8_t lastQuarter;

// Arduino's initArduino() would mark a new image good before setup(); it is marked in ota_poll() instead, once it
// has shown that it works.
extern "C" bool verifyRollbackLater() { return true; }

// TODO(Phase U, signed releases): an image signed with the release key (Ed25519 over the SHA-256 that `update
// begin` announces, checked against a public key built into the firmware) may skip the BOOT press; a GitHub
// release would carry the signature and the CLI send it with `update begin`. Since the SHA-256 is checked against
// the image in `update end`, checking the signature over it at begin is enough. Until then every update in a
// release build needs the press.
static bool releaseSigned(const ota_begin_req &) { return false; }

void ota_setup() {
  lock = xSemaphoreCreateMutex();
  og_init(&gate);
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  rolledBack = bad ? bad->label : "";
  esp_ota_img_states_t st;
  onTrial = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  if (onTrial)
    board_log("ota: new firmware %s in %s on trial: kept once a session comes up or after 30 s, a restart before that "
              "goes back to the previous one", FW_VERSION, run->label);
  else
    board_log("ota: %s in %s%s%s", tentacle_fw_marker, run->label, bad ? ", the update in " : "",
              bad ? (String(bad->label) + " did not start properly and was given up").c_str() : "");
}

static void keep(const char *why) {
  if (!onTrial) return;
  esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
  onTrial = false;
  if (e == ESP_OK) board_log("ota: firmware %s kept (%s)", FW_VERSION, why);
  else board_log("ota: marking firmware %s good failed (%s)", FW_VERSION, esp_err_to_name(e));
}

// Everything below holds the lock.
static void cleanUp() {
  if (handle) esp_ota_abort(handle);
  handle = 0;
  if (shaOn) mbedtls_sha256_free(&sha);
  shaOn = false;
  confirm_clear(CONFIRM_UPDATE);  // ours alone: nothing else asks for this kind
}

static void stop(const char *why) {
  bool was = og_busy(&gate);
  og_fail(&gate, why);
  cleanUp();
  if (was) board_log("ota: update stopped: %s (still running %s)", why, FW_VERSION);
}

static const char *startWrite() {
  if (!target) return "no update partition";
  esp_err_t e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);  // erases as it goes
  if (e != ESP_OK) {
    handle = 0;
    return esp_err_to_name(e);
  }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  shaOn = true;
  lastQuarter = 0;
  board_log("ota: receiving %lu bytes of firmware %s into %s", (unsigned long)gate.size, gate.version, target->label);
  return nullptr;
}

static String begin(const String &arg) {
  ota_begin_req r;
  if (!og_parse_begin(arg.c_str(), &r)) return "err usage: update begin SIZE SHA256HEX [VERSION]";
  keep("an update was asked by the session");  // a running image still on trial cannot start an update
  target = esp_ota_get_next_update_partition(nullptr);
  bool press = !TENTACLE_DEBUG && !releaseSigned(r);
  const char *why = og_begin(&gate, &r, target ? target->size : 0, press, millis());
  if (why) return String("err ") + why;
  if (press) {
    char l2[CF_TEXT];
    snprintf(l2, sizeof l2, "%s -> %s", FW_VERSION, gate.version[0] ? gate.version : "?");
    if (!confirm_request(CONFIRM_UPDATE, "Firmware update", l2, OTA_CONFIRM_MS)) {
      og_fail(&gate, "busy: the board waits for another confirmation");
      return "err busy: the board waits for another confirmation (pairing or Wi-Fi setup)";
    }
    board_log("ota: firmware %s (%lu bytes) offered: waiting for BOOT", gate.version, (unsigned long)gate.size);
    return "ok confirm " + String(OTA_CONFIRM_MS / 1000);
  }
  if ((why = startWrite())) {
    stop(why);
    return String("err ") + why;
  }
  return "ok ready";
}

static String state() {
  switch (gate.state) {
    case OG_CONFIRM: {
      uint32_t gone = millis() - gate.since;
      return "ok confirm " + String(gone >= OTA_CONFIRM_MS ? 0 : (OTA_CONFIRM_MS - gone + 999) / 1000);
    }
    case OG_RECEIVING: return "ok ready " + String(gate.got) + " " + String(gate.size);
    case OG_DONE: return "ok done";
  }
  if (gate.why[0]) return String("err ") + gate.why;
  return "ok idle";
}

// Every byte is in: check it, make it the boot image. NULL on success.
static const char *finish() {
  if (gate.state != OG_RECEIVING) return gate.why[0] ? gate.why : "no update running";
  if (!og_complete(&gate)) {
    static char why[64];
    snprintf(why, sizeof why, "image incomplete (%lu of %lu bytes)", (unsigned long)gate.got, (unsigned long)gate.size);
    stop(why);
    return gate.why;
  }
  uint8_t sum[32];
  mbedtls_sha256_finish(&sha, sum);
  if (memcmp(sum, gate.sha, sizeof sum)) {
    stop("SHA-256 does not match: the image was damaged on the way");
    return gate.why;
  }
  esp_err_t e = esp_ota_end(handle);  // checks the image: header, chip, segments, its own checksum and hash
  handle = 0;
  if (e != ESP_OK) {
    stop(e == ESP_ERR_OTA_VALIDATE_FAILED ? "not a valid ESP32-S3 firmware image" : esp_err_to_name(e));
    return gate.why;
  }
  if ((e = esp_ota_set_boot_partition(target)) != ESP_OK) {
    stop(esp_err_to_name(e));
    return gate.why;
  }
  mbedtls_sha256_free(&sha);
  shaOn = false;
  og_done(&gate);
  board_log("ota: firmware %s verified in %s, restarting into it", gate.version, target->label);
  return nullptr;
}

String ota_command(const String &arg, const String &id) {
  String reply;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (arg.startsWith("begin ")) reply = begin(arg.substring(6));
  else if (arg == "state") reply = state();
  else if (arg == "abort") {
    if (og_busy(&gate)) stop("stopped by the computer");
    reply = "ok";
  } else if (arg == "end") {
    const char *why = finish();
    reply = why ? String("err ") + why : String("ok restarting into ") + gate.version;
  } else reply = "err usage: update begin SIZE SHA256HEX [VERSION] | state | end | abort";
  bool restart = gate.state == OG_DONE && arg == "end";
  xSemaphoreGive(lock);
  if (!restart) return reply;
  link_reply(id + " " + reply);
  link_flush();                    // let the reply go out
  vTaskDelay(pdMS_TO_TICKS(300));  // ... and onto the network before the connection closes
  ESP.restart();
  return "";
}

void ota_poll() {
  if (onTrial && (cli_up() || millis() >= TRIAL_MS)) keep(cli_up() ? "a session came up" : "30 s without a restart");
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (gate.state == OG_CONFIRM) {
    int st = confirm_state(CONFIRM_UPDATE);
    if (!cli_up()) stop("the session ended");
    else if (st == CF_ACCEPTED) {
      confirm_clear(CONFIRM_UPDATE);
      og_confirmed(&gate, millis());
      board_log("ota: update confirmed (BOOT pressed)");
      if (const char *why = startWrite()) stop(why);
    } else if (st != CF_WAITING) stop("not confirmed: BOOT was not pressed within 30 s");
  } else if (gate.state == OG_RECEIVING) {
    if (!cli_up()) stop("the session ended");
    else if (og_stalled(&gate, millis())) stop("no data for 15 s");
  }
  xSemaphoreGive(lock);
}

void ota_chunk(const uint8_t *p, size_t n) {
  if (!lock || n < 4) return;
  uint32_t off = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool busy = og_busy(&gate);
  if (!og_chunk(&gate, off, n - 4, millis())) {
    if (busy) {  // it failed just now: og_chunk() said why
      cleanUp();
      board_log("ota: update stopped: %s (still running %s)", gate.why, FW_VERSION);
    }
  } else {
    esp_err_t e = esp_ota_write(handle, p + 4, n - 4);
    if (e != ESP_OK) stop(esp_err_to_name(e));
    else {
      mbedtls_sha256_update(&sha, p + 4, n - 4);
      og_written(&gate, n - 4);
      uint8_t q = (uint64_t)gate.got * 4 / gate.size;
      if (q != lastQuarter) {
        lastQuarter = q;
        board_log("ota: %u%% (%lu bytes)", q * 25, (unsigned long)gate.got);
      }
    }
  }
  xSemaphoreGive(lock);
}

void ota_link_down() {
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (og_busy(&gate)) stop("the link to the computer dropped");
  xSemaphoreGive(lock);
}

String ota_json() {
  static const char *names[] = {"idle", "confirm", "receiving", "done"};
  const esp_partition_t *run = esp_ota_get_running_partition();
  char s[200];
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(s, sizeof s, "{\"state\":\"%s\",\"got\":%lu,\"size\":%lu,\"why\":\"%s\",\"running\":\"%s\",\"trial\":%d,\"rolled_back\":\"%s\"}",
           names[gate.state], (unsigned long)gate.got, (unsigned long)gate.size, gate.why, run ? run->label : "?",
           (int)onTrial, rolledBack ? rolledBack : "");
  xSemaphoreGive(lock);
  return s;
}
