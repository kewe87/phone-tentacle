# tentacle

Command line and MCP server that let a computer (and the AI assistant on it, such as Claude Code or Codex) see
and drive an iPhone through a [Tentacle](../README.md) board. The board is plugged into the iPhone as its USB
mouse and keyboard and receives its AirPlay screen mirroring; the computer reaches it over Wi-Fi (TCP 7202,
found by mDNS). Commands and JSON output follow [mobilecli](https://github.com/mobile-next/mobilecli).

> Not yet tested against a real board. The tests run against a fake board in Go and against a simulated board
> built from the firmware's own link code (`hardware/tests/sim`).

First use, step by step, with the button, LED and display explained: the [manual](../docs/manual.md). This page
is the reference for the CLI itself.

## Build

```sh
cd cli
go build -o tentacle ./cmd/tentacle        # Windows: -o tentacle.exe
```

Put the binary on your `PATH`. Screenshots need `ffmpeg` (`winget install Gyan.FFmpeg`, `brew install ffmpeg`,
`apt install ffmpeg`).

There are no published releases yet. Releases are tagged `vX.Y.Z` and built by
[`release.yml`](../.github/workflows/release.yml); [`install/install.sh`](install/install.sh) and
[`install/install.ps1`](install/install.ps1) download `tentacle` from the latest one (or `TENTACLE_VERSION`) and
check it against the release's `SHA256SUMS`; [`npm/`](npm/) does the same for the release `v<package version>`.

## Pair

Setting up the board's Wi-Fi, pairing (double press of BOOT, 6-digit code, one more press), preparing the
iPhone and troubleshooting are in the [manual](../docs/manual.md) ([Deutsch](../docs/manual.de.md)). In short:
`tentacle devices`, double-press BOOT, `tentacle pair`, compare the code, press BOOT, start Screen Mirroring,
`tentacle screenshot`.

Pairing is an X25519 key exchange confirmed at the board; sessions are AES-GCM (see
[`docs/cli-protocol.md`](../docs/cli-protocol.md)). Keys live in the user config dir
(`%AppData%\tentacle`, `~/Library/Application Support/tentacle`, `~/.config/tentacle`; `TENTACLE_HOME`
overrides it), mode 0600.

## Commands

| Command | |
|---|---|
| `devices` | boards on this network (`id`, `name`, `paired`, address, firmware, whether pairing is open) |
| `pair` | pair this computer (double-press BOOT first, then confirm the code with BOOT) |
| `unpair [--board]` | forget the board here; `--board`: the board forgets every paired computer too |
| `screenshot [--format png\|jpeg] [--quality N] [--scale F] [--max-size N] [--output PATH\|DIR/\|-]` | the screen after the last action settled; default a file in the current directory |
| `io tap X,Y [--aim] [--count N]` | coordinates are pixels of a full-size screenshot; `--count 2` double-taps |
| `io longpress X,Y [--duration MS]` | |
| `io swipe X1,Y1,X2,Y2` | |
| `io text 'TEXT'` | ASCII; Chinese is typed as pinyin |
| `io keys COMBO` | `enter`, `esc`, `tab`, `backspace`, `space`, arrows, `cmd+a`, `shift+tab` ... |
| `io button HOME` | the only button |
| `screencapture --format h264\|mjpeg` | live video to stdout, e.g. `\| ffplay -` |
| `device info` | board status: `ap` (mirroring), `fz` (video frozen), `usb`, `net`, `ip`, `fw`, `rssi`; always from the current session (right after a reconnect it waits up to 3 s for one; none: `"stale": true`, the last values under `previous`) |
| `calibrate` | measure the pointer (runs by itself on the first tap; needs the pointer visible) |
| `net` | the board's Wi-Fi |
| `net join [SSID [PASSWORD]]` | the board moves to another Wi-Fi; without arguments it asks for name and password (not echoed). Sent over the encrypted session. The iPhone has to mirror over that Wi-Fi too |
| `net hotspot` | the board forgets its Wi-Fi and opens its setup hotspot again |
| `listen` | what the iPhone played in the last 30 s (a video, a call, an app's voice) as words, or why it can't be heard |
| `listen on\|off` | hear the phone's sound while it mirrors (off by default; kept in the config). The words are made on this computer by whisper.cpp |
| `listen lang CODE\|none` | the language besides English, e.g. `de` (default: this computer's language) |
| `history list`, `history show RUN\|latest`, `history new` | what was done on the phone: runs (newest first), one run's steps; start a new run |
| `intent list`, `intent show NAME\|ID` | saved intents |
| `intent save --name N --prompt P [--hint H ...] [--from-run RUN [--steps 1,3,4]]` | save an intent; steps are the run's replayable moves (all, or the numbers from `history show`) |
| `intent delete NAME\|ID` | |
| `intent export NAME\|ID... [--screenshots] [--output FILE]`, `intent import FILE` | one `.tentacle-intent` file, to move intents to another computer; reference screens only with `--screenshots` |
| `memory list`, `memory search TEXT [--limit N]` | what the agent remembers, the most related first |
| `memory add TEXT [--kind preference\|fact\|app] [--topic APP]` | remember one line; passwords, codes and card or ID numbers are refused |
| `memory rm ID` | |
| `memory export [--output FILE]`, `memory import FILE` | a copy of `MEMORY.md`; on import an entry with the same id or words keeps the newer one |
| `logs [--board] [--save]` | the CLI's log; `--board`: the board's own log; `--save`: both in one file. Nothing is sent anywhere |
| `mcp` | MCP server on stdio |
| `skill install` | writes the agent skill to `~/.claude/skills/tentacle/` |
| `daemon [stop]`, `version` | |

Options and environment:

- `--device ID` or `TENTACLE_DEVICE`: which board, when several are around (the 12-hex id or the 4 hex digits of
  its name).
- `--host IP[:PORT]` or `TENTACLE_HOST`: the board's address, when it is not found by itself (`--host auto`: find
  again). The address is on the board's display.
- `--verbose` or `TENTACLE_VERBOSE=1`: how the board is found, the handshake and timings (also in the daemon's
  log). `TENTACLE_DAEMON_DEBUG=1` logs every request's command and outcome.

Output is `{"status":"ok","data":...}` or `{"status":"error","error":"..."}` (exit code 1). mobilecli commands
that need an agent on the device (`apps`, `dump ui`, `fs`, `webview`, ...) answer `not supported on Tentacle`.

The first command starts a background `tentacle daemon` that holds the session with the board and decodes the
video; commands reach it over a local socket in the config dir (log: `daemon.log` there). It exits after 10
minutes idle.

## Agents

```sh
claude mcp add tentacle -- tentacle mcp      # Claude Code
codex mcp add tentacle -- tentacle mcp       # Codex
tentacle skill install                       # Claude Code skill (the loop, typing rules)
```

MCP tools: `screenshot` (returns the image), `tap`, `longpress`, `swipe`, `type_text`, `press_key`,
`press_button`, `device_info`, and `history`, `intent_list`, `intent_show`, `intent_save`, `memory_recall`,
`memory_add`. The loop is screenshot, decide, one action, screenshot: see [SKILL.md](SKILL.md).

## History and intents

The daemon records every action and screenshot: `runs/<yyyyMMdd-HHmmss>/steps.jsonl` in the config dir (actions
in 0-1000 screen units, screens as 480-wide JPEGs). A new run starts after 10 minutes idle or with `history new`;
the newest 100 are kept. Nothing leaves the computer.

An intent is a saved task: name, prompt, up to 6 hints and recorded steps with their screens (`intents.json` and
`intent-frames/<id>/` in the config dir). `intent save --from-run RUN` takes the run's replayable moves (tap,
swipe, type, key, home).

## Memory

What the user told the agent (preferences, facts, how an app works) is kept in `memory/MEMORY.md` in the config
dir, one entry per line under a dated header. Nothing is synced anywhere.

The file formats of runs, intents (`.tentacle-intent`) and `MEMORY.md` are those of the original project's iPhone
app, which Tentacle does not have; they are kept as they were (only the intent file is named for Tentacle now;
older exports import the same).

## Limits

- Video comes at a few frames a second: a screenshot can be a moment behind, animations look choppy.
- Taps are relative mouse moves, dead-reckoned from the top-left corner and re-homed every 10 gestures; accuracy
  depends on calibration (`calibrate`, `io tap --aim`).
- Typing is ASCII only (one key per character); no clipboard, no volume/power buttons, no app install or launch.
- Phone audio: words only, and they can be misheard. It needs whisper.cpp's `whisper-cli` on `PATH` (e.g.
  `brew install whisper-cpp`) and the model `ggml-base.bin` in the config dir's `whisper/` folder; `listen` says
  what is missing.
- While the stop button on the board is on, input commands fail with `stopped (safety button)`.

## tentacle-flash

`tentacle-flash` installs the firmware over USB with nothing else to install (usage: the
[manual](../docs/manual.md#flashing-the-easy-way-tentacle-flash)). It is pure Go (no cgo) and builds into one file
per system:

```sh
cd cli
tentacle sign ../hardware/build/hid_bridge.ino.merged.bin --key KEYFILE    # puts the signature into the file
go run ./tools/flashbuild --image ../hardware/build/hid_bridge.ino.merged.bin --version 0.4.0
```

`dist/` then holds `tentacle-flash_windows_amd64.exe`, `_linux_amd64`, `_linux_arm64` and
`_darwin_universal` (arm64 + amd64 in one Mach-O file). `--image` gzips the image into
`internal/flash/release/` for `go:embed` (signature included) and takes it out again afterwards;
without `--image` the binaries carry no firmware and need `--image FILE` or "Advanced" at run time.

- **Image.** A `.merged.bin` from arduino-cli (what `hardware/flash.sh` uploads) is cut along its partition
  table: bootloader (0x0), partition table (0x8000), `otadata` (0xe000, reset so the board boots `app0`) and the
  app (0x10000) are written; `nvs` (Wi-Fi, pairing), `app1` and the rest stay as they are. A plain `.ino.bin`
  goes to 0x10000 with a fresh `otadata`. The signature travels inside the file (`fwsign.Split` cuts it off before
  the partitions are read) and is checked as for OTA (`internal/fwsign`, the release key
  in `fwsign.ReleaseKey`, compared with `hardware/hid_bridge/ota_pubkey.h` by a test): a wrong one is refused,
  none is a warning.
- **Board.** `internal/esprom` speaks the ESP32-S3 ROM bootloader protocol (SLIP, SYNC, chip detection, eFuses,
  JEDEC id, compressed writes, MD5 check). `internal/flash` finds the board: the ROM's USB-Serial/JTAG port and
  the firmware's TinyUSB port both report 303A:1001 and differ by product string ("USB JTAG/serial debug unit"
  vs. "Tentacle"). A running firmware is switched into the bootloader with a 1200 baud touch, a board in the
  ROM with the DTR/RTS sequence; afterwards the port is found again. After writing, the board is reset into the
  firmware and asked for its setup Wi-Fi (`0 setup`).
- **Page.** `internal/flashui` serves the installer page on `127.0.0.1` (random port, a token in the URL and an
  `X-Token` header on every API call, the `Host` header checked).
- **Another source.** `flash.Source` is the hook for fetching a release (e.g. from GitHub, once the repository is
  public): `Fetch` returns the signed `.merged.bin`, `ParseImage` checks it, and the source is used in
  front of `flash.Embedded` in `cmd/tentacle-flash`.
- **Tests** run against `esprom.FakeROM` and `flash.FakeSystem`, a simulated ROM and computer; nothing touches a
  serial port. `TENTACLE_FLASH_DEBUG=1 tentacle-flash --yes` prints every ROM command.

## Development

```sh
cd cli && go vet ./... && go test ./...
make -C hardware/tests/sim      # simulated board (the firmware's cli_link.cpp): enables the integration test
TENTACLE_SIM=$PWD/hardware/tests/sim/cli_sim tentacle screenshot   # the CLI against the simulator
```

Release: one path for firmware, `tentacle` and `tentacle-flash`, a tag `vX.Y.Z` and
[`release.yml`](../.github/workflows/release.yml) (see [CONTRIBUTING.md](../CONTRIBUTING.md#releasing)).
`tentacle_<os>_<arch>` for Windows (amd64, arm64; zip) and Linux (amd64, arm64; tar.gz), `tentacle_darwin_universal`
for macOS; `cli.yml` only builds and tests. Installer testing:
`TENTACLE_ARCHIVE=/path/tentacle_darwin_universal.tar.gz sh cli/install/install.sh` (SHA256SUMS next to it).

---

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).
