# Android

[English](android.md) · **Deutsch**

**Kurz:** Tentacle bedient heute iPhones. Android ist geplant, mit demselben Board; Android-Code gibt es in
diesem Repository noch nicht.

## Was gleich ist, was anders

Das Board ist bei beiden die **Hände**: eine USB-Maus und -Tastatur. Der Unterschied liegt darin, wie der
Computer zu seinen **Augen** (dem Bildschirm) kommt:

| | iPhone (Tentacle heute) | Android (geplant) |
|---|---|---|
| Augen (Bildschirm) | AirPlay-Bildschirmsynchronisierung zum Board, von dort zum Computer | **ADB-Screenshots** über WLAN (Kabelloses Debugging, ab Android 11); keine App, kein Root |
| Hände (Eingabe) | USB-HID vom Board | USB-HID vom selben Board |
| Zeiger | relative Maus + AssistiveTouch + Kalibrierung | absoluter Zeiger (die Firmware hat ihn als `flash.sh --abs`) |

Der USB-Anschluss des Handys ist vom Board belegt, deshalb muss ADB über WLAN laufen.

## Plan

- Ein zweites „Augen“-Backend in der CLI `tentacle`, das Screenshots per ADB holt, neben AirPlay.
- Eine Firmware für beide: Zeigermodus zur Laufzeit wählbar, oder ein zusammengesetztes HID-Gerät mit relativem
  und absolutem Zeiger.
- Nach dem ersten Hardware-Test mit einem iPhone. Siehe [ROADMAP.de.md](../ROADMAP.de.md).

## Bis dahin

**[shenlan-ai/esp32-android-agent]** (MIT) ist ein eigenes Projekt für Android, das auf demselben Originalprojekt
aufbaut wie Tentacle: USB-HID auf ESP32-S3-Boards oder Bluetooth-HID auf einem klassischen ESP32, Bildschirm über
ADB-Screenshots. Es gehört nicht zu Tentacle und wurde hier nicht ausprobiert.

---

Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan (MIT).

[shenlan-ai/esp32-android-agent]: https://github.com/shenlan-ai/esp32-android-agent
