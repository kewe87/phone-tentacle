# Das Board sicher betreiben

[English](hardening.md) · **Deutsch**

Das Board steuert ein echtes Handy und betreibt ein paar Netzwerkdienste. Für den privaten Gebrauch
und Bastelprojekte ist das in Ordnung; diese Schritte verringern die Angriffsfläche gegenüber allen
in WLAN- bzw. Bluetooth-Reichweite.

## Bedrohungsmodell
Wenn das Board seinen eigenen WLAN-Hotspot betreibt, sind der AirPlay/RTSP-Empfänger (TCP 7000) und
die Video-/Update-Ports für **jedes Gerät erreichbar, das diesem Hotspot beitritt**. Ein feindliches
Gerät im Hotspot — oder im Heimnetz, wenn das Board dort angemeldet ist — kann diese Dienste ansprechen.

## In diesem Fork behoben
- **Absturz über AirPlay ohne Anmeldung (DoS).** Ein manipulierter `Content-Length`-Wert in einer
  RTSP-Anfrage oder manipulierte Größenfelder in einer Binär-Plist konnten die Größenprüfung durch
  einen Zahlenüberlauf austricksen; der Parser las dann über seinen Puffer hinaus → Absturz →
  Neustart. Erreichbar ohne Kopplung für jedes Gerät im Hotspot. Ein Schreiben außerhalb des
  Speichers oder Ausführen von Fremdcode wurde nicht gefunden. Behoben in `src/airplay/rtsp.c` und
  `src/bplist/bplist.c` (Grenzen per Subtraktion/Division statt Addition geprüft), abgesichert durch
  einen Regressionstest und dauerhaftes Fuzzing (`hardware/tests/fuzz/`).
- Offene Punkte mit geringerer Schwere: [ROADMAP-FORK.de.md](../ROADMAP-FORK.de.md).

## Empfohlene Einstellungen
- **Starkes Hotspot-Passwort behalten.** Die Firmware erzeugt zufällige Zugangsdaten; nicht durch
  etwas Schwaches ersetzen und nicht weitergeben.
- **Nur ein Gerät im Hotspot (Standard in diesem Fork).** Nur ein Gerät — dein iPhone — kann
  gleichzeitig verbunden sein (`AP_MAX_CLIENTS=1`). Damit kann auch kein Computer neben dem iPhone in
  den Hotspot; die CLI bleibt dann bei Bluetooth, oder nutze `esp32agent net join` für dein Heim-WLAN.
  Nur bei Bedarf mit `flash.sh --multi-client` bauen (bis zu 4 Geräte), z. B. für die Entwickler-Testumgebung.
- **Hotspot statt Heim-WLAN** in fremden Umgebungen: Im Heimnetz sind die Dienste für alles in
  diesem Netz erreichbar.
- **Stopp-Taster (Standard in diesem Fork).** Den BOOT-Taster des Boards drücken, um alle Maus- und
  Tastatureingaben zu unterdrücken (LED dauerhaft = gestoppt, langsames Blinken = läuft); nochmal
  drücken zum Fortsetzen. In Reichweite halten, solange ein Agent das Handy steuert.
  `flash.sh --no-safety` entfernt ihn.
- **Lokales oder vertrauenswürdiges KI-Modell nutzen.** Screenshots des Handys gehen an das Modell,
  das du einträgst; ein lokales Modell (Ollama / vLLM) behält sie auf deinem eigenen Rechner.
- **Physisch im Blick behalten.** Die Spiegelung stoppt, wenn das Handy sperrt, und iOS verbirgt
  Code-Eingabe / Apple Pay / DRM-Inhalte — aber das Board bleibt eine Tastatur + Maus. Abziehen,
  wenn du es nicht benutzt.

## Nur automatisieren, was dir gehört
Nur Geräte und Konten steuern, die dir gehören oder für die du die Erlaubnis hast.
