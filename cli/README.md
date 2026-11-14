# tentacle

Command line and MCP server that let a computer (and the AI assistant on it, such as Claude Code or Codex) see
and drive an iPhone through a [Tentacle](../README.md) board. The board is plugged into the iPhone as its USB
mouse and keyboard and receives its AirPlay screen mirroring; the computer reaches it over Wi-Fi (TCP 7202,
found by mDNS). Commands and JSON output follow [mobilecli](https://github.com/mobile-next/mobilecli).

> Not yet tested against a real board. The tests run against a fake board in Go and against a simulated board
> built from the firmware's own link code (`hardware/tests/sim`).

## Build

```sh
cd cli
go build -o tentacle ./cmd/tentacle        # Windows: -o tentacle.exe
```

Put the binary on your `PATH`. Screenshots need `ffmpeg` (`winget install Gyan.FFmpeg`, `brew install ffmpeg`,
`apt install ffmpeg`).

There are no releases yet. Once there are, [`install/install.sh`](install/install.sh) and
[`install/install.ps1`](install/install.ps1) download a release from GitHub and check its SHA-256, and
[`npm/`](npm/) wraps the same download; none of them has anything to fetch today.

## Pair

The board has to be on your Wi-Fi first (its setup hotspot `Tentacle-XXXX` and the page on it: see the
[main README](../README.md#setup-step-by-step)).

1. `tentacle devices` lists the boards on this network.
2. Double-press BOOT on the board: pairing is open for 60 s (`tentacle pair` waits up to 60 s for that).
3. `tentacle pair` prints a 6-digit code. Compare it with the board's display, then press BOOT once more within
   30 s.
4. On the iPhone (same Wi-Fi): Control Center > Screen Mirroring > Tentacle XXXX.
5. `tentacle screenshot` should now write a PNG of the phone's screen.

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
| `device info` | board status: `ap` (mirroring), `fz` (video frozen), `usb`, `net`, `ip`, `fw` |
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
| `intent export NAME\|ID... [--screenshots] [--output FILE]`, `intent import FILE` | one `.esp32intent` file, to move intents to another computer; reference screens only with `--screenshots` |
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

The file formats of runs, intents (`.esp32intent`) and `MEMORY.md` are those of the original project's iPhone
app, which Tentacle does not have; they are kept as they were.

## Limits

- Video comes at a few frames a second: a screenshot can be a moment behind, animations look choppy.
- Taps are relative mouse moves, dead-reckoned from the top-left corner and re-homed every 10 gestures; accuracy
  depends on calibration (`calibrate`, `io tap --aim`).
- Typing is ASCII only (one key per character); no clipboard, no volume/power buttons, no app install or launch.
- Phone audio: words only, and they can be misheard. It needs whisper.cpp's `whisper-cli` on `PATH` (e.g.
  `brew install whisper-cpp`) and the model `ggml-base.bin` in the config dir's `whisper/` folder; `listen` says
  what is missing.
- While the stop button on the board is on, input commands fail with `stopped (safety button)`.

## Development

```sh
cd cli && go vet ./... && go test ./...
make -C hardware/tests/sim      # simulated board (the firmware's cli_link.cpp): enables the integration test
TENTACLE_SIM=$PWD/hardware/tests/sim/cli_sim tentacle screenshot   # the CLI against the simulator
```

Release: push a tag `cli-vX.Y.Z`; `.github/workflows/cli.yml` builds darwin, linux and windows for amd64/arm64
and uploads the archives with `checksums.txt` to a GitHub release. Installer testing:
`TENTACLE_ARCHIVE=/path/tentacle_darwin_arm64.tar.gz sh cli/install/install.sh` (checksums.txt next to it).

---

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).
