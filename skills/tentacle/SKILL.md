---
name: tentacle
description: See and drive a real iPhone through a Tentacle board with the tentacle CLI or MCP server. Use when asked to look at, tap, swipe, type on, or operate an iPhone, or to test an iOS app on a real device.
license: MIT-0
---

# Tentacle: drive a real iPhone with tentacle

A Tentacle board plugs into the iPhone and acts as the phone's USB mouse and keyboard. It also receives the
iPhone's AirPlay screen mirroring. Your computer reaches the board over Wi-Fi with the `tentacle` command line
tool, which is also an MCP server. No jailbreak and no app on the phone are needed.

iPhone only for now.

## Prerequisites

- A Seeed Studio XIAO ESP32S3 board (8 MB flash, 8 MB PSRAM) with the Tentacle firmware, set up on the same
  Wi-Fi as the computer (the user does this once, on the board's setup hotspot `Tentacle-XXXX`).
- On the iPhone:
  - AssistiveTouch on (Settings > Accessibility > Touch > AssistiveTouch), so the mouse pointer shows and taps.
  - Screen Mirroring to the board: Control Center > Screen Mirroring > Tentacle XXXX. Only the user can start it.
  - The phone unlocked while the agent works.
- On the computer: the `tentacle` binary on `PATH` (built from the repository: `cd cli && go build -o tentacle
  ./cmd/tentacle`) and `ffmpeg` (needed for screenshots).

MCP server (stdio), if your agent does not already have it from this plugin:

```sh
claude mcp add tentacle -- tentacle mcp
```

Other MCP clients:

```json
{ "mcpServers": { "tentacle": { "command": "tentacle", "args": ["mcp"] } } }
```

## Pair (once per computer; needs the user at the board)

1. Ask the user to double-press the BOOT button on the board (two quick presses). Pairing is then open for 60 s.
2. Run `tentacle pair`. It prints a 6-digit code and waits.
3. Ask the user to compare the code with the board's display and to press BOOT once more within 30 s.
4. Ask the user to start Screen Mirroring on the iPhone to Tentacle XXXX.
5. `tentacle screenshot` should now save a PNG of the phone's screen.
6. `tentacle calibrate` measures the pointer. It also runs by itself before the earliest tap. It needs the
   pointer visible on screen.

Never retry `pair` in a loop: the board locks pairing for a while after a request that was not confirmed.

## MCP tools

| Tool | What it does |
|---|---|
| `screenshot` | The screen now, after the last action settled. Other tools take pixel coordinates of this image |
| `tap` | Tap at `x`, `y`; `count: 2` double-taps |
| `longpress` | Press and hold at `x`, `y` (`duration_ms`, default 1000) |
| `swipe` | Swipe from `x1`, `y1` to `x2`, `y2` |
| `type_text` | Type into the focused field (ASCII; Chinese is typed as pinyin) |
| `press_key` | `enter`, `esc`, `tab`, `backspace`, `space`, arrows, `cmd+a`, `shift+tab` ... |
| `press_button` | `HOME` only (go to the Home Screen) |
| `device_info` | Board status: mirroring (`ap`), video frozen (`fz`), `usb`, `net` |
| `history` | `action: list` (recent runs) or `show` (one run's steps with their screenshots) |
| `intent_list`, `intent_show`, `intent_save` | Saved tasks: list, show one, save what worked |
| `memory_recall`, `memory_add` | What the user asked to remember: search it, add a line |

Every tool takes an optional `device` (board id) when more than one board is on the network.

## CLI commands

Every command prints JSON: `{"status":"ok","data":...}` or `{"status":"error","error":"..."}` (exit code 1).

| Command | What it does |
|---|---|
| `devices` | Boards on this network (`id`, `name`, `paired`) |
| `pair`, `unpair` | Pair this computer with the board; forget it here |
| `screenshot [--format png\|jpeg] [--quality N] [--scale F] [--max-size N] [--output PATH\|DIR/\|-]` | Save the screen; prints `filePath` |
| `io tap X,Y [--aim] [--count N]` | Tap; `--aim` finds the pointer and corrects before tapping |
| `io longpress X,Y [--duration MS]` | Press and hold |
| `io swipe X1,Y1,X2,Y2` | Swipe |
| `io text 'TEXT'` | Type ASCII text |
| `io keys COMBO` | Press a key or combo |
| `io button HOME` | Home Screen |
| `device info` | Board status |
| `calibrate` | Measure the pointer |
| `listen` | What the phone played in the last 30 s, as words (after `listen on`) |
| `history list`, `history show RUN\|latest`, `history new` | What was done on the phone; start a new run |
| `intent list`, `intent show NAME`, `intent save ...`, `intent delete NAME` | Saved tasks |
| `memory search TEXT`, `memory add TEXT` | What the user asked to remember |
| `mcp` | Run the MCP server on stdio |
| `update FILE.bin` | New board firmware over Wi-Fi (signed images only; `FILE.bin.sig` beside it). Only when the user asks |
| `logs --board [--follow]` | The board's log; `--follow` streams new lines (debug firmware) |

With several boards pass `--device <id>` or set `TENTACLE_DEVICE`. A board that is not found by itself:
`--host IP` (the address is on the board's display).

## The see-act loop

1. Take a screenshot (`screenshot` tool, or `tentacle screenshot` and read the PNG at `filePath`).
   It waits until the screen has settled after your last action.
2. Decide on one action.
3. Do that one action, with coordinates in pixels of that full-size screenshot. If you used `--scale` or
   `--max-size`, convert the coordinates back to full size.
4. Take another screenshot and check the result before the next step. Never chain several taps blind.

Tips:
- To scroll a list, swipe up from the lower middle to the upper middle of the screen.
- To open an app: press HOME, swipe down in the middle of the Home Screen, type the app name, press `enter`.
- If a tap missed, retry with `io tap X,Y --aim`.
- Tap a text field so it has focus before typing. `cmd+a` then `backspace` clears it.
- The board is a hardware keyboard, so iOS may hide the on-screen keyboard; the field still takes the keys.
- Chinese text is typed as pinyin (`io text '拿铁'` types `natie`). With the Chinese pinyin keyboard active, press
  `space` to accept the top candidate, or screenshot and tap the right one.

## Save what worked

After a task succeeds, save it so the next run is faster:
1. `history show latest` (MCP `history`, action `show`) lists the run's numbered steps in 0-1000 screen units.
2. Pick the steps that did the job, skipping misses and retries.
3. `intent save --name 'Alarm 7:00' --prompt 'Set an alarm for 7:00 AM every day' --hint 'Type the time' --from-run RUN --steps 2,3,5`
   (MCP `intent_save`). Keep the prompt short and use at most 6 hints.

To reuse one, `intent show NAME`, convert its 0-1000 coordinates to pixels (x * width / 1000,
y * height / 1000), and check each screen against the step's reference screen. If the screen looks different,
drop the recorded steps and continue with the normal loop.

## Safety

You are using a real phone with the owner's accounts, money and contacts.
- Ask the user to confirm before anything that spends money, sends a message, email or post, calls someone,
  deletes data, changes passwords or security settings, or accepts terms.
- Do not read or copy private content (messages, photos, codes, passwords) beyond what the task needs.
- If a screen asks for a password, passcode, Face ID or a one-time code, stop and hand it to the user.
- Stop and ask when the screen is not what you expected.
- `stopped (safety button)` means the user pressed the stop button on the board. Stop, and ask before going on.

## Troubleshooting

| Error or symptom | What to do |
|---|---|
| `not paired` | Ask the user to double-press BOOT, run `pair`, and ask them to press BOOT once more when the code shows |
| `pairing closed` | The board was not double-pressed in the last 60 s; ask the user to do it, then `pair` |
| `not found on the network` | The board is off, on another Wi-Fi, or not set up yet; ask the user (its address can be given with `--host IP`) |
| `screen mirroring is off` | Ask the user to start Screen Mirroring to Tentacle XXXX |
| `video frozen` | Ask the user to stop and restart Screen Mirroring |
| `phone usb asleep` | The phone is locked; ask the user to unlock it |
| `stopped (safety button)` | The user stopped all input at the board. Ask before going on |
| `not supported on Tentacle` | There is no app list, UI tree or file access; work from screenshots |
| Taps land off target | Make sure AssistiveTouch is on and the pointer is visible, run `calibrate`, or use `io tap --aim` |
| Screenshot fails | Check that `ffmpeg` is installed and Screen Mirroring is on |

Limits: screenshots can be a moment behind and animations look choppy. Typing is ASCII only, one key per
character. HOME is the only button: no volume or power buttons, and no installing or launching apps directly.
