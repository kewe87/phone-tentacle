# Optionale Hardware-Erweiterungen

[English](hardware-addons.md) · **Deutsch**

Standardmäßig aus; jedes Extra muss angeschlossen sein. Die Firmware-Unterstützung ist eingebaut — einschalten
per Build-Schalter. Der Test am echten Board läuft noch. (Stopp-Taster und Hotspot-Beschränkung brauchen keine
Teile und sind standardmäßig an; siehe [hardening.de.md](hardening.de.md).)

## Status-OLED (128×64 SSD1306, I²C) — `--display`
Ein 0,96″-SSD1306-OLED (wenige Euro) zeigt Board-Name + Firmware, Netzwerkmodus + IP, Anzahl der Verbindungen
und **RUNNING / STOPPED** (gekoppelt an den BOOT-Stopp-Taster).

Anschluss (Standard-I²C-Pins des XIAO ESP32-S3):

| OLED | XIAO |
|------|------|
| VCC  | 3V3  |
| GND  | GND  |
| SDA  | D4 (GPIO5) |
| SCL  | D5 (GPIO6) |

Bauen: `hardware/flash.sh --display`. Einmalig die U8g2-Bibliothek installieren: `arduino-cli lib install U8g2`.

## Akku-Anzeige (LiPo) — `--battery-pin <gpio>`
Misst die Akkuspannung über einen Spannungsteiler an einem Analog-Pin und zeigt grob die Prozent auf dem OLED.

- Verdrahten: `BAT+ → 100 kΩ → D0 (GPIO1) → 100 kΩ → GND` (zwei gleiche Widerstände = Teiler 2:1; hält den
  Pin unter 3,3 V).
- Bauen: `hardware/flash.sh --display --battery-pin 1`. Bei anderem Teilerverhältnis `BATTERY_DIVIDER` in
  `hardware/hid_bridge/hid_bridge.ino` anpassen.
- Die Prozentkurve ist grob 3,3 V = 0 % … 4,2 V = 100 %; für den eigenen Akku kalibrieren.
- Der XIAO ESP32-S3 lädt einen LiPo an seinen BAT-Pads, so laufen Board + Handy auch unterwegs.

## Gehäuse
Auf Etsy/Printables gibt es Gehäuse für den XIAO ESP32-S3 (einfache Version, USB-C-Seite offen). Mit OLED oder
Akku ein Gehäuse mit OLED-Fenster bzw. Akkufach wählen oder drucken lassen.
