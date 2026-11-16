# Changelog

Tentacle's own history starts here. It is derived from
[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (the state of its tag
`v1.0.6`: board firmware 0.2.5, CLI 0.1.5); that project's history is in its repository.

## tentacle-flash – unreleased

Not run on hardware yet.

- **Firmware installer with nothing to install**: one file each for Windows, macOS (universal) and Linux
  (amd64, arm64), pure Go. A double click opens a three-step page on `127.0.0.1` (plug in, install, done with the
  setup Wi-Fi as a QR code), English and German; `--yes` does the same on the command line.
- Speaks the ESP32-S3 ROM bootloader itself (compressed writes, MD5 check), finds the board on the ROM's and on
  the firmware's USB port, checks chip, flash size and PSRAM, and keeps the stored Wi-Fi and the pairing.
- Checks the image's Ed25519 signature with the release key; unsigned images get a warning.

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
