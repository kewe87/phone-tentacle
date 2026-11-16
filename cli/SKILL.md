---
name: tentacle
description: See and drive a real iPhone through a Tentacle board (USB mouse/keyboard + AirPlay screen mirroring, reached over Wi-Fi). Use when asked to look at, tap, type on, or operate the iPhone, or to test an iOS app on the device.
---

# tentacle

`tentacle` talks to a Tentacle board plugged into the iPhone. The board is the phone's USB mouse and
keyboard and receives its AirPlay screen mirroring; this computer reaches it over Wi-Fi (found by itself on the
local network, or `--host IP`). Every command prints
JSON: `{"status":"ok","data":...}` or `{"status":"error","error":"..."}`. The MCP server (`tentacle mcp`)
offers the same as tools: screenshot, tap, longpress, swipe, type_text, press_key, press_button, keyboard_layout,
device_info, plus history, intent_list, intent_show, intent_save, memory_recall and memory_add.

## Setup (once)
- `tentacle pair`: the user first double-presses the BOOT button on the board (two quick presses; pairing is then
  open for 60 s, and `pair` waits up to 60 s for it). It prints a 6-digit code and waits; the user compares it with
  the board's display and presses BOOT once more within 30 s. This needs the user at the board: ask them, never
  retry in a loop (the board locks pairing for a while after a request that was not confirmed).
- Screen Mirroring on the iPhone: Control Center > Screen Mirroring > Tentacle XXXX (the board's name with its 4-character suffix).
- AssistiveTouch on (Settings > Accessibility > Touch > AssistiveTouch): without it taps do nothing, yet the CLI
  still answers `tap done`. Tip for the user: Settings > Accessibility > Accessibility Shortcut > AssistiveTouch,
  then a triple click of the side button turns it on.
- The keyboard layout, once: `tentacle keyboard layout de` for an iPhone with a German hardware keyboard layout
  (Settings > General > Keyboard > Hardware Keyboard), `us` (default) otherwise. Wrong: `_` comes out `?`, Y/Z swap.
- `tentacle calibrate` (also runs by itself on the first tap). It needs the mouse pointer visible on screen.
- `tentacle update FILE.bin` puts new firmware on the board over Wi-Fi (only images signed for it, `FILE.bin.sig`
  beside the file). Only when the user asks for it.

## The loop
1. `tentacle screenshot` (writes a PNG in the current directory and prints its `filePath`; read the image).
   It waits until the screen has settled after your last action. The iPhone sends a picture only when the screen
   changes: the answer has `age_ms`, and `"stale": true` with a `warning` when no newer picture came within 3 s
   (`--wait MS`) after your last action. Stale: the action may have changed nothing, or the picture stopped;
   screenshot once more, and if it stays stale ask the user to restart Screen Mirroring.
2. Decide one action from what you see.
3. Act, with coordinates in pixels of that screenshot (full size: don't pass `--scale`/`--max-size` if you
   take coordinates from it):
   - `tentacle io tap X,Y` (add `--aim` after a missed tap: finds the pointer and corrects first; `--count 2` double-taps, e.g. to zoom a map or select a word)
   - `tentacle io longpress X,Y --duration 1500`
   - `tentacle io swipe X1,Y1,X2,Y2` (scroll a list: swipe up from the lower to the upper middle)
   - `tentacle io text 'hello'`, `tentacle io keys enter`, `tentacle io keys cmd+a`
   - `tentacle io button HOME` (Home Screen)
4. Screenshot again and check the result before the next step. Never chain several taps blind.

When the task depends on what the phone plays (a video, a call, a voice message, a spoken code),
`tentacle listen` gives its words from the last 30 s, or says why the phone can't be heard (listening off,
no sound). The words can be partial or misheard; check the screen when it matters.

## Typing
- The board types with the keys of its keyboard layout (`tentacle keyboard layout`): ASCII; on `de` also
  ä ö ü ß € and accented letters. Characters the layout lacks are typed without the accent or left out; the reply
  names them (`not_typed`, `typed_as`).
- Chinese is typed as pinyin: `io text '拿铁'` types `natie` (the reply says `typed_as`). With the iPhone's Chinese (pinyin)
  keyboard active, commit the first candidate with `io keys space`, or screenshot and tap the right candidate.
  Many search boxes accept the plain pinyin as well.
- The board is a hardware keyboard, so iOS may hide the on-screen keyboard; the text field still takes the keys.
- Tap the text field first so it has focus; `io keys cmd+a` then `io keys backspace` clears it.

## Save what worked
After a task succeeded, save it so the next run is faster:
1. `tentacle history show latest` (MCP: `history` with action show): the run's steps in 0-1000 screen units,
   numbered, each with the screenshot it was done on. Every action and screenshot is recorded by itself; a new run
   starts after 10 minutes idle, or with `tentacle history new` before a new task.
2. Pick the steps that did the job (skip detours, misses and retries).
3. `tentacle intent save --name 'Alarm 7:00' --prompt 'Set an alarm for 7:00 AM every day' --hint 'Type the time
   instead of scrolling the wheel' --from-run RUN --steps 2,3,5` (MCP: `intent_save` with name, prompt, hints,
   from_run, steps). One short, clear instruction as the prompt and at most 6 concrete hints. Only replayable moves
   can be steps: tap, swipe, type, key, home. The same name replaces the old intent.

## Run a saved intent
1. `tentacle intent list`, then `tentacle intent show NAME` (MCP: `intent_list`, `intent_show`): the prompt,
   hints and recorded steps, each with its reference screen (a JPEG path).
2. Follow the steps as references, not blindly: coordinates are 0-1000 units of the screen, so convert them to
   pixels of your screenshot (x * width / 1000, y * height / 1000).
3. Take a screenshot after each step and check it looks like the next step's reference screen.
4. On a mismatch (another screen, a popup, a changed layout) drop the recorded steps and go on with the normal loop,
   using the prompt and hints.

## Memory
- Before a task, `tentacle memory search 'TASK'` (MCP: `memory_recall`): the user's preferences and how their apps
  work.
- When the user says to remember something, `tentacle memory add 'TEXT' --kind preference` (MCP: `memory_add`).
  Only the user's own words; never passwords, codes or numbers off the screen.

## Errors to act on
- `screen mirroring is off`: ask the user to start Screen Mirroring to Tentacle XXXX.
- `no screen picture yet …`: take a screenshot first (taps need the screenshot's size).
- After `tentacle update` the board restarted: ask the user to start Screen Mirroring again.
- `video frozen`: ask the user to stop and restart Screen Mirroring.
- `not paired`: ask the user to double-press BOOT on the board, run `tentacle pair`, and ask them to press BOOT
  once more when the code shows.
- `not found on the network`: the board is off, on another Wi-Fi, or not set up yet; ask the user (its address can
  be given with `--host IP`).
- `stopped (safety button)`: the user pressed BOOT to stop all input. Stop, and ask before going on.
- `phone usb asleep`: the phone is locked; ask the user to unlock it.
- `not supported on Tentacle`: there is no app list, UI tree, file access or webview; work from screenshots.

## Limits
- Video comes over Wi-Fi at a few frames a second: a screenshot can be a moment behind, animations look choppy.
- Only HOME as a button; no volume, power, or app install/launch (open apps from the Home Screen or Spotlight:
  `io button HOME`, swipe down in the middle, `io text 'Settings'`, `io keys enter`).
