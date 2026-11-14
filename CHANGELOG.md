# Changelog

Tentacle's own history starts here. It is derived from
[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (the state of its tag
`v1.0.6`: board firmware 0.2.5, CLI 0.1.5); that project's history is in its repository.

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
