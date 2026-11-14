# Roadmap

[English](ROADMAP.md) · **Deutsch**

## Als Nächstes

1. **Erster Lauf auf Hardware** (XIAO ESP32S3): Setup-WLAN → Koppeln → Bild → Eingabe → Stopp-Taster. Nichts in
   diesem Repository ist bisher auf einem Board gelaufen; es wird Korrekturen geben.
2. **Inbetriebnahme-Schnittstelle ausbauen** (`--debug`-Builds), sobald diese Kette stabil läuft.
3. **Web-Fernbedienung auf dem Board:** eine Seite direkt vom Board mit Live-Bild sowie Maus und Tastatur, damit
   am Computer nichts installiert werden muss. Geschützt durch ein Fernsteuerungs-Kennwort, das auf der
   Einrichtungsseite gesetzt wird, HTTPS mit einem pro Board erzeugten Zertifikat, eine Sitzung zugleich; der
   Stopp-Taster gilt weiter. Ein KI-Assistent braucht weiterhin die CLI, weil er Screenshots als Bild braucht.
4. **Android:** dasselbe Board als Hände (USB-HID, absoluter Zeiger), Screenshots per ADB über WLAN als zweites
   „Augen“-Backend der CLI. Siehe [docs/android.de.md](docs/android.de.md).

## Bekannte offene Punkte

- **AirPlay-SETUP-Schleife über Streams** (`src/airplay/handlers.c`): geht jeden Eintrag der `streams`-Liste
  durch; eine präparierte 64-KB-Anfrage könnte das Board eine Weile beschäftigen (möglicher CPU-DoS, nicht
  ausprobiert). Idee: nach etwa 8 Einträgen aufhören (echte Geräte schicken 1–2).
- **CLI-Daemon:** Die Warteschlange der Transkription (`daemon/listen.go`, `h.work`) wird beim Verbindungsende
  nie geschlossen; der Zeiger-Zustand ist bei gleichzeitigen Anfragen nicht pro Board serialisiert
  (`internal/pointer`).
- **Ohne OLED** lässt sich der Koppel-Code am Board nicht vergleichen (siehe
  [docs/hardening.de.md](docs/hardening.de.md)).

## Ideen

- Nicht in Felder tippen, die wie ein Passwortfeld aussehen.
- Eine Liste der Apps, die der Agent öffnen darf.
- Ein dauerhaftes Protokoll dessen, was der Agent getan hat (heute: Log im RAM des Boards und die Läufe, die die
  CLI aufzeichnet).
