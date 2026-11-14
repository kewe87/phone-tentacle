# Android (and iPhone) from one board

**English** · [Deutsch](android.de.md)

**Short answer:** this upstream project is iPhone-only — there is no `android/` code here.
But the same board works for Android through a sister project, and only the "eyes" differ.

## What's the same, what's different

The board is always just the **hands** (a USB/BLE keyboard + mouse). What changes per
platform is how the agent gets its **eyes** (the screen) and the input transport:

| | iPhone (this repo / Handsy) | Android ([esp32-android-agent]) |
|---|---|---|
| Eyes (screen) | AirPlay mirroring, decoded in the iOS app | **ADB screenshots** — no app, no root |
| Hands (input) | USB HID (ESP32-S3) | **USB HID** (ESP32-S3) or **BLE HID** (classic ESP32) |
| Pointer | relative + AssistiveTouch + calibration | native absolute pointer (simpler) |
| Companion app | required (TestFlight) | none needed in ADB mode |

So **one XIAO ESP32-S3 can serve both** (USB HID on iPhone *and* Android), and a plain
classic **ESP32-WROOM can act as the Android "hands" over BLE** even though it can't do the
iPhone USB path.

## Android path

Use **[shenlan-ai/esp32-android-agent]** (MIT, builds on this project's vision + HID approach):

- Board profiles: `esp32dev` (classic ESP32 → BLE HID), and `esp32_s3_devkitc` / `xiao_esp32s3`
  / `esp32_s3_zero` (→ native USB HID over OTG).
- Screen via standard **ADB screenshots** (`adb` over USB/Wi-Fi); no companion app, no root.
- Early-stage (tested on one Android 15 device at time of writing) — treat as experimental.

## Could one firmware do both?

The *hands* protocol is nearly identical, so a shared firmware is plausible. Today they are
separate builds (different repos). A unified "hands" firmware that both hosts can drive is a
good roadmap item — see `ROADMAP-FORK.md`.

---

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).

[esp32-android-agent]: https://github.com/shenlan-ai/esp32-android-agent
[shenlan-ai/esp32-android-agent]: https://github.com/shenlan-ai/esp32-android-agent
