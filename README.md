# ESP32 iPhone Agent — hardened fork

**English** · [Deutsch](README.de.md)

> **Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan** (MIT,
> Copyright © 2026 Henry Yuan). The idea, the firmware, the iOS app and the CLI are his work. This fork only adds
> security fixes, safer defaults, tests and optional hardware. For how the project works, the app and the full
> documentation, see the **[original README](https://github.com/onehumanbeing/esp32-iphone-agent#readme)** and
> [handsy.tech](https://handsy.tech).

[![fork-ci](https://github.com/kewe87/esp32-iphone-agent/actions/workflows/fork-ci.yml/badge.svg?branch=main)](https://github.com/kewe87/esp32-iphone-agent/actions/workflows/fork-ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-111111?style=flat-square)](LICENSE)
[![Upstream](https://img.shields.io/badge/upstream-onehumanbeing%2Fesp32--iphone--agent-111111?style=flat-square&logo=github)](https://github.com/onehumanbeing/esp32-iphone-agent)

**In one sentence:** an ESP32-S3 board plugged into the iPhone acts as AirPlay receiver (eyes) and USB
mouse/keyboard (hands), so an AI can use the phone — no jailbreak, no computer at run time.

## How it works

```
iPhone ──Screen Mirroring (Wi-Fi)──▶ Board: AirPlay receiver ──Wi-Fi──▶ App on the same iPhone
   ▲                                  own hotspot                       image → AI model → action
   └──────── USB mouse + keyboard ◀──────── commands over Bluetooth ◀─────────────┘
```

1. **Eyes:** the board opens its own Wi-Fi hotspot. The iPhone mirrors its screen to it over AirPlay; the board
   forwards the video to the app.
2. **Brain:** the app sends screenshots to a vision model (cloud API or your own server) and gets back what to do
   next: tap, swipe, type …
3. **Hands:** the app tells the board over Bluetooth what to do. To the iPhone the board is an ordinary USB mouse
   and keyboard; with AssistiveTouch the pointer becomes a finger.

**User interface:** there is no web interface. You control it through
- the **iPhone app "ESP32 Agent"** ([TestFlight](https://testflight.apple.com/join/1xQHgJcA)) — type a task, press
  Start, watch (the app is the original author's; languages: English and Chinese),
- **or from a computer** with the `esp32agent` command line, which is also an MCP server for AI assistants such as
  Claude Code ([cli/README.md](cli/README.md)),
- and on the board itself: **BOOT button = stop**, LED for status, optionally a small OLED display.

More detail: [how it works (upstream)](https://github.com/onehumanbeing/esp32-iphone-agent#how-it-works).

This fork is not affiliated with or endorsed by the original project or handsy.tech. The TestFlight app and the
browser flasher are the original author's; the browser flasher installs the **upstream** firmware, so build from
this repository to get the changes below.

## What this fork changes

Base: upstream tag [`v1.0.6`](https://github.com/onehumanbeing/esp32-iphone-agent/tree/v1.0.6) (board firmware
`0.2.5`, CLI `0.1.5`). Every change is in the git history.

| Area | Upstream v1.0.6 | This fork |
|---|---|---|
| 🔒 AirPlay security | A crafted `Content-Length` or binary plist from any device on the hotspot can wrap the size checks → out-of-bounds read → board crash/reboot (pre-auth DoS) | Overflow-safe size checks; oversized input is rejected |
| 🛑 Stop button | Stop only from the app / Dynamic Island | **On by default:** the board's BOOT button stops all mouse/keyboard input; the LED shows the state |
| 🔐 Hotspot | Up to 4 devices can join the board's hotspot | **On by default:** only one device (the phone) can join |
| 🪟 Windows CLI | `esp32agent` often finds no board on Windows (issue #114) | Board is also matched by name on Windows |
| 📡 BLE discovery | Name only in the scan response | Short name in the main advertisement + faster interval → found sooner |
| 🛡️ CLI daemon | A malformed request can crash the whole daemon; data race in the video decoder | Inputs checked, panics caught, race fixed |
| 📟 Display | Status only in the app | Optional 0.96″ OLED: name, IP, connections, RUNNING/STOPPED, battery % |
| 🧪 Testing | Upstream CI | Plus libFuzzer/ASan harness for the plist parser and a [fork CI](.github/workflows/fork-ci.yml) building every firmware variant |

Not yet fixed (lower severity) and planned features: [ROADMAP-FORK.md](ROADMAP-FORK.md).

> [!NOTE]
> Stop button, single-client hotspot and display are compile-verified; on-hardware testing is in progress.

## Hardware

One board for everyone: the plain **Seeed Studio XIAO ESP32S3** (8 MB PSRAM — *not* Sense, *not* Plus). Classic
ESP32 boards (e.g. ESP32-WROOM-32 with a CH340 chip) **do not work**: they have no native USB, so they can't be
a mouse/keyboard.

**Basic** — all you need:

| Part | Approx. price |
|---|---|
| Seeed Studio XIAO ESP32S3 (plain) | 7–12 € |
| Short USB-C ↔ USB-C **data** cable (Lightning iPhones: Lightning to USB Camera Adapter) | 5–10 € |
| Case, optional (XIAO ESP32S3 case with the USB-C end open, e.g. Etsy/Printables) | 5–10 € |

Stop button and single-client hotspot need no extra parts — they are always included.

**Extended** — optional display and battery, on separate pins (the basic build is unaffected):

| Add-on | Part | Wiring (XIAO) |
|---|---|---|
| 📟 Display | 0.96″ OLED 128×64, **SSD1306, I²C** (4 pins, not the SPI version), 3–6 € | VCC→3V3, GND→GND, SDA→D4 (GPIO5), SCL→D5 (GPIO6) |
| 🔋 Battery | 3.7 V 1S LiPo, ~500 mAh (e.g. 503035), 5–8 € | BAT+/BAT− pads on the back; the XIAO charges it over USB-C |
| 🔋 Battery % | 2 × 100 kΩ resistors | BAT+ → 100 kΩ → D0 (GPIO1) → 100 kΩ → GND |

- The battery is **not needed** for normal use: the iPhone powers the board. It's for running the board unplugged.
- Check the LiPo polarity (red = BAT+) before soldering; reversed, it can destroy the board.
- With display or battery, pick a case with an OLED window / battery bay, or print one.
- Details and calibration: [docs/hardware-addons.md](docs/hardware-addons.md).

## Build and flash

Needs [arduino-cli](https://arduino.github.io/arduino-cli/) with the ESP32 core 3.3.x:

```bash
git clone https://github.com/kewe87/esp32-iphone-agent.git
cd esp32-iphone-agent
arduino-cli lib install "NimBLE-Arduino@2.5.1"
arduino-cli lib install "U8g2"                       # only for --display

hardware/flash.sh                                    # basic
hardware/flash.sh --display --battery-pin 1          # with OLED (+ battery %; drop --battery-pin without resistors)
```

Opt-outs, only if you need them: `--multi-client` lets a computer join the board's hotspot next to the phone
(otherwise the CLI uses Bluetooth, or your home Wi-Fi after `esp32agent net join`); `--no-safety` removes the
stop button. Other options (`--name`, `--debug`, …): see the header of [`hardware/flash.sh`](hardware/flash.sh).

## Use it

Unchanged from upstream — follow the original docs:

1. Install the app from [TestFlight](https://testflight.apple.com/join/1xQHgJcA) and run its four setup steps
   ([setup guide](https://handsy.tech/esp32-iphone-agent-setup)).
2. Or drive the phone from a coding agent with the `esp32agent` CLI / MCP server
   ([upstream instructions](https://github.com/onehumanbeing/esp32-iphone-agent#use-from-a-coding-agent),
   [cli/README.md](cli/README.md)).

**Stop button:** press BOOT once → all input stops (LED solid); press again → it continues (LED blinks slowly).

## Security

- What was found and fixed, and recommended settings: [docs/hardening.md](docs/hardening.md).
- Screenshots of your phone go to the vision model you configure — prefer a local or trusted one.
- Only automate devices and accounts you own or are allowed to use.

Android: not supported by upstream; see [docs/android.md](docs/android.md) for options.

## License and credits

This is a modified version of **[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by
Henry Yuan**. The original copyright and permission notice in [LICENSE](LICENSE) and [NOTICE](NOTICE) are kept
unchanged.

- [MIT](LICENSE) — except `hardware/hid_bridge/src/airplay/` (ported from [UxPlay](https://github.com/FDH2/UxPlay)),
  which is [GPL-3.0](hardware/hid_bridge/src/airplay/LICENSE-GPL.md); the firmware binary that links it is
  distributed under GPL-3.0. Files under [`skills/`](skills/) are [MIT-0](skills/LICENSE).
- The fork's modifications (by [kewe87](https://github.com/kewe87), 2026) are released under the same terms:
  MIT for MIT-licensed files, GPL-3.0 for modified files under `hardware/hid_bridge/src/airplay/` (e.g. `rtsp.c`).
- Upstream acknowledgements: [UxPlay](https://github.com/FDH2/UxPlay) ·
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) ·
  [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) · [TinyUSB](https://github.com/hathach/tinyusb).
  This fork also uses [U8g2](https://github.com/olikraus/u8g2) for the display.

Building on this fork? Keep the LICENSE and credit **ESP32 iPhone Agent by Henry Yuan** (see [NOTICE](NOTICE)) —
in your README and in public demos or posts.
