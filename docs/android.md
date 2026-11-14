# Android

**English** · [Deutsch](android.de.md)

**Short answer:** Tentacle drives iPhones today. Android is planned, with the same board; there is no Android
code in this repository yet.

## What's the same, what's different

The board is the **hands** on both: a USB mouse and keyboard. What differs is how the computer gets its **eyes**
(the screen):

| | iPhone (Tentacle today) | Android (planned) |
|---|---|---|
| Eyes (screen) | AirPlay Screen Mirroring to the board, passed on to the computer | **ADB screenshots** over Wi-Fi (Wireless debugging, Android 11+); no app, no root |
| Hands (input) | USB HID from the board | USB HID from the same board |
| Pointer | relative mouse + AssistiveTouch + calibration | absolute pointer (the firmware has it as `flash.sh --abs`) |

The phone's USB port is taken by the board, so ADB has to go over Wi-Fi.

## Plan

- A second "eyes" backend in the `tentacle` CLI that takes screenshots through ADB, next to AirPlay.
- One firmware for both: the pointer mode chosen at run time, or a composite HID device with a relative and an
  absolute pointer.
- After the first hardware test with an iPhone. See [ROADMAP.md](../ROADMAP.md).

## Until then

**[shenlan-ai/esp32-android-agent]** (MIT) is a separate project for Android that builds on the same original
project as Tentacle: USB HID on ESP32-S3 boards or Bluetooth HID on a classic ESP32, screen through ADB
screenshots. It is not part of Tentacle and was not tried here.

---

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).

[shenlan-ai/esp32-android-agent]: https://github.com/shenlan-ai/esp32-android-agent
