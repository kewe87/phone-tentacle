# Android (und iPhone) mit einem Board

[English](android.md) · **Deutsch**

**Kurz gesagt:** Das Original-Projekt ist nur für das iPhone — hier gibt es keinen `android/`-Code.
Dasselbe Board funktioniert aber über ein Schwesterprojekt auch mit Android; nur die „Augen“ sind anders.

## Was gleich ist, was anders

Das Board ist immer nur die **Hände** (USB- bzw. Bluetooth-Tastatur + Maus). Je nach Plattform ändert sich,
wie der Agent seine **Augen** (den Bildschirm) bekommt und wie die Eingaben übertragen werden:

| | iPhone (dieses Repo / Handsy) | Android ([esp32-android-agent]) |
|---|---|---|
| Augen (Bildschirm) | AirPlay-Spiegelung, dekodiert in der iOS-App | **ADB-Screenshots** — keine App, kein Root |
| Hände (Eingabe) | USB-HID (ESP32-S3) | **USB-HID** (ESP32-S3) oder **Bluetooth-HID** (klassischer ESP32) |
| Zeiger | relativ + AssistiveTouch + Kalibrierung | nativer absoluter Zeiger (einfacher) |
| Begleit-App | nötig (TestFlight) | im ADB-Modus nicht nötig |

Ein **XIAO ESP32-S3 kann also beides** (USB-HID am iPhone *und* an Android), und ein einfacher
**klassischer ESP32-WROOM kann bei Android die „Hände“ über Bluetooth** übernehmen, obwohl er den
USB-Weg zum iPhone nicht kann.

## Weg für Android

**[shenlan-ai/esp32-android-agent]** nutzen (MIT, baut auf dem Bild- + HID-Ansatz dieses Projekts auf):

- Board-Profile: `esp32dev` (klassischer ESP32 → Bluetooth-HID) sowie `esp32_s3_devkitc` / `xiao_esp32s3`
  / `esp32_s3_zero` (→ natives USB-HID über OTG).
- Bildschirm über normale **ADB-Screenshots** (`adb` per USB/WLAN); keine Begleit-App, kein Root.
- Frühes Stadium (zum Zeitpunkt des Schreibens auf einem Android-15-Gerät getestet) — als experimentell betrachten.

## Könnte eine Firmware beides?

Das Protokoll für die *Hände* ist fast identisch, eine gemeinsame Firmware ist also denkbar. Heute sind es
getrennte Builds (verschiedene Repos). Eine gemeinsame „Hände“-Firmware für beide ist ein guter Punkt für die
Roadmap — siehe [ROADMAP-FORK.de.md](../ROADMAP-FORK.de.md).

---

Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan (MIT).

[esp32-android-agent]: https://github.com/shenlan-ai/esp32-android-agent
[shenlan-ai/esp32-android-agent]: https://github.com/shenlan-ai/esp32-android-agent
