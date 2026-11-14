# Fork notes & roadmap (kewe87)

**English** · [Deutsch](ROADMAP-FORK.de.md)

This fork adds security fixes, safer defaults, robustness fixes and optional hardware to
[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).
All upstream copyright and the LICENSE are kept, as required by the MIT License and `AGENTS.md`.

## ✅ Implemented in this fork

| Change | File(s) | Verified |
|---|---|---|
| **AirPlay/RTSP + bplist overflow fixes** (pre-auth out-of-bounds read → DoS) | `src/airplay/rtsp.c`, `src/bplist/bplist.c` | compiles; regression test + fuzzing in CI |
| **Stop button + status LED** (BOOT button, **on by default**, `--no-safety` to remove) | `hardware/hid_bridge/hid_bridge.ino`, `flash.sh` | compiles with and without; **needs on-hardware test** |
| **Single-client SoftAP** (**on by default**, `--multi-client` to allow 4) | `netcfg.cpp`, `flash.sh`, `docs/hardening.md` | compiles; **check phone re-join on hardware** |
| **Windows BLE scan fix** (#114: empty `devices[]` on WinRT) | `cli/internal/ble/ble.go` | `go build` / `vet` / `test` in CI |
| **BLE discovery boost** (short name in the primary advertisement + faster interval) | `ble_link.cpp` | compiles; **needs on-hardware test** |
| **Daemon panic guard** (malformed `io` request crashed the whole daemon) | `cli/internal/daemon/ops.go`, `server.go` | CI |
| **Decoder.err data race** fix | `cli/internal/video/decoder.go` | CI |
| **Empty-line panic guard** | `cli/internal/daemon/conn.go` | CI |
| **OLED status display** (`--display`, SSD1306 I²C via U8g2) | `hid_bridge.ino`, `docs/hardware-addons.md` | compiles; **needs the OLED wired** |
| **Battery gauge** (`--battery-pin`, on the OLED) | `hid_bridge.ino`, `docs/hardware-addons.md` | compiles; **needs a LiPo + divider** |
| **bplist fuzz + regression harness** (libFuzzer + portable repro) | `hardware/tests/fuzz/` | runs in CI (libFuzzer + ASan/UBSan) |
| **CI** (3 firmware variants, `go build`/`vet`/`test`, bplist fuzz) | `.github/workflows/fork-ci.yml` | green on GitHub Actions |

Security details: [docs/hardening.md](docs/hardening.md).

## 🟡 Open (lower severity — tracked, not yet fixed)

Fixing these well needs runtime testing:

- **AirPlay SETUP stream loop** (`src/airplay/handlers.c`): iterates over every entry of the `streams`
  array and does a dict lookup in each; a crafted 64 KB request could keep the board busy (possible
  CPU DoS, untested). Fix idea: cap at ~8 entries (real devices send 1–2).
- **Goroutine leak**: `daemon/listen.go` `transcribe` — `h.work` is never closed on Conn teardown.
- **Pointer state not serialized** per board across concurrent requests (`internal/pointer`).
- **`wifi.go fallBack`** lacks the `claim()/release()` guard that `up()` has (narrow race).
- **`ble/tcp.go`** keepalive goroutine lingers up to one tick after read-side teardown (tidy-up).

## 💡 Proposed features (need the iOS app / hardware; design only so far)

- **Secure-field typing guard** — refuse to type when a password/secure field is detected (OCR), fail-closed.
- **App allowlist** — only let the agent open/act in approved apps.
- **Action audit log** — persist what the agent did (the board already has a RAM log ring).
- **End-state verification** — check the real end state to catch "false success".
- **Android dual-mode** — one "hands" firmware drivable by both hosts (see [docs/android.md](docs/android.md)).

---
Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan.
