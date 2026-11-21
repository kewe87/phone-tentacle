# Optionale Hardware-Erweiterungen

[English](hardware-addons.md) · **Deutsch**

Jedes Extra muss angeschlossen sein. Die Firmware-Unterstützung steckt in jedem Image (0.4.0): das OLED findet
das Board selbst, der Akku-Pin ist eine Einstellung (`tentacle device set`, oder Einstellungen › Hardware in der
Weboberfläche; siehe [Handbuch](manual.de.md#board-einstellungen)). Noch nicht auf Hardware getestet. (Der Stopp-Taster braucht keine Teile und ist
standardmäßig an; siehe [hardening.de.md](hardening.de.md).)

## Status-OLED (128×64 SSD1306, I²C) — Einstellung `display`
Ein 0,96″-SSD1306-OLED (wenige Euro) zeigt Board-Name + Firmware, Netzwerkmodus + IP, Anzahl der Verbindungen
und **läuft / GESTOPPT** (gekoppelt an den BOOT-Stopp-Taster). Außerdem zeigt es, was man am Board braucht: Name
und Passwort des Setup-WLANs, `KOPPELN OFFEN` nach einem Doppeldruck auf BOOT und den **Koppel-Code** mit dem
Namen des Computers, solange eine Anfrage auf die Taste wartet. **Empfohlen:** Ohne OLED lässt sich der Code nicht
vergleichen.

Anschluss (die I²C-Pins des XIAO ESP32-S3; die Firmware nimmt GPIO5/GPIO6 auf jedem Board):

| OLED | XIAO |
|------|------|
| VCC  | 3V3  |
| GND  | GND  |
| SDA  | D4 (GPIO5) |
| SCL  | D5 (GPIO6) |

<p align="center"><img src="images/wiring-oled.de.svg" alt="Verdrahtung des OLED am XIAO ESP32S3: 3V3 an VCC, GND an GND, D4 an SDA, D5 an SCL" width="820"></p>


Beim Start fragt die Firmware die Adressen 0x3C und 0x3D an diesen Pins ab; antwortet eine, wird die Anzeige
benutzt. Nichts zu bauen oder einzustellen: `display auto` ist der Standard. `display on` zeichnet auch ohne
Antwort (ein Modul, das nicht quittiert), `display off` lässt die Pins in Ruhe. Die Texte sind deutsch oder
englisch (Einstellung `lang`).

## Akku-Anzeige (LiPo) — Einstellung `battery`
Misst die Akkuspannung über einen Spannungsteiler an einem Analog-Pin und zeigt grob die Prozent auf dem OLED.

<p align="center"><img src="images/wiring-battery.de.svg" alt="Verdrahtung der Akku-Anzeige: BAT+ über 100 kΩ an D0, D0 über 100 kΩ an GND, Akku an die BAT-Pads" width="820"></p>

- Verdrahten: `BAT+ → 100 kΩ → D0 (GPIO1) → 100 kΩ → GND` (zwei gleiche Widerstände = Teiler 2:1; hält den
  Pin unter 3,3 V).
- Pin einstellen: `tentacle device set battery 1` (GPIO1-4 oder 7-10; 5 und 6 gehören dem OLED). Bei anderem
  Teilerverhältnis: `tentacle device set battery_div 3.00` (Akku-Volt je Pin-Volt; Standard 2.00).
- Der volle Status (`tentacle device info`) enthält den Ladestand auch als `bat`.
- Die Prozentkurve ist grob 3,3 V = 0 % … 4,2 V = 100 %; für den eigenen Akku kalibrieren.
- Der XIAO ESP32-S3 lädt einen LiPo an seinen BAT-Pads, so laufen Board + Handy auch unterwegs.

## Waveshare ESP32-S3-ePaper-1.54 (E-Paper, Uhr, microSD)

Ein fertiges Board statt XIAO plus Teilen: [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm)
(V2: ESP32-S3-PICO-1-N8R8, 8 MB Flash, 8 MB PSRAM, natives USB), ein 1,54″-E-Paper mit 200×200 Pixeln in
Schwarz-Weiß, eine Echtzeituhr PCF85063, ein microSD-Slot (dazu Codec, Mikrofon und Lautsprecher, die die Firmware
nicht nutzt). Es läuft dasselbe Firmware-Image; es erkennt das Board beim Start selbst (Gehäuse des Chips und die
Uhr an I²C 47/48) und nutzt dann:

- **Das E-Paper** als Statusanzeige, mit Symbolen ([Handbuch](manual.de.md#10-das-display)): keine Verdrahtung,
  Einstellung `display` wie beim OLED. Der QR-Code des Setup-WLANs füllt es aus.
- **Die Uhr:** die CLI stellt die Zeit beim Start einer Sitzung, die Webseite beim Öffnen (`time EPOCH OFFSET`,
  [Protokoll](cli-protocol.md#commands)); die Uhr behält sie, solange das Board Strom hat (über USB oder den
  Akku-Anschluss; ohne Knopfzelle beginnt sie danach bei der zuletzt gestellten Zeit). Sie stempelt die Aufzeichnung.
- **Die microSD-Karte** (FAT32; das Board formatiert sie nie): Einstellung `sdlog` oder REC auf der Webseite.
  - `log`: `/tentacle/sNNNNN.log`, bei jedem Start eine neue Nummer: das Protokoll des Boards und jede Eingabe an
    das iPhone (von der CLI, der Webseite, USB-Seriell: Befehl und Antwort), mit Uhrzeit. Getippter Text nie: nur
    seine Länge.
  - `video`: das Protokoll und der gespiegelte Bildschirm als `/tentacle/sNNNNN.ts` (H.264 in einem
    MPEG-Transportstrom: VLC, mpv, ffmpeg spielen ihn ab; eine durch Ziehen der Karte abgeschnittene Datei spielt
    trotzdem). Es beginnt beim letzten Keyframe, eventuell Minuten früher, wenn die Spiegelung schon lief, und
    läuft in Echtzeit. Über 3,9 GB (FAT32) geht es in `sNNNNN_2.ts`, `_3` … weiter; zum Durchspielen die Teile
    aneinanderhängen (`copy /b` unter Windows, sonst `cat`).
  - **Das Video enthält alles, was der Bildschirm zeigt**, auch Passwörter, soweit das iPhone sie anzeigt. Es ist
    aus, bis es eingeschaltet wird, und E-Paper und Webseite zeigen `REC`, solange es läuft.
  - Eine langsame Karte hält weder das iPhone noch die Spiegelung auf: das Board puffert Minuten; erst wenn die
    Karte einen ganzen Puffer zurückliegt, wartet das Video auf den nächsten Keyframe (das Protokoll sagt es).
  - Vor dem Ziehen der Karte ausschalten (`sdlog off` oder REC noch einmal), damit die Dateien geschlossen sind.

Flashen: das Board erscheint als zwei USB-Ports, der der Tentacle-Firmware und der des Chips selbst
(Download-Modus). Findet ein Upload das Board nicht, BOOT beim Einstecken halten und den dann erscheinenden Port
nehmen. Das Setup-WLAN sendet auf diesem Board mit 15 dBm (beim XIAO 5 dBm): seine Chipantenne erreichte mit 5 dBm
kein Handy.

## Gehäuse
Auf Etsy/Printables gibt es Gehäuse für den XIAO ESP32-S3 (einfache Version, USB-C-Seite offen). Mit OLED oder
Akku ein Gehäuse mit OLED-Fenster bzw. Akkufach wählen oder drucken lassen.
