# Changelog

Tentacle's own history starts here. It is derived from
[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (the state of its tag
`v1.0.6`: board firmware 0.2.5, CLI 0.1.5); that project's history is in its repository.

## 0.4.1 (firmware): Waveshare e-paper board, recording on microSD – unreleased

Runs on the board (flashed and on Wi-Fi 2026-11-21); the screen recording has not been played back from a card yet.

- **The Waveshare ESP32-S3-ePaper-1.54** ([hardware-addons.md](docs/hardware-addons.md#waveshare-esp32-s3-epaper-154-e-paper-clock-microsd)):
  the same image finds it by itself and uses its e-paper, clock and microSD slot.
- **The e-paper** shows the status as symbols in the board's language: the Tentacle mark and the name, running or
  stopped, Wi-Fi with its strength, the phone mirroring, the CLI connected, the address in large type, the
  recording, the battery. The setup Wi-Fi's QR code fills it; the Tentacle mark is the start picture.
- **A wall clock:** the command `time EPOCH OFFSET`; the CLI and the web page set it, the board's clock keeps it.
- **Recording on the card** (setting `sdlog off|log|video`): the log and every input (typed text only as its
  length) as `/tentacle/sNNNNN.log`, and with `video` the mirrored screen as `sNNNNN.ts` (H.264 in MPEG-TS, our
  own muxer, tested on the host with ffprobe), in parts of 3.9 GB.
- **The web interface:** the settings are a tab of their own instead of a dialog; a REC button in the head starts
  and stops the recording.
- **The setup page** also asks for the board's name, the language (it switches the page), the keyboard layout and
  the recording; stored with the network after the press of BOOT, as before.
- **The setup hotspot sends at 15 dBm on this board** (5 dBm on the XIAO): at 5 dBm phones did not see it.
- **The release key:** firmware is now signed with the release key instead of the development key of 16 November.
  A board that runs an older build takes 0.4.1 only over USB (`tentacle-flash`), not as a network update.
- **`tentacle-flash`** names both boards in its messages and on its page.

## Video buffer as a ring, the web picture recovers by itself (firmware) – unreleased

Not run on hardware yet.

- **The board's video buffer no longer freezes.** It keeps the units since the last keyframe in one ring of 4.5 MB
  (before: a 4 MB GOP that froze when full, plus a 512 KB ring behind it, about 20 s at 8 fps). When it is full it
  drops the oldest units: a stream that is on the chain goes on, minutes behind if need be; only a stream that has
  to start from the keyframe (a page opened or reloaded, a snap) waits for the next one. Seen 2026-11-16: "GOP
  full (1225 AUs, 4073 KB)" 150 s after mirroring started, then the web page stayed black until mirroring restarted.
- **The web page is never cut off for being slow.** It used to skip to the next keyframe once it was 6 s behind,
  and a page that had just caught up with a big buffer always was; iOS often sends no keyframe until mirroring
  restarts, so the picture stayed black. Now it gets every frame in order and catches up (the corner says
  "catching up"); it draws a few frames a second while it decodes the backlog.
- **The web page resumes after a dropped connection** (`stream on at IDR_TS N`, as the CLI does): its decoder
  keeps its place, so it goes on even after the keyframe left the buffer. `stream on at` from the CLI works past
  that point too.
- When the picture really can not go on, the page says why and what brings a new keyframe: turn the phone sideways
  and back in an app that turns along, or restart Screen Mirroring.
- Status: `ovf` counts chains whose start was dropped, `fz` is 1 when that happened and the session's stream is not
  on the chain; `vdbg` shows the ring and both streams.

## Signed firmware in one file – unreleased

- **No more `.sig` files.** `tentacle sign FILE --key K` writes the signature into the file itself (in place,
  atomically; signing again replaces it): image ‖ 64-byte Ed25519 signature ‖ `TNTSIG1\n`. The signature still
  covers the image alone, as before (`"tentacle-fw-v1" ‖ SHA-256(image)`).
- `tentacle update FILE`, the board's web page (Settings › Firmware: one file field instead of two) and
  `tentacle-flash` (page and `--image`) take the one file, cut the signature off and send the image and the
  signature apart, as before; the board firmware is unchanged and never sees the trailer. An unsigned file is
  reported as "not signed".
- Older files with `FILE.sig` next to them still work with `tentacle update` and `tentacle-flash --image`.
- The release carries `tentacle-fw-X.Y.Z.bin` and `.merged.bin` signed inside, without `.sig` files; the workflow
  checks the embedded signatures against `ota_pubkey.h`.

## Release workflow – unreleased

Not run on GitHub yet.

- **`.github/workflows/release.yml`**: a tag `vX.Y.Z` (equal to `FW_VERSION` in `hardware/hid_bridge/version.h`)
  builds the release firmware (arduino-cli 1.5.1, esp32 core 3.3.12, only `build_cfg.h`, no debug switch), signs
  the app image and the merged image with the release key from the secret `TENTACLE_SIGNING_KEY`, checks both
  signatures against `ota_pubkey.h`, builds `tentacle` (Windows, Linux amd64/arm64, macOS universal) and
  `tentacle-flash` with the signed image inside, and creates a draft release with `SHA256SUMS`. Started by hand it
  is a dry run that keeps the files as workflow artifacts. See CONTRIBUTING.md, "Releasing".
- **One release path:** `cli.yml` only builds and tests (no more `cli-v*` releases). `install.sh`, `install.ps1` and
  the npm package fetch `tentacle` from the `vX.Y.Z` releases and check it against `SHA256SUMS`; macOS gets
  `tentacle_darwin_universal`. The release also carries `tentacle_windows_arm64`.

## tentacle-flash – unreleased

Not run on hardware yet.

- **Firmware installer with nothing to install**: one file each for Windows, macOS (universal) and Linux
  (amd64, arm64), pure Go. A double click opens a three-step page on `127.0.0.1` (plug in, install, done with the
  setup Wi-Fi as a QR code), English and German; `--yes` does the same on the command line.
- Speaks the ESP32-S3 ROM bootloader itself (compressed writes, MD5 check), finds the board on the ROM's and on
  the firmware's USB port, checks chip, flash size and PSRAM, and keeps the stored Wi-Fi and the pairing.
- Checks the image's Ed25519 signature with the release key; unsigned images get a warning.

## 0.4.0 (firmware) – unreleased

Not run on hardware yet.

- **One release image for every board.** What used to be a build option is a setting on the board, kept in NVS
  across restarts and updates: `tentacle device settings`, `tentacle device set KEY VALUE`, the web interface's
  Settings › Hardware, and the session command `config` (paired computers only; docs/cli-protocol.md).
  - `display auto|on|off`: the status OLED is found on I²C by itself (0x3C/0x3D); a board without one runs the same.
  - `abs on|off`: the absolute pointer is the default now; `off` is the relative mouse the CLI calibrates. Takes
    effect after a restart (it changes the USB descriptor). The status says `"abs":1` as the board runs.
  - `battery none|GPIO`, `battery_div`: the LiPo gauge; the full status has `"bat"` with a pin set.
  - `lang auto|en|de`: the OLED's texts in English or German (`auto` follows the web interface).
  - `name`: as before.
  An NVS without these keys keeps today's release behaviour: absolute pointer, OLED found automatically.
- `flash.sh --abs`, `--display` and `--battery-pin` are accepted and ignored; the U8g2 library is always needed.
  `SAFETY_CONTROLS` (`--no-safety`) stays a build switch on purpose, `TENTACLE_DEBUG` builds the separate debug image.
- The OLED now uses GPIO5 (SDA) / GPIO6 (SCL), as documented; the generic ESP32-S3 build had used GPIO8/9.
- OLED: `no Wi-Fi: BOOT 10s=new` was one character too wide and is `no Wi-Fi: BOOT 10s` now.

## 0.3.0 (firmware) – unreleased

Not run on hardware yet.

- **Web interface** at `https://tentacle.local/`: the mirrored screen live (H.264 over a WebSocket, WebCodecs in
  the browser) and the phone under mouse and keyboard (tap, swipe, long press, scroll, system gestures, text and
  captured keys). English and German, dark and light. Password set on the setup page only (PBKDF2-SHA256),
  sign-in lock after 5 wrong tries, one browser at a time, a self-signed ECDSA certificate per board.
- **Sharing with the CLI:** while the page has control, the CLI's input commands get `err busy: …`; the stop toggle
  stops both.
- **Settings page:** board name, keyboard layout, language, Wi-Fi, firmware update with a signed image (the same
  checks as `tentacle update`), password change confirmed with BOOT.
- **Port 80** forwards to `https://` once the board is on a Wi-Fi.

## 0.1.0 – unreleased

Not run on hardware yet.

What differs from the project it started from:

- **Wi-Fi only.** The computer reaches the board on TCP 7202 and finds it by mDNS (`_tentacle._tcp`). Bluetooth,
  the iPhone app, its Wi-Fi video ports and updates over the air are gone; firmware goes on over USB.
- **Setup hotspot.** A board without a Wi-Fi opens `Tentacle-XXXX` (WPA2, a random password per board) with a
  setup page in English and German; the chosen network is stored only after a press of BOOT. Holding BOOT 10 s
  starts over.
- **Pairing needs a person at the board.** A double press of BOOT opens pairing for 60 s; the board and the
  computer show the same 6-digit code; one more press confirms. Failed requests lock pairing for a growing time.
- **Stop button and status LED** on by default (BOOT, short press); optional OLED (`--display`) and battery gauge.
- **AirPlay receiver:** overflow-safe size checks in the RTSP and binary-plist parsers (a crafted request could
  crash the board before), with a regression test and a fuzz harness.
- **CLI:** renamed to `tentacle`; `--verbose`; daemon and decoder fixes; `report` and `upgrade` removed.
- **Bring-up build** (`--debug`): a time-stamped state log and `dbg` commands on USB serial. Not for release.
- **New names on the wire:** HKDF labels `tentacle-pair-v1` / `tentacle-session-v1`, host name `tentacle`; the
  board shows as `Tentacle XXXX` to the CLI and under Screen Mirroring.
