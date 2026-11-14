[English](README.md) | **Deutsch**

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/brand/logo-wordmark-dark.png">
    <img src="docs/brand/logo-wordmark.png" alt="Tentacle" width="460">
  </picture>
</p>

<p align="center">
  <b>Der Fangarm deines PCs ins echte Handy.</b><br>
  Ein daumennagelgroßes ESP32-S3-Board, das am iPhone steckt: USB-Maus und -Tastatur als Hände, ein AirPlay-Empfänger als Augen.<br>
  Gesteuert vom Computer über WLAN, von dir oder von einem KI-Assistenten. Keine App auf dem Handy, kein Jailbreak, kein Bluetooth, keine Cloud.
</p>

<p align="center">
  <a href="https://github.com/kewe87/phone-tentacle/actions/workflows/ci.yml"><img src="https://github.com/kewe87/phone-tentacle/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/Lizenz-MIT%20%2B%20GPL--3.0%20(AirPlay--Teil)-c7361f" alt="Lizenz: MIT, AirPlay-Teil GPL-3.0"></a>
  <img src="https://img.shields.io/badge/Status-noch%20nicht%20auf%20Hardware%20getestet-c7361f" alt="Status: noch nicht auf Hardware getestet">
</p>

> **Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan (MIT).**
> Die Idee, die AirPlay-Portierung, die USB-Seite und die Kommandozeile stammen aus seinem Projekt. Tentacle ist
> ein eigenes, davon abgeleitetes Projekt und weder mit ihm verbunden noch von ihm unterstützt. Einzelheiten:
> [Herkunft und Lizenz](#herkunft-und-lizenz).

> [!WARNING]
> **Status: noch nicht auf Hardware getestet.** Alles hier kompiliert und besteht seine Tests am Computer
> (Host-Tests, ein simuliertes Board, die Tests der CLI), aber noch kein Tentacle-Build ist auf einem echten
> Board gelaufen. Der erste Hardware-Test ist für den 17. November 2026 geplant. Solange dieser Hinweis hier
> steht, kann einiges nicht funktionieren.

---

## Was ist Tentacle?

Ein kleines Board steckt am USB-C-Anschluss des Handys. Für das Handy ist es eine gewöhnliche Maus und Tastatur
und ein Bildschirm, auf den es spiegeln kann. Für deinen Computer ist es ein Gerät im WLAN, das den Bildschirm
des Handys zeigt und Tippen, Wischen und Text entgegennimmt.

- **Hände:** Das Board ist USB-Maus und -Tastatur (mit AssistiveTouch wird der Zeiger zum Finger)
- **Augen:** Das Board empfängt die Bildschirmsynchronisierung (AirPlay) des Handys und reicht das Bild weiter
- **Vom Computer aus:** die Kommandozeile `tentacle`, die zugleich MCP-Server für KI-Assistenten ist
- **Nur WLAN:** Der Computer findet das Board von selbst (mDNS) und spricht über einen verschlüsselten Port mit ihm
- **Jemand muss am Board stehen:** WLAN-Einrichtung und Koppeln eines Computers brauchen einen Druck auf seine Taste
- **Ein Stopp-Taster:** einmal BOOT drücken, und das Board schickt keine Eingaben mehr

```
iPhone ── Bildschirmsynchronisierung (AirPlay, WLAN) ──▶ Board ── TCP 7202, verschlüsselt ──▶ Computer: tentacle
   ▲                                                      │                                    Screenshot → du oder eine KI
   └────────────── USB-Maus + -Tastatur ◀─────────────────┘ ◀──────── tippen, wischen, schreiben ──────────┘
```

## Was du brauchst

| Teil | Hinweise |
|---|---|
| **Seeed Studio XIAO ESP32S3** (die einfache Version, 8 MB PSRAM) | Nicht Sense, nicht Plus. Klassische ESP32-Boards (ESP32-WROOM mit CH340-Chip) funktionieren nicht: kein natives USB, also keine Maus oder Tastatur. |
| **USB-C-auf-USB-C-Datenkabel**, kurz | Ein reines Ladekabel funktioniert nicht. |
| **iPhone mit USB-C**, iOS 18 oder neuer | Lightning-iPhones über einen Lightning-auf-USB-Kamera-Adapter: vom Originalprojekt übernommen, hier nicht ausprobiert. |
| **0,96″-OLED, 128×64, SSD1306, I²C** (4 Pins) — empfohlen | Zeigt das Setup-WLAN mit Passwort, die Adresse des Boards und den **Koppel-Code**. Ohne OLED lässt sich der Code am Board nicht vergleichen (siehe [Sicherheit](#sicherheit)). |
| Ein Computer im selben WLAN | Windows, macOS oder Linux, mit [Go](https://go.dev/dl/) zum Bauen der CLI und `ffmpeg` für Screenshots. |
| Optional: LiPo 3,7 V, zwei 100-kΩ-Widerstände, Gehäuse | Für den Betrieb ohne Kabel und eine Akku-Anzeige: [docs/hardware-addons.de.md](docs/hardware-addons.de.md). |

OLED-Anschluss: VCC→3V3, GND→GND, SDA→D4 (GPIO5), SCL→D5 (GPIO6).

## Einrichtung, Schritt für Schritt

### 1. Firmware flashen

Benötigt [arduino-cli](https://arduino.github.io/arduino-cli/) mit dem ESP32-Core 3.3.x (und die Bibliothek U8g2
für das OLED):

```bash
git clone https://github.com/kewe87/phone-tentacle.git
cd phone-tentacle
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install U8g2                 # nur für --display

hardware/flash.sh --display                  # mit OLED
hardware/flash.sh                            # ohne
```

Vorher das Board an den Computer stecken. Schlägt das Hochladen fehl, das Board in den Download-Modus bringen
(BOOT halten und dabei einstecken) und das Skript noch einmal starten. `hardware/flash.sh` ist ein Bash-Skript
(Linux, macOS, WSL, Git Bash); alle Optionen stehen in seinem Kopf (`--name`, `--abs`, `--battery-pin`,
`--no-safety` …).

Nach dem Flashen fragt das Skript das Board nach seinem **Setup-WLAN** und zeigt Name und Passwort an, zum
Beispiel `Tentacle-1A2B`. Beides steht auch auf dem OLED.

### 2. Das Board ins eigene WLAN bringen

1. Mit Handy oder Laptop ins WLAN `Tentacle-XXXX` gehen, mit dem Passwort aus Schritt 1.
2. Die Einrichtungsseite öffnet sich von selbst (sonst: `http://192.168.4.1/`). Sie ist deutsch oder englisch,
   je nach Browser.
3. Das Heim-WLAN auswählen und sein Passwort eingeben.
4. **Innerhalb von 30 Sekunden die BOOT-Taste am Board drücken.** Erst dann wird das Netz gespeichert.

Das Setup-WLAN verschwindet danach, das Board geht in dein Netz. Das OLED zeigt seine Adresse.
Neu anfangen geht jederzeit: BOOT 10 Sekunden halten.

### 3. Die CLI bauen

```bash
cd cli
go build -o tentacle ./cmd/tentacle          # Windows: -o tentacle.exe
```

Die Datei in einen Ordner legen, der im `PATH` steht. Screenshots brauchen `ffmpeg`
(`winget install Gyan.FFmpeg`, `brew install ffmpeg`, `apt install ffmpeg`).

### 4. Den Computer koppeln

```bash
tentacle devices        # das Board sollte in der Liste stehen
tentacle pair
```

1. Am Board **BOOT doppelt drücken** (zweimal kurz hintereinander). Das Koppeln ist jetzt 60 Sekunden offen; die
   LED blitzt zweimal pro Sekunde, das OLED zeigt `PAIRING OPEN`.
2. `tentacle pair` zeigt einen **6-stelligen Code**. Das OLED zeigt denselben Code und den Namen deines Computers.
3. Vergleichen. Stimmen sie überein, innerhalb von 30 Sekunden **BOOT noch einmal drücken**.

Danach braucht dieser Computer keinen Tastendruck mehr. Ein Board merkt sich bis zu vier Computer.

### 5. Ans iPhone stecken und den Bildschirm spiegeln

1. Das Board vom Computer abziehen und ans iPhone stecken.
2. Am iPhone: *Einstellungen › Bedienungshilfen › Tippen › AssistiveTouch* einschalten. Damit kann der Mauszeiger
   tippen.
3. Das iPhone muss im selben WLAN sein wie das Board. Kontrollzentrum › **Bildschirmsynchronisierung** ›
   **Tentacle XXXX** wählen.

iOS startet die Bildschirmsynchronisierung nur von Hand; das Board kann das nicht übernehmen.

### 6. Erster Screenshot, erster Tipp

```bash
tentacle screenshot             # schreibt ein PNG und zeigt den Pfad
tentacle io button HOME         # zum Home-Bildschirm
tentacle io tap 240,520         # Koordinaten sind Pixel des Screenshots
tentacle io text 'hello'
```

Beim ersten Tipp vermisst die CLI den Zeiger von selbst (`tentacle calibrate`). Alle Befehle:
[cli/README.md](cli/README.md) (englisch).

## Nutzung mit einem KI-Assistenten (MCP)

`tentacle mcp` ist ein MCP-Server über stdio. Jeder MCP-Client kann ihn nutzen, zum Beispiel Claude Code oder
Codex:

```bash
claude mcp add tentacle -- tentacle mcp
codex mcp add tentacle -- tentacle mcp
tentacle skill install          # optional: ein Skill mit Arbeitsschleife und Tippregeln, für Claude Code
```

Andere Clients: `{ "mcpServers": { "tentacle": { "command": "tentacle", "args": ["mcp"] } } }`.

Werkzeuge: `screenshot`, `tap`, `longpress`, `swipe`, `type_text`, `press_key`, `press_button`, `device_info`,
dazu `history`, `intent_list`, `intent_show`, `intent_save`, `memory_recall`, `memory_add`. Die Schleife ist immer:
Screenshot, entscheiden, eine Aktion, wieder Screenshot.

Die Screenshots deines Handys gehen an das Modell, das dein Assistent benutzt. Daran denken, bevor du die
Banking-App öffnest.

## Sicherheit

Tentacle bewegt den Zeiger auf einem echten Handy mit deinen Konten. Das Konzept in Kürze:

- **Ein Weg vom Computer hinein:** TCP 7202, authentifiziert und verschlüsselt (X25519, AES-GCM). Kein Bluetooth.
- **Koppeln braucht jemanden am Board:** Ein Doppeldruck auf BOOT öffnet es für 60 Sekunden, ein 6-stelliger
  Code wird verglichen, ein weiterer Druck bestätigt. Nichts im Netz kann das Board dazu bringen, nach seiner
  Taste zu fragen.
- **Auch die WLAN-Einrichtung braucht die Taste**, und das Setup-WLAN hat ein zufälliges Passwort pro Board.
- **Stopp-Taster:** Ein kurzer Druck auf BOOT stoppt alle Maus- und Tastaturausgaben (LED an = gestoppt). Das
  Board in Reichweite behalten, solange ein Assistent das Handy bedient.
- **Offen im eigenen Netz:** der AirPlay-Empfänger (TCP 7000 und seine Datenports), der CLI-Port 7202 und mDNS.
  Jeder im selben WLAN kann eine Spiegelung zum Board starten oder versuchen, es zum Absturz zu bringen; den
  Bildschirm sehen oder Eingaben schicken kann nur ein gekoppelter Computer.
- **Ohne OLED** lässt sich der Koppel-Code nicht vergleichen. Die Tastendrücke sagen dann nur „jetzt darf ein
  Computer koppeln“.

Alles dazu, mit den offenen Punkten: **[docs/hardening.de.md](docs/hardening.de.md)**. Ein Problem melden:
[SECURITY.md](SECURITY.md).

Nur Geräte und Konten automatisieren, die dir gehören oder die du benutzen darfst.

## Status

| | |
|---|---|
| Firmware baut (Standard, `--display`, `--no-safety`, `--debug`) | ja |
| Host-Tests, simuliertes Board, CLI-Tests | bestanden |
| Lauf auf einem echten Board | **noch nicht** |
| Setup-WLAN, Koppeln, Spiegelung, Tippen an einem echten iPhone | **noch nicht getestet** |
| Releases, Installer, npm-Paket | noch keine; aus dem Quellcode bauen |

## Roadmap

- Erster Lauf auf Hardware, danach Korrekturen
- **Web-Fernbedienung auf dem Board:** Live-Bild, Maus und Tastatur im Browser, nichts am Computer zu installieren
- **Android:** dasselbe Board als Hände, Screenshots über ADB ([docs/android.de.md](docs/android.de.md))

Mehr: [ROADMAP.de.md](ROADMAP.de.md). Änderungen: [CHANGELOG.md](CHANGELOG.md) (englisch).

## Dokumentation

- [cli/README.md](cli/README.md) — die Kommandozeile `tentacle` und der MCP-Server (englisch)
- [docs/hardening.de.md](docs/hardening.de.md) — Sicherheitsmodell und Einstellungen
- [docs/hardware-addons.de.md](docs/hardware-addons.de.md) — OLED, Akku, Gehäuse
- [docs/cli-protocol.md](docs/cli-protocol.md) — was über die Leitung geht (englisch)
- [docs/android.de.md](docs/android.de.md) — Android, geplant

## Herkunft und Lizenz

**Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan**
(MIT, Copyright © 2026 Henry Yuan) — *Based on ESP32 iPhone Agent by Henry Yuan*. Sein Urheberrechts- und
Erlaubnisvermerk steht unverändert in [LICENSE](LICENSE) und [NOTICE](NOTICE). Tentacle hat aus diesem Code die
iPhone-App, Bluetooth und Updates über Funk entfernt und die reine WLAN-Verbindung, das Setup-WLAN, das Koppeln
per Taste, den Stopp-Taster und das Display ergänzt.

- **MIT** ([LICENSE](LICENSE)) für alles außer dem Verzeichnis unten. Tentacles eigene Änderungen: Copyright ©
  2026 kewe87, zu denselben Bedingungen (siehe [NOTICE](NOTICE)).
- **GPL-3.0** für `hardware/hid_bridge/src/airplay/`, den aus [UxPlay](https://github.com/FDH2/UxPlay) portierten
  AirPlay-Empfänger ([LICENSE-GPL.md](hardware/hid_bridge/src/airplay/LICENSE-GPL.md)). Jede Firmware-Datei
  enthält dieses Verzeichnis, deshalb steht **eine Firmware-Binärdatei als Ganzes unter GPL-3.0**: Wer eine
  weitergibt (als Download oder auf einem fertig geflashten Board), muss den vollständigen passenden Quellcode
  unter GPL-3.0 anbieten und die Lizenzhinweise mitgeben. Die CLI `tentacle` enthält diesen Code nicht und
  bleibt MIT.
- Dateien unter [`skills/`](skills/) stehen unter [MIT-0](skills/LICENSE).
- Baut auf: [UxPlay](https://github.com/FDH2/UxPlay) ·
  [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) · [TinyUSB](https://github.com/hathach/tinyusb) ·
  [U8g2](https://github.com/olikraus/u8g2).

**Wer auf Tentacle aufbaut:** `LICENSE` und `NOTICE` behalten und das Original sichtbar nennen, im README und in
öffentlichen Demos, Videos und Beiträgen:

> Based on ESP32 iPhone Agent by Henry Yuan — https://github.com/onehumanbeing/esp32-iphone-agent

Apple, iPhone und AirPlay sind Marken von Apple Inc. Tentacle ist kein Apple-Produkt und steht in keiner
Verbindung zu Apple. Der Name hat nichts mit dem gleichnamigen Deployment-Agenten von Octopus Deploy zu tun.
