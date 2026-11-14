# Das Board sicher betreiben

[English](hardening.md) · **Deutsch**

Tentacle bedient ein echtes Handy und hat ein paar Netzwerkdienste. Diese Seite sagt, wer was erreichen kann, was
ein Mensch am Board tun muss und was offen bleibt.

> Noch nicht auf Hardware getestet. Beschrieben ist, wie die Firmware geschrieben ist und was ihre Tests am
> Computer prüfen (Host-Tests, ein simuliertes Board). Das Verhalten auf einem echten Board muss sich noch
> bestätigen.

## Was erreichbar ist

Das Board hat nur WLAN. Es gibt kein Bluetooth und keine App auf dem Handy.

| Port | Wann | Wer ihn nutzen kann |
|---|---|---|
| **TCP 7202** (CLI-Verbindung) | immer | Jeder im Netz kann sich verbinden; eine Sitzung bekommt nur ein gekoppelter Computer. Nach dem Handshake ist alles verschlüsselt (AES-128-GCM, Schlüssel aus X25519 + HKDF). |
| **TCP 7000** (AirPlay) und seine Datenports (7010–7012 UDP, 7100 TCP) | immer | **Jeder im Netz.** Die AirPlay-Spiegelung zum Board hat kein Passwort. |
| **mDNS** (UDP 5353) | immer | Meldet den AirPlay-Empfänger und `_tentacle._tcp`: Name, Board-ID, Firmware-Version, Zahl der gekoppelten Computer, ob das Koppeln offen ist. Nichts Geheimes. |
| **TCP 80** (Einrichtungsseite) + DNS für das Captive Portal | nur solange das Setup-WLAN läuft | Geräte im Setup-WLAN (WPA2, Passwort nötig). |
| USB-Seriell | wenn das Board an einem Computer steckt | Wer das Kabel in der Hand hat: siehe „Physischer Zugriff“. |

Die Ports der App des Originalprojekts (7200, 7201, 7300) gibt es nicht mehr. Firmware-Updates gehen nur über USB.

## Was einen Menschen am Board braucht

Die BOOT-Taste ist der Nachweis, dass jemand da ist. Drei Dinge brauchen sie:

**1. Ein WLAN von der Einrichtungsseite speichern.** Ein Board ohne WLAN öffnet sein eigenes: `Tentacle-XXXX`,
WPA2, mit einem zufälligen 12-stelligen Passwort, das auf diesem Board erzeugt wird (das Flash-Skript und das
OLED zeigen es). Es kann immer nur ein Gerät darin sein. Die Auswahl eines Netzes auf der Seite speichert nichts,
bis innerhalb von 30 s BOOT gedrückt wird. Ist das Board in deinem WLAN, sind Setup-WLAN und Port 80 weg, und sie
kommen nicht von selbst zurück, wenn dein WLAN ausfällt: nur BOOT 10 s halten (oder `tentacle net hotspot` von
einem gekoppelten Computer) bringt sie zurück.

**2. Das Koppeln öffnen.** Das Board ignoriert Koppel-Anfragen, wenn BOOT nicht in den letzten 60 s **doppelt
gedrückt** wurde. Außerhalb dieses Fensters bekommt eine Anfrage einen Fehler, und sonst passiert nichts: kein
Text auf dem Display, keine LED, keine Sperre. Nichts im Netz kann das Board also dazu bringen, nach seiner Taste
zu fragen, oder dich mit Nachfragen mürbe machen.

**3. Den Computer bestätigen.** Im Fenster zeigen Computer und Board je einen **6-stelligen Code**, der aus dem
gerade vereinbarten Schlüssel entsteht, und das Board zeigt den Namen des Computers. Ein weiterer Druck innerhalb
von 30 s speichert den Schlüssel. Ein anderer Code oder ein unbekannter Name heißt: Jemand anderes fragt oder
sitzt dazwischen. Dann nicht drücken.
- Nur eine Anfrage zugleich; eine zweite ersetzt nie Namen oder Code der ersten.
- Ein Druck zählt erst als „Ja“, wenn die Anfrage eine Sekunde gewartet hat. Ein Druck, der schon unterwegs war,
  kann sie also nicht bestätigen.
- Eine Anfrage, die ohne Tastendruck endet, sperrt das Koppeln für 5 s, dann 10, 20 … bis zu 5 Minuten.
- Solange ein gekoppelter Computer das Handy bedient, kann nichts gekoppelt werden, und BOOT bleibt der
  Stopp-Taster.

Ein gekoppelter Computer gilt danach als vertrauenswürdig: Er sieht den Bildschirm, schickt Eingaben und kann das
Board ohne Taste in ein anderes WLAN bringen (`tentacle net join`). Das Board merkt sich bis zu vier Computer;
`tentacle unpair --board` vergisst alle.

## Der Stopp-Taster

Ein kurzer Druck auf BOOT stoppt alle Maus- und Tastaturausgaben (LED an = gestoppt; langsames Blinken = läuft).
Ein weiterer Druck setzt fort. Befehle scheitern dann mit `stopped (safety button)`; der Computer kann den Stopp
nicht aufheben. Das Board in Reichweite behalten, solange ein Assistent das Handy bedient. `flash.sh --no-safety`
baut ohne ihn; nicht für unbeaufsichtigtes Arbeiten verwenden.

## Was offen bleibt

- **Ohne OLED lässt sich der Code nicht vergleichen.** Doppeldruck und Bestätigungsdruck sagen dann nur „jemand
  am Board ist einverstanden, dass jetzt ein Computer koppelt“. Wer sonst in deinem WLAN genau in diesen Sekunden
  eine Anfrage schickt, könnte an deiner Stelle gekoppelt werden (dein eigenes `tentacle pair` scheitert dann mit
  `busy`). Das OLED anbauen oder nur in einem Netz koppeln, dessen Geräten du traust.
- **AirPlay ist im Netz offen.** Jeder im selben WLAN kann sich mit dem Empfänger verbinden. Den Bildschirm deines
  Handys bekommt er so nicht, aber er kann deinem Handy den Empfänger wegnehmen oder präparierte Anfragen
  schicken. Zwei Überlauffehler in den Parsern, die das Board zum Absturz brachten, sind behoben (Regressionstest
  und Fuzzing in `hardware/tests/`); eine mögliche rechenintensive Schleife ist bekannt
  ([ROADMAP.de.md](../ROADMAP.de.md)). Einen Absturz oder Neustart durch ein feindliches Gerät im eigenen Netz
  für möglich halten.
- **Dein WLAN ist die Außenmauer.** Das Board in einem Netz betreiben, dessen Geräten du traust, nicht in einem
  öffentlichen oder geteilten WLAN. Ein Gastnetz, das Geräte voneinander trennt, hält auch deinen Computer vom
  Board fern.
- **Physischer Zugriff.** BOOT 10 s halten vergisst das WLAN und öffnet das Setup-WLAN; dessen Passwort steht auf
  dem OLED. Die USB-Schnittstelle nimmt die Befehle der Firmware ohne Koppeln an (sie ist der Rettungsweg),
  darunter das Vergessen aller gekoppelten Computer und das Ändern des WLANs. Wer das Board in der Hand hat, dem
  gehört es; abziehen, wenn du es nicht benutzt.
- **Debug-Builds** (`flash.sh --debug`, `flash.ps1 -Debug`) können BOOT über die serielle Schnittstelle drücken
  und geben dort den Koppel-Code aus. Sie sind nur für die Inbetriebnahme. Nie weitergeben und nicht auf einem
  Board lassen, das du benutzt.
- **Der Computer.** Die Koppel-Schlüssel sind Dateien im Konfigurationsordner der CLI, in deinem Benutzerprofil.
  Jedes Programm, das als du läuft, kann sie benutzen, ebenso jeder MCP-Client, den du anschließt.
- **Das Modell sieht deinen Bildschirm.** Screenshots gehen an das Modell, das dein Assistent benutzt. Die
  Spiegelung endet, wenn das Handy sperrt, und iOS blendet manche Bildschirme aus der Spiegelung aus; für
  Geheimnisse sollte man sich darauf nicht verlassen.

## Empfohlen

- Das OLED anbauen und beim Koppeln den Code vergleichen.
- Den Stopp-Taster eingeschaltet lassen (Standard) und das Board in Reichweite behalten.
- Das eigene WLAN benutzen. Das Passwort des Setup-WLANs nicht weitergeben.
- Nur die Computer koppeln, die du brauchst; `tentacle unpair --board`, wenn einer wegfällt.
- Das Board abziehen, wenn nichts das Handy bedienen soll.
- Nur Geräte und Konten automatisieren, die dir gehören oder die du benutzen darfst.

Protokoll im Einzelnen: [cli-protocol.md](cli-protocol.md) (englisch). Ein Problem melden:
[SECURITY.md](../SECURITY.md).

---

Basiert auf [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) von Henry Yuan (MIT).
