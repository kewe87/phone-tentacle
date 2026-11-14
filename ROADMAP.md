# Roadmap

**English** · [Deutsch](ROADMAP.de.md)

## Next

1. **First run on hardware** (XIAO ESP32S3): setup hotspot → pairing → screen → input → stop button. Nothing in
   this repository has run on a board yet; expect fixes.
2. **Remove the bring-up interface** (`--debug` builds) once that chain is stable.
3. **Web remote on the board:** a page served by the board with the live picture and mouse/keyboard, so nothing
   has to be installed on the computer. Protected by a remote-control password set on the setup page, HTTPS with a
   certificate made per board, one session at a time; the stop button stays in force. An AI assistant still needs
   the CLI, because it needs screenshots as images.
4. **Android:** the same board as the hands (USB HID, absolute pointer), screenshots through ADB over Wi-Fi as a
   second "eyes" backend of the CLI. See [docs/android.md](docs/android.md).

## Known open points

- **AirPlay SETUP stream loop** (`src/airplay/handlers.c`): walks every entry of the `streams` array; a crafted
  64 KB request could keep the board busy for a while (possible CPU denial of service, not tried). Idea: stop
  after about 8 entries (real devices send 1–2).
- **CLI daemon:** the transcription worker's queue (`daemon/listen.go`, `h.work`) is never closed when a
  connection ends; the pointer state is not serialized per board across concurrent requests
  (`internal/pointer`).
- **Without the OLED** the pairing code cannot be compared at the board (see [docs/hardening.md](docs/hardening.md)).

## Ideas

- Refuse to type into a field that looks like a password field.
- A list of apps the agent may open.
- A lasting log of what the agent did (today: a log in the board's RAM, and the runs the CLI records).
