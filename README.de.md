# ESP32 iPhone Agent — gehärteter Fork

[English](README.md) · **Deutsch**

> **Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan** (MIT,
> Copyright © 2026 Henry Yuan). Die Idee, die Firmware, die iOS-App und die CLI sind sein Werk. Dieser Fork ergänzt
> nur Sicherheits-Fixes, sichere Voreinstellungen, Tests und optionale Hardware. Wie das Projekt funktioniert, die
> App und die vollständige Dokumentation: siehe die **[Original-README](https://github.com/onehumanbeing/esp32-iphone-agent#readme)**
> und [handsy.tech](https://handsy.tech) (jeweils auf Englisch).

[![fork-ci](https://github.com/kewe87/esp32-iphone-agent/actions/workflows/fork-ci.yml/badge.svg?branch=main)](https://github.com/kewe87/esp32-iphone-agent/actions/workflows/fork-ci.yml)
[![Lizenz: MIT](https://img.shields.io/badge/license-MIT-111111?style=flat-square)](LICENSE)
[![Upstream](https://img.shields.io/badge/upstream-onehumanbeing%2Fesp32--iphone--agent-111111?style=flat-square&logo=github)](https://github.com/onehumanbeing/esp32-iphone-agent)

**In einem Satz:** Ein ESP32-S3-Board, eingesteckt ins iPhone, ist AirPlay-Empfänger (Augen) und USB-Maus/-Tastatur
(Hände) — so kann eine KI das Handy bedienen, ohne Jailbreak und ohne Computer im Betrieb.

## So funktioniert's

```
iPhone ──Bildschirmsynchronisierung (WLAN)──▶ Board: AirPlay-Empfänger ──WLAN──▶ App auf demselben iPhone
   ▲                                          eigener Hotspot                    Bild → KI-Modell → Aktion
   └──────── USB-Maus + Tastatur ◀──────────── Befehle per Bluetooth ◀────────────────────┘
```

1. **Augen:** Das Board öffnet einen eigenen WLAN-Hotspot. Das iPhone spiegelt seinen Bildschirm per AirPlay
   dorthin; das Board reicht das Video an die App weiter.
2. **Gehirn:** Die App schickt Screenshots an ein KI-Bildmodell (Cloud-API oder eigener Server) und bekommt zurück,
   was als Nächstes zu tun ist: tippen, wischen, schreiben …
3. **Hände:** Die App sagt dem Board per Bluetooth, was es tun soll. Das Board ist für das iPhone eine ganz normale
   USB-Maus und -Tastatur; mit AssistiveTouch wird der Mauszeiger zum Finger.

**Bedienoberfläche:** Eine Web-Oberfläche gibt es nicht. Bedient wird über
- die **iPhone-App „ESP32 Agent“** ([TestFlight](https://testflight.apple.com/join/1xQHgJcA)) — Aufgabe eintippen,
  Start drücken, zuschauen (die App ist vom Original-Autor, Sprachen: Englisch und Chinesisch),
- **oder vom Computer aus** mit der Kommandozeile `esp32agent`, die auch als MCP-Server für KI-Assistenten wie
  Claude Code dient ([cli/README.md](cli/README.md)),
- und am Board selbst: **BOOT-Taster = Stopp**, LED als Statusanzeige, optional ein kleines OLED-Display.

Mehr Details: [How it works (Original, Englisch)](https://github.com/onehumanbeing/esp32-iphone-agent#how-it-works).

Dieser Fork ist weder mit dem Original-Projekt noch mit handsy.tech verbunden oder von ihnen unterstützt. Die
TestFlight-App und der Browser-Flasher stammen vom Original-Autor; der Browser-Flasher spielt die **Original**-Firmware
auf. Für die Änderungen unten die Firmware aus diesem Repository bauen.

## Was dieser Fork ändert

Basis: Original-Version [`v1.0.6`](https://github.com/onehumanbeing/esp32-iphone-agent/tree/v1.0.6)
(Board-Firmware `0.2.5`, CLI `0.1.5`). Jede Änderung ist in der Git-Historie nachvollziehbar.

| Bereich | Original v1.0.6 | Dieser Fork |
|---|---|---|
| 🔒 AirPlay-Sicherheit | Ein manipuliertes Paket von einem beliebigen Gerät im Hotspot kann die Größenprüfung austricksen → Lesen außerhalb des Speichers → Board stürzt ab (ohne Anmeldung) | Überlaufsichere Größenprüfung; zu große Eingaben werden abgewiesen |
| 🛑 Stopp-Taster | Stopp nur aus der App / Dynamic Island | **Standardmäßig an:** Der BOOT-Taster des Boards stoppt sofort alle Maus-/Tastatureingaben; die LED zeigt den Zustand |
| 🔐 Hotspot | Bis zu 4 Geräte können in den Hotspot | **Standardmäßig an:** nur ein Gerät (das iPhone) |
| 🪟 Windows-CLI | `esp32agent` findet unter Windows oft kein Board (Issue #114) | Board wird unter Windows auch am Namen erkannt |
| 📡 Bluetooth-Suche | Name nur in der Scan-Antwort | Kurzname direkt in der Ankündigung + schnelleres Intervall → wird schneller gefunden |
| 🛡️ CLI-Dienst | Eine fehlerhafte Anfrage kann den ganzen Dienst abstürzen lassen; Datenrennen im Video-Decoder | Eingaben geprüft, Abstürze abgefangen, Rennen behoben |
| 📟 Display | Status nur in der App | Optionales 0,96″-OLED: Name, IP, Verbindungen, RUNNING/STOPPED, Akku-% |
| 🧪 Tests | Original-CI | Zusätzlich Fuzz-Tests (libFuzzer/ASan) für den Parser und eine [Fork-CI](.github/workflows/fork-ci.yml), die alle Firmware-Varianten baut |

Noch offene Punkte (geringere Schwere) und geplante Funktionen: [ROADMAP-FORK.de.md](ROADMAP-FORK.de.md).

> [!NOTE]
> Stopp-Taster, Hotspot-Beschränkung und Display kompilieren fehlerfrei; der Test am echten Board läuft.

## Hardware

Ein Board für alle: der einfache **Seeed Studio XIAO ESP32S3** (8 MB PSRAM — *nicht* Sense, *nicht* Plus).
Klassische ESP32-Boards (z. B. ESP32-WROOM-32 mit CH340-Chip) **funktionieren nicht**: Ihnen fehlt natives USB,
sie können keine Maus/Tastatur sein.

**Basis** — mehr braucht man nicht:

| Teil | ca. Preis |
|---|---|
| Seeed Studio XIAO ESP32S3 (einfache Version) | 7–12 € |
| Kurzes USB-C ↔ USB-C-**Daten**kabel (Lightning-iPhones: Lightning-auf-USB-Kamera-Adapter) | 5–10 € |
| Gehäuse, optional (XIAO-ESP32S3-Gehäuse mit offener USB-C-Seite, z. B. Etsy/Printables) | 5–10 € |

Stopp-Taster und Hotspot-Beschränkung brauchen keine Zusatzteile — sie sind immer dabei.

**Erweitert** — optional Display und Akku, an eigenen Pins (die Basis-Version bleibt unberührt):

| Extra | Teil | Anschluss (XIAO) |
|---|---|---|
| 📟 Display | 0,96″-OLED 128×64, **SSD1306, I²C** (4 Pins, nicht die SPI-Version), 3–6 € | VCC→3V3, GND→GND, SDA→D4 (GPIO5), SCL→D5 (GPIO6) |
| 🔋 Akku | 3,7-V-LiPo (1S), ca. 500 mAh (z. B. 503035), 5–8 € | BAT+/BAT−-Lötpads auf der Rückseite; der XIAO lädt ihn über USB-C |
| 🔋 Akku-% | 2 × 100-kΩ-Widerstände | BAT+ → 100 kΩ → D0 (GPIO1) → 100 kΩ → GND |

- Der Akku ist im Normalbetrieb **nicht nötig**: Das iPhone versorgt das Board mit Strom. Er ist nur für den Betrieb ohne Kabel.
- Vor dem Löten die Polung prüfen (rot = BAT+); verpolt kann der Akku das Board zerstören.
- Mit Display oder Akku ein Gehäuse mit OLED-Fenster bzw. Akkufach wählen oder drucken lassen.
- Details und Kalibrierung: [docs/hardware-addons.de.md](docs/hardware-addons.de.md).

## Bauen und flashen

Benötigt [arduino-cli](https://arduino.github.io/arduino-cli/) mit dem ESP32-Core 3.3.x:

```bash
git clone https://github.com/kewe87/esp32-iphone-agent.git
cd esp32-iphone-agent
arduino-cli lib install "NimBLE-Arduino@2.5.1"
arduino-cli lib install "U8g2"                       # nur für --display

hardware/flash.sh                                    # Basis
hardware/flash.sh --display --battery-pin 1          # mit OLED (+ Akku-%; ohne Widerstände --battery-pin weglassen)
```

Abschalten, nur falls nötig: `--multi-client` lässt zusätzlich zum iPhone einen Computer in den Hotspot (sonst
nutzt die CLI Bluetooth oder nach `esp32agent net join` dein Heim-WLAN); `--no-safety` entfernt den Stopp-Taster.
Weitere Optionen (`--name`, `--debug`, …): siehe Kopf von [`hardware/flash.sh`](hardware/flash.sh).

## Benutzen

Unverändert gegenüber dem Original — dessen Anleitungen folgen:

1. App über [TestFlight](https://testflight.apple.com/join/1xQHgJcA) installieren und die vier Einrichtungsschritte
   durchgehen ([Anleitung mit Bildern](https://handsy.tech/esp32-iphone-agent-setup), Englisch): Board per
   Bluetooth koppeln → Hotspot beitreten und Bildschirmsynchronisierung auf „ESP32 Agent“ → AssistiveTouch
   einschalten → KI-Modell (Adresse + Schlüssel) eintragen.
2. Oder das Handy von einem KI-Assistenten auf dem Computer steuern lassen, über die `esp32agent`-CLI / den MCP-Server
   ([Original-Anleitung](https://github.com/onehumanbeing/esp32-iphone-agent#use-from-a-coding-agent),
   [cli/README.md](cli/README.md)).

**Stopp-Taster:** BOOT einmal drücken → alle Eingaben stoppen (LED leuchtet dauerhaft); nochmal drücken → es geht
weiter (LED blinkt langsam).

## Sicherheit

- Was gefunden und behoben wurde, und empfohlene Einstellungen: [docs/hardening.de.md](docs/hardening.de.md).
- Screenshots deines Handys gehen an das KI-Modell, das du einträgst — am besten ein lokales oder vertrauenswürdiges.
- Nur Geräte und Konten automatisieren, die dir gehören oder für die du die Erlaubnis hast.

Android: vom Original nicht unterstützt; Möglichkeiten siehe [docs/android.de.md](docs/android.de.md).

## Lizenz und Credits

Dies ist eine veränderte Version von **[ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von
Henry Yuan**. Der ursprüngliche Copyright- und Lizenzhinweis in [LICENSE](LICENSE) und [NOTICE](NOTICE) bleibt
unverändert erhalten.

- [MIT](LICENSE) — außer `hardware/hid_bridge/src/airplay/` (portiert aus [UxPlay](https://github.com/FDH2/UxPlay)),
  das unter [GPL-3.0](hardware/hid_bridge/src/airplay/LICENSE-GPL.md) steht; die Firmware-Datei, die es enthält,
  wird daher unter GPL-3.0 verbreitet. Dateien unter [`skills/`](skills/) stehen unter [MIT-0](skills/LICENSE).
- Die Änderungen dieses Forks (von [kewe87](https://github.com/kewe87), 2026) stehen unter denselben Bedingungen:
  MIT für MIT-lizenzierte Dateien, GPL-3.0 für geänderte Dateien unter `hardware/hid_bridge/src/airplay/` (z. B. `rtsp.c`).
- Danksagungen des Originals: [UxPlay](https://github.com/FDH2/UxPlay) ·
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) ·
  [Arduino ESP32 Core](https://github.com/espressif/arduino-esp32) · [TinyUSB](https://github.com/hathach/tinyusb).
  Dieser Fork nutzt zusätzlich [U8g2](https://github.com/olikraus/u8g2) für das Display.

Du baust auf diesem Fork auf? LICENSE behalten und **ESP32 iPhone Agent von Henry Yuan** nennen (siehe [NOTICE](NOTICE)) —
in deiner README und in öffentlichen Demos oder Posts.
