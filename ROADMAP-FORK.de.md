# Fork-Notizen & Roadmap (kewe87)

[English](ROADMAP-FORK.md) · **Deutsch**

Dieser Fork ergänzt [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan (MIT)
um Sicherheits-Fixes, sichere Voreinstellungen, Robustheits-Fixes und optionale Hardware. Copyright und LICENSE des
Originals bleiben erhalten, wie es die MIT-Lizenz und `AGENTS.md` verlangen.

## ✅ In diesem Fork umgesetzt

| Änderung | Datei(en) | Geprüft |
|---|---|---|
| **AirPlay/RTSP- + bplist-Überlauf-Fixes** (Lesen außerhalb des Speichers ohne Anmeldung → Absturz) | `src/airplay/rtsp.c`, `src/bplist/bplist.c` | kompiliert; Regressionstest + Fuzzing in der CI |
| **Stopp-Taster + Status-LED** (BOOT-Taster, **standardmäßig an**, `--no-safety` entfernt ihn) | `hardware/hid_bridge/hid_bridge.ino`, `flash.sh` | kompiliert mit und ohne; **Test am Board nötig** |
| **Nur ein Gerät im Hotspot** (**standardmäßig an**, `--multi-client` erlaubt 4) | `netcfg.cpp`, `flash.sh`, `docs/hardening.md` | kompiliert; **Wiederverbinden des iPhones am Board prüfen** |
| **Windows-Bluetooth-Suche** (#114: leere `devices[]` unter WinRT) | `cli/internal/ble/ble.go` | `go build` / `vet` / `test` in der CI |
| **Schnellere Bluetooth-Erkennung** (Kurzname in der Haupt-Ankündigung + schnelleres Intervall) | `ble_link.cpp` | kompiliert; **Test am Board nötig** |
| **Absturzschutz im CLI-Dienst** (fehlerhafte `io`-Anfrage ließ den ganzen Dienst abstürzen) | `cli/internal/daemon/ops.go`, `server.go` | CI |
| **Datenrennen in Decoder.err** behoben | `cli/internal/video/decoder.go` | CI |
| **Schutz vor leeren Zeilen** | `cli/internal/daemon/conn.go` | CI |
| **OLED-Statusanzeige** (`--display`, SSD1306 I²C über U8g2) | `hid_bridge.ino`, `docs/hardware-addons.md` | kompiliert; **OLED muss angeschlossen sein** |
| **Akku-Anzeige** (`--battery-pin`, auf dem OLED) | `hid_bridge.ino`, `docs/hardware-addons.md` | kompiliert; **LiPo + Spannungsteiler nötig** |
| **bplist-Fuzz- + Regressionstest** (libFuzzer + portabler Nachstelltest) | `hardware/tests/fuzz/` | läuft in der CI (libFuzzer + ASan/UBSan) |
| **CI** (3 Firmware-Varianten, `go build`/`vet`/`test`, bplist-Fuzzing) | `.github/workflows/fork-ci.yml` | grün auf GitHub Actions |

Sicherheitsdetails: [docs/hardening.de.md](docs/hardening.de.md).

## 🟡 Offen (geringere Schwere — erfasst, noch nicht behoben)

Ein sauberer Fix braucht Tests im laufenden Betrieb:

- **AirPlay-SETUP-Schleife über Streams** (`src/airplay/handlers.c`): geht jeden Eintrag der `streams`-Liste durch
  und sucht in jedem ein Feld; eine präparierte 64-KB-Anfrage könnte das Board beschäftigen (möglicher
  CPU-DoS, nicht ausprobiert). Fix-Idee: auf ca. 8 Einträge begrenzen (echte Geräte schicken 1–2).
- **Goroutine-Leck**: `daemon/listen.go` `transcribe` — `h.work` wird beim Verbindungsende nie geschlossen.
- **Zeiger-Zustand nicht serialisiert** pro Board bei gleichzeitigen Anfragen (`internal/pointer`).
- **`wifi.go fallBack`** fehlt der `claim()/release()`-Schutz, den `up()` hat (kleines Zeitfenster).
- **`ble/tcp.go`**: Keepalive-Goroutine läuft nach dem Verbindungsende bis zu einem Takt weiter (Aufräumen).

## 💡 Vorgeschlagene Funktionen (brauchen iOS-App / Hardware; bisher nur Entwurf)

- **Schutz bei Passwortfeldern** — nichts tippen, wenn ein Passwort-/Sicherheitsfeld erkannt wird (OCR), im Zweifel blockieren.
- **App-Freigabeliste** — der Agent darf nur freigegebene Apps öffnen/bedienen.
- **Aktionsprotokoll** — dauerhaft speichern, was der Agent getan hat (das Board hat bereits einen RAM-Log-Ring).
- **Endzustand prüfen** — den echten Endzustand kontrollieren, um „falschen Erfolg“ zu erkennen.
- **Android-Doppelmodus** — eine „Hände“-Firmware für beide Plattformen (siehe [docs/android.de.md](docs/android.de.md)).

---
Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan.
