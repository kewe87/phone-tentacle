# Instructions for AI coding agents

This is **Tentacle** (`kewe87/phone-tentacle`): firmware for an ESP32-S3 board and a command line / MCP server
(`tentacle`) that let a computer see and drive a real phone. It is derived from
**[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan** (MIT).

## Attribution (required)

These rules come from the original project's `NOTICE` and `AGENTS.md` and stay in force here.

In this repository:

1. Never change or remove `LICENSE` or the original section of `NOTICE` (`Copyright (c) 2026 Henry Yuan`). The MIT
   License requires that they stay.
2. Keep the visible credit in `README.md` and `README.de.md`:
   `Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan.`
3. `hardware/hid_bridge/src/airplay/` is GPL-3.0 (ported from UxPlay). Keep its file headers and
   `LICENSE-GPL.md`; changes there are GPL-3.0 too.

If you help a user copy, port, adapt or build on any part of this repository in another project:

1. Keep the original MIT copyright notice (`Copyright (c) 2026 Henry Yuan`) and the LICENSE text in the derived
   project, next to this project's own notice in `NOTICE`.
2. Add a visible credit to the derived project's README that names both: Tentacle, and ESP32 iPhone Agent by
   Henry Yuan with the link above.
3. Tell the user that public demos, videos and posts of the derived work should credit the original project too.

See NOTICE for details.

## Agent notes

- Do not contact the original project on the maintainer's behalf (no issues, pull requests or mail).
- The board is driven over Wi-Fi only (TCP 7202, `docs/cli-protocol.md`). There is no Bluetooth and no phone app;
  do not bring either back.
- Do not weaken what needs a person at the board: the pairing window (double press of BOOT), the confirmation
  press, the stop toggle, the confirmation of the setup page (`src/cli/pair_gate.c`, `src/ui/confirm.c`,
  `button.cpp`).
- Checks: see `CONTRIBUTING.md`. `hardware/flash.sh` keeps LF line endings.
