// Firmware update over the computer's session, see ota.h. The image comes in on cli_tcp's task (ota_chunk), the
// commands on loop(); one lock keeps them apart.
#include "ota.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include "cli_link.h"
#include "link.h"
#include "logbuf.h"
#include "netcfg.h"
#include "ota_pubkey.h"
#include "version.h"
#include "src/ota/ota_gate.h"
#include "src/ota/ota_sig.h"

// The version in a form the CLI finds in an image file (`tentacle update` shows old -> new and refuses a file
// without it). Referenced below, so the linker keeps it.
extern "C" const char tentacle_fw_marker[] = FW_MARKER;

// A new firmware is kept once a session comes up, or once its network (station with an address, or the setup
// hotspot) has been up this long. Merely running is not enough: an image that starts but lost its Wi-Fi would be
// kept, and only a USB cable would bring the board back. Without either by TRIAL_GIVEUP_MS it goes back.
const uint32_t TRIAL_NET_MS = 30000;
const uint32_t TRIAL_GIVEUP_MS = 120000;
static uint32_t netSince;  // millis() since the network is up on trial (0: not up)
// Unsigned images: only a debug build takes them (it is never handed out). A release build takes only images
// signed with the release key (ota_pubkey.h).
const bool ALLOW_UNSIGNED = TENTACLE_DEBUG;

static SemaphoreHandle_t lock;
static ota_gate gate;
static ota_begin_req offer;               // what `update begin` announced: checked again before the switch
static esp_ota_handle_t handle;           // 0: no flash write open
static const esp_partition_t *target;
static mbedtls_sha256_context sha;
static bool shaOn;
static bool onTrial;                      // this boot runs an image that is not marked good yet
static const char *rolledBack;            // the slot of an update that did not start properly ("" if none)
static uint8_t lastQuarter;
static bool fromWeb;                      // the running update came from the web interface (web_server.cpp), not
                                          // the computer's session: each side's chunks only go into its own update

// Arduino's initArduino() would mark a new image good before setup(); it is marked in ota_poll() instead, once it
// has shown that it works.
extern "C" bool verifyRollbackLater() { return true; }

void ota_setup() {
  lock = xSemaphoreCreateMutex();
  og_init(&gate);
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  rolledBack = bad ? bad->label : "";
  esp_ota_img_states_t st;
  onTrial = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  if (onTrial)
    board_log("ota: new firmware %s in %s on trial: kept once a session comes up or the network has been up "
              "for 30 s; a restart before that, or 2 min without either, goes back to the previous one", FW_VERSION, run->label);
  else
    board_log("ota: %s in %s%s%s%s, %s", tentacle_fw_marker, run->label, bad ? ", the update in " : "", bad ? bad->label : "",
              bad ? " did not start properly and was given up" : "",
              ALLOW_UNSIGNED ? "takes signed and unsigned updates (debug build)" : "takes signed updates only");
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
}

static void stop(const char *why) {
  bool was = og_busy(&gate);
  og_fail(&gate, why);
  cleanUp();
  if (was) board_log("ota: update stopped: %s (still running %s)", why, FW_VERSION);
}

static String begin(const String &arg) {
  ota_begin_req r;
  if (!og_parse_begin(arg.c_str(), &r)) return "err usage: update begin SIZE SHA256HEX SIGHEX|- [VERSION]";
  if (og_busy(&gate)) return "err busy: an update is already running";
  // Before a single byte: an image this board would not take is refused here, not after the transfer.
  if (const char *why = ota_sig_check(&r, OTA_RELEASE_PUBKEY, ALLOW_UNSIGNED)) {
    board_log("ota: firmware %s refused: %s", r.version, why);
    return String("err ") + why;
  }
  keep("an update was asked by the session");  // a running image still on trial cannot start an update
  target = esp_ota_get_next_update_partition(nullptr);
  const char *why = og_begin(&gate, &r, target ? target->size : 0, millis());
  if (why) return String("err ") + why;
  offer = r;
  esp_err_t e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);  // erases as it goes
  if (e != ESP_OK) {
    handle = 0;
    stop(esp_err_to_name(e));
    return String("err ") + gate.why;
  }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  shaOn = true;
  lastQuarter = 0;
  board_log("ota: receiving %lu bytes of firmware %s (%s) into %s", (unsigned long)gate.size, gate.version,
            r.has_sig ? "signed" : "unsigned", target->label);
  return "ok ready";
}

static String state() {
  switch (gate.state) {
    case OG_RECEIVING: return "ok ready " + String(gate.got) + " " + String(gate.size);
    case OG_DONE: return "ok done";
    default: break;
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
  // The signature covers that SHA-256: checked at begin, and once more right before the switch.
  if (const char *why = ota_sig_check(&offer, OTA_RELEASE_PUBKEY, ALLOW_UNSIGNED)) {
    stop(why);
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

// One `update ...` for either side. The other side's running update is left alone (busy).
static String command(const String &arg, bool web) {
  bool busy = og_busy(&gate);
  if (busy && web != fromWeb) return "err busy: an update is already running";
  if (arg.startsWith("begin ")) {
    String r = begin(arg.substring(6));
    if (r.startsWith("ok")) fromWeb = web;
    return r;
  }
  if (arg == "state") return state();
  if (arg == "abort") {
    if (busy) stop(web ? "stopped from the web interface" : "stopped by the computer");
    return "ok";
  }
  if (arg == "end") {
    const char *why = finish();
    return why ? String("err ") + why : String("ok restarting into ") + gate.version;
  }
  return "err usage: update begin SIZE SHA256HEX SIGHEX|- [VERSION] | state | end | abort";
}

String ota_web(const String &arg) {
  if (!lock) return "err not ready";
  xSemaphoreTake(lock, portMAX_DELAY);
  String reply = command(arg, true);
  xSemaphoreGive(lock);
  return reply;
}

String ota_command(const String &arg, const String &id) {
  String reply;
  xSemaphoreTake(lock, portMAX_DELAY);
  reply = command(arg, false);
  bool restart = gate.state == OG_DONE && arg == "end" && !fromWeb;
  xSemaphoreGive(lock);
  if (!restart) return reply;
  link_reply(id + " " + reply);
  link_flush();                    // let the reply go out
  vTaskDelay(pdMS_TO_TICKS(300));  // ... and onto the network before the connection closes
  ESP.restart();
  return "";
}

void ota_poll() {
  if (onTrial) {
    bool net = net_up() || net_setup_active();
    if (!net) netSince = 0;
    else if (!netSince) netSince = millis() | 1;
    if (cli_up()) keep("a session came up");
    else if (netSince && millis() - netSince >= TRIAL_NET_MS) keep("the network has been up for 30 s");
    else if (millis() >= TRIAL_GIVEUP_MS) {
      board_log("ota: firmware %s found no network and no session in 2 min: back to the previous one", FW_VERSION);
      link_flush();
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_ota_mark_app_invalid_rollback_and_reboot();  // returns only if there is nothing to go back to
      onTrial = false;
    }
  }
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (gate.state == OG_RECEIVING) {
    if (!fromWeb && !cli_up()) stop("the session ended");
    else if (og_stalled(&gate, millis())) stop("no data for 15 s");
  }
  xSemaphoreGive(lock);
}

static void chunk(const uint8_t *p, size_t n, bool web) {
  if (!lock || n < 4) return;
  uint32_t off = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (web != fromWeb) {  // not this side's update
    xSemaphoreGive(lock);
    return;
  }
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

void ota_chunk(const uint8_t *p, size_t n) { chunk(p, n, false); }
void ota_web_chunk(const uint8_t *p, size_t n) { chunk(p, n, true); }

void ota_link_down() {
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (og_busy(&gate) && !fromWeb) stop("the link to the computer dropped");
  xSemaphoreGive(lock);
}

String ota_json() {
  static const char *names[] = {"idle", "receiving", "done"};
  const esp_partition_t *run = esp_ota_get_running_partition();
  char s[220];
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(s, sizeof s, "{\"state\":\"%s\",\"got\":%lu,\"size\":%lu,\"why\":\"%s\",\"running\":\"%s\",\"trial\":%d,"
           "\"rolled_back\":\"%s\",\"unsigned_ok\":%d}",
           names[gate.state], (unsigned long)gate.got, (unsigned long)gate.size, gate.why, run ? run->label : "?",
           (int)onTrial, rolledBack ? rolledBack : "", (int)ALLOW_UNSIGNED);
  xSemaphoreGive(lock);
  return s;
}
