**English** | [Deutsch](README.de.md)

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/brand/logo-wordmark-dark.png">
    <img src="docs/brand/logo-wordmark.png" alt="Tentacle" width="460">
  </picture>
</p>

<p align="center">
  <b>Your PC's arm into a real phone.</b><br>
  A thumbnail-sized ESP32-S3 board that plugs into an iPhone: USB mouse and keyboard for the hands, an AirPlay receiver for the eyes.<br>
  Driven from your computer over Wi-Fi, by you or by an AI assistant. No app on the phone, no jailbreak, no Bluetooth, no cloud.
</p>

<p align="center">
  <a href="https://github.com/kewe87/phone-tentacle/actions/workflows/ci.yml"><img src="https://github.com/kewe87/phone-tentacle/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT%20%2B%20GPL--3.0%20(AirPlay%20part)-c7361f" alt="License: MIT, AirPlay part GPL-3.0"></a>
  <img src="https://img.shields.io/badge/status-not%20yet%20tested%20on%20hardware-c7361f" alt="Status: not yet tested on hardware">
</p>

> **Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).**
> The idea, the AirPlay port, the USB side and the command line come from his project. Tentacle is a separate
> project derived from it, not affiliated with or endorsed by it. Details: [Origin and license](#origin-and-license).

> [!WARNING]
> **Status: not yet tested on hardware.** Everything here compiles and passes its tests on the computer (host
> tests, a simulated board, the CLI's tests), but no build of Tentacle has run on a real board yet. The first
> hardware test is planned for 17 November 2026. Until this note is gone, expect things that do not work.

---

## What is Tentacle?

A small board sits on the phone's USB-C port. To the phone it is an ordinary mouse and keyboard, and a screen
it can mirror to. To your computer it is a device on the Wi-Fi that shows the phone's screen and takes taps,
swipes and text.

- **Hands:** the board is a USB mouse and keyboard (with AssistiveTouch the pointer is a finger)
- **Eyes:** the board receives the phone's Screen Mirroring (AirPlay) and passes the picture on
- **From the computer:** the `tentacle` command line, which is also an MCP server for AI assistants
- **Wi-Fi only:** the computer finds the board by itself (mDNS) and talks to it on one encrypted port
- **Someone has to be at the board:** setting up its Wi-Fi and pairing a computer need a press of its button
- **A stop button:** one press of BOOT and the board sends no more input

```
iPhone ── Screen Mirroring (AirPlay, Wi-Fi) ──▶ board ── TCP 7202, encrypted ──▶ computer: tentacle
   ▲                                             │                                 screenshot → you or an AI
   └────────── USB mouse + keyboard ◀────────────┘ ◀──────── tap, swipe, type ─────────────┘
```

## What you need

| Part | Notes |
|---|---|
| **Seeed Studio XIAO ESP32S3** (the plain one, 8 MB PSRAM) | Not the Sense, not the Plus. Classic ESP32 boards (ESP32-WROOM with a CH340 chip) do not work: no native USB, so they cannot be a mouse or keyboard. |
| **USB-C to USB-C data cable**, short | A charge-only cable does not work. |
| **iPhone with USB-C**, iOS 18 or later | Lightning iPhones through a Lightning to USB Camera Adapter: taken over from the original project, not tried here. |
| **0.96″ OLED, 128×64, SSD1306, I²C** (4 pins) — recommended | Shows the setup Wi-Fi and its password, the board's address, and the **pairing code**. Without it the code cannot be compared at the board (see [Security](#security)). |
| A computer on the same Wi-Fi | Windows, macOS or Linux, with [Go](https://go.dev/dl/) to build the CLI and `ffmpeg` for screenshots. |
| Optional: LiPo 3.7 V, two 100 kΩ resistors, a case | For running the board unplugged and a battery gauge: [docs/hardware-addons.md](docs/hardware-addons.md). |

OLED wiring: VCC→3V3, GND→GND, SDA→D4 (GPIO5), SCL→D5 (GPIO6).

## Setup, step by step

### 1. Flash the firmware

Needs [arduino-cli](https://arduino.github.io/arduino-cli/) with the ESP32 core 3.3.x (and the U8g2 library for
the OLED):

```bash
git clone https://github.com/kewe87/phone-tentacle.git
cd phone-tentacle
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install U8g2                 # only for --display

hardware/flash.sh --display                  # with the OLED
hardware/flash.sh                            # without
```

Plug the board into the computer first. If the upload fails, put it into download mode (hold BOOT while
plugging it in) and run the script again. `hardware/flash.sh` is a bash script (Linux, macOS, WSL, Git Bash);
its header lists every option (`--name`, `--abs`, `--battery-pin`, `--no-safety` …).

After flashing, the script asks the board for its **setup Wi-Fi** and prints the name and password, for example
`Tentacle-1A2B`. Both are also on the OLED.

### 2. Put the board on your Wi-Fi

1. Join the Wi-Fi `Tentacle-XXXX` with a phone or laptop, using the password from step 1.
2. The setup page opens by itself (if not: `http://192.168.4.1/`). It is in English or German, after your browser.
3. Choose your home Wi-Fi and enter its password.
4. **Press the BOOT button on the board within 30 seconds.** Only then is the network stored.

The setup Wi-Fi then disappears and the board joins your network. The OLED shows its address.
To start over at any time: hold BOOT for 10 seconds.

### 3. Build the CLI

```bash
cd cli
go build -o tentacle ./cmd/tentacle          # Windows: -o tentacle.exe
```

Put the binary somewhere on your `PATH`. Screenshots need `ffmpeg` (`winget install Gyan.FFmpeg`,
`brew install ffmpeg`, `apt install ffmpeg`).

### 4. Pair the computer

```bash
tentacle devices        # the board should be listed
tentacle pair
```

1. **Double-press BOOT** on the board (two quick presses). Pairing is now open for 60 seconds; the LED flashes
   twice a second and the OLED says `PAIRING OPEN`.
2. `tentacle pair` prints a **6-digit code**. The OLED shows the same code and your computer's name.
3. Compare them. If they match, **press BOOT once more** within 30 seconds.

From then on this computer needs no button press. A board remembers up to four computers.

### 5. Plug it into the iPhone and mirror the screen

1. Unplug the board from the computer and plug it into the iPhone.
2. On the iPhone: *Settings › Accessibility › Touch › AssistiveTouch* on. That makes the mouse pointer tap.
3. The iPhone has to be on the same Wi-Fi as the board. Open Control Center › **Screen Mirroring** ›
   **Tentacle XXXX**.

iOS starts Screen Mirroring only by hand; the board cannot do that for you.

### 6. First screenshot, first tap

```bash
tentacle screenshot             # writes a PNG and prints its path
tentacle io button HOME         # go to the Home Screen
tentacle io tap 240,520         # coordinates are pixels of the screenshot
tentacle io text 'hello'
```

The first tap measures the pointer by itself (`tentacle calibrate`). All commands:
[cli/README.md](cli/README.md).

## Use it with an AI assistant (MCP)

`tentacle mcp` is an MCP server on stdio. Any MCP client can use it, for example Claude Code or Codex:

```bash
claude mcp add tentacle -- tentacle mcp
codex mcp add tentacle -- tentacle mcp
tentacle skill install          # optional: a skill with the working loop and typing rules, for Claude Code
```

Other clients: `{ "mcpServers": { "tentacle": { "command": "tentacle", "args": ["mcp"] } } }`.

Tools: `screenshot`, `tap`, `longpress`, `swipe`, `type_text`, `press_key`, `press_button`, `device_info`, plus
`history`, `intent_list`, `intent_show`, `intent_save`, `memory_recall`, `memory_add`. The loop is always:
screenshot, decide, one action, screenshot again.

Screenshots of your phone go to whatever model your assistant uses. Keep that in mind before you open your
banking app.

## Security

Tentacle moves the pointer on a real phone with your accounts on it. The design in short:

- **One way in from the computer:** TCP 7202, authenticated and encrypted (X25519, AES-GCM). No Bluetooth.
- **Pairing needs a person at the board:** a double press of BOOT opens it for 60 seconds, a 6-digit code is
  compared, one more press confirms. Nothing on the network can make the board ask for its button.
- **Wi-Fi setup needs the button too**, and the setup Wi-Fi has a random password per board.
- **Stop button:** a short press of BOOT stops all mouse and keyboard output (LED on = stopped). Keep the board
  within reach while an assistant drives the phone.
- **Open on your network:** the AirPlay receiver (TCP 7000 and its data ports), the CLI port 7202 and mDNS.
  Anyone on the same Wi-Fi can start mirroring to the board or try to crash it; only a paired computer can see
  the screen or send input.
- **Without the OLED** you cannot compare the pairing code. The presses then only say "a computer may pair now".

All of it, with what is left open: **[docs/hardening.md](docs/hardening.md)**. Reporting a problem:
[SECURITY.md](SECURITY.md).

Only automate devices and accounts that are yours or that you are allowed to use.

## Status

| | |
|---|---|
| Firmware builds (default, `--display`, `--no-safety`, `--debug`) | yes |
| Host tests, simulated board, CLI tests | pass |
| Run on a real board | **not yet** |
| Setup hotspot, pairing, mirroring, taps on a real iPhone | **not yet tested** |
| Releases, installers, npm package | none yet; build from source |

## Roadmap

- First run on hardware, then fixes
- **Web remote on the board:** live picture, mouse and keyboard in the browser, nothing to install on the computer
- **Android:** the same board as the hands, screenshots over ADB ([docs/android.md](docs/android.md))

More: [ROADMAP.md](ROADMAP.md). Changes: [CHANGELOG.md](CHANGELOG.md).

## Documentation

- [cli/README.md](cli/README.md) — the `tentacle` command line and MCP server
- [docs/hardening.md](docs/hardening.md) — security model and settings
- [docs/hardware-addons.md](docs/hardware-addons.md) — OLED, battery, case
- [docs/cli-protocol.md](docs/cli-protocol.md) — what goes over the wire
- [docs/android.md](docs/android.md) — Android, planned

## Origin and license

**Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan**
(MIT, Copyright © 2026 Henry Yuan). His copyright and permission notice stay unchanged in [LICENSE](LICENSE) and
[NOTICE](NOTICE). Tentacle removed the iPhone app, Bluetooth and over-the-air updates from that code and added
the Wi-Fi-only link, the setup hotspot, pairing with the button, the stop button and the display.

- **MIT** ([LICENSE](LICENSE)) for everything except the directory below. Tentacle's own changes: Copyright ©
  2026 kewe87, under the same terms (see [NOTICE](NOTICE)).
- **GPL-3.0** for `hardware/hid_bridge/src/airplay/`, the AirPlay receiver ported from
  [UxPlay](https://github.com/FDH2/UxPlay) ([LICENSE-GPL.md](hardware/hid_bridge/src/airplay/LICENSE-GPL.md)).
  Every firmware binary links that directory, so **a firmware binary as a whole is GPL-3.0**: if you pass one
  on (a download, a pre-flashed board), you have to offer the complete matching source code under GPL-3.0 and
  keep the license notices with it. The `tentacle` CLI does not contain that code and stays MIT.
- Files under [`skills/`](skills/) are [MIT-0](skills/LICENSE).
- Built on: [UxPlay](https://github.com/FDH2/UxPlay) ·
  [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) · [TinyUSB](https://github.com/hathach/tinyusb) ·
  [U8g2](https://github.com/olikraus/u8g2).

**If you build on Tentacle:** keep `LICENSE` and `NOTICE`, and credit the original visibly, in your README and
in public demos, videos and posts:

> Based on ESP32 iPhone Agent by Henry Yuan — https://github.com/onehumanbeing/esp32-iphone-agent

Apple, iPhone and AirPlay are trademarks of Apple Inc. Tentacle is not an Apple product and is not connected to
Apple. The name has nothing to do with the deployment agent of the same name by Octopus Deploy.
