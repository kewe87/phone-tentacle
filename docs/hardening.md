# Running the board safely

**English** · [Deutsch](hardening.de.md)

Tentacle drives a real phone and runs a few network services. This page says who can reach what, what a person
at the board has to do, and what is left open.

> Not yet tested on hardware. What is described here is how the firmware is written and what its tests on the
> computer check (host tests, a simulated board). Behaviour on a real board still has to be confirmed.

## What is reachable

The board has Wi-Fi only. There is no Bluetooth and no app on the phone.

| Port | When | Who can use it |
|---|---|---|
| **TCP 7202** (CLI link) | always | Anyone on the network can connect; only a paired computer gets a session. Everything after the handshake is encrypted (AES-128-GCM, keys from X25519 + HKDF). |
| **TCP 7000** (AirPlay) and its data ports (7010–7012 UDP, 7100 TCP) | always | **Anyone on the network.** AirPlay mirroring to the board has no password. |
| **mDNS** (UDP 5353) | always | Announces the AirPlay receiver and `_tentacle._tcp`: name, board id, firmware version, how many computers are paired, whether pairing is open. Nothing secret. |
| **TCP 80** (setup page) + DNS for the captive portal | only while the setup hotspot is up | Devices that joined the setup Wi-Fi (WPA2, password needed). |
| USB serial | when plugged into a computer | Whoever holds the cable: see "Physical access". |

Ports of the original project's app (7200, 7201, 7300) do not exist any more. Firmware updates go over USB only.

## What needs a person at the board

The BOOT button is the proof that someone is there. Three things need it:

**1. Storing a Wi-Fi from the setup page.** A board without a Wi-Fi opens its own: `Tentacle-XXXX`, WPA2, with a
random 12-character password made on that board (shown by the flash script and on the OLED). Only one device can
be on it at a time. Choosing a network on the page stores nothing until BOOT is pressed within 30 s. Once the
board is on your Wi-Fi the hotspot and port 80 are gone, and they do not come back by themselves when your Wi-Fi
is down: only holding BOOT for 10 s (or a paired computer's `tentacle net hotspot`) brings them back.

**2. Opening pairing.** The board ignores pairing requests unless BOOT was **double-pressed** in the last 60 s.
Outside that window a request gets an error and nothing else happens: no text on the display, no LED, no lock.
So nothing on the network can make the board ask for its button, or wear you down with prompts.

**3. Confirming the computer.** Inside the window, the computer and the board each show a **6-digit code** made
from the key they just agreed on, and the board shows the computer's name. One more press within 30 s stores the
key. A different code or a name you do not know means someone else is asking or sits in between: do not press.
- Only one request at a time; a second one never replaces the first one's name or code.
- A press counts as "yes" only after the request has waited a second, so a press that was already on its way
  cannot confirm it.
- A request that ends without the button locks pairing for 5 s, then 10, 20 … up to 5 minutes.
- While a paired computer is driving the phone, nothing can be paired, and BOOT stays the stop button.

A paired computer is trusted from then on: it sees the screen, sends input, and can move the board to another
Wi-Fi (`tentacle net join`) without the button. The board keeps up to four computers; `tentacle unpair --board`
forgets them all.

## The stop button

A short press of BOOT stops all mouse and keyboard output (LED on = stopped; slow blink = running). Another
press resumes. Commands then fail with `stopped (safety button)`; the computer cannot switch the stop off. Keep
the board within reach while an assistant drives the phone. `flash.sh --no-safety` builds without it; do not use
that for unattended work.

## What is left open

- **Without the OLED the code cannot be compared.** The double press and the confirming press then only say
  "someone at the board agrees that a computer pairs now". Somebody else on your Wi-Fi who sends a request in
  exactly those seconds could get paired instead of you (your own `tentacle pair` would then fail with `busy`).
  Fit the OLED, or pair only on a network whose devices you trust.
- **AirPlay is open to the network.** Anyone on the same Wi-Fi can connect to the receiver. They do not get your
  phone's screen that way, but they can take the receiver away from your phone or send it crafted requests. Two
  overflow bugs in the request parsers that crashed the board were fixed (regression test and fuzzing in
  `hardware/tests/`); one possible CPU-bound loop is known ([ROADMAP.md](../ROADMAP.md)). Treat a crash or
  reboot by a hostile device on your network as possible.
- **Your Wi-Fi is the outer wall.** Use the board on a network whose devices you trust, not on a public or shared
  Wi-Fi. A guest network that isolates clients will also keep your computer from reaching the board.
- **Physical access.** Holding BOOT for 10 s forgets the Wi-Fi and opens the setup hotspot; its password is on
  the OLED. The USB serial port accepts the firmware's commands without pairing (it is the recovery path), among
  them forgetting all paired computers and changing the Wi-Fi. Whoever can hold the board owns it; unplug it when
  you are not using it.
- **Debug builds** (`flash.sh --debug`, `flash.ps1 -Debug`) can press BOOT from the serial port and print the
  pairing code there. They are for bring-up only. Never give one to anyone else or leave one on a board you use.
- **The computer.** The pairing keys are files in the CLI's config directory, in your user profile. Any
  program running as you can use them, and so can any MCP client you connect.
- **The model sees your screen.** Screenshots go to whatever model your assistant uses. Mirroring stops when the
  phone locks, and iOS hides some screens from mirroring, but do not rely on that for secrets.

## Recommended

- Fit the OLED and compare the code when you pair.
- Keep the stop button on (the default) and the board within reach.
- Use your own Wi-Fi. Do not hand out the setup hotspot's password.
- Pair only the computers you need; `tentacle unpair --board` when one is gone.
- Unplug the board when nothing should drive the phone.
- Only automate devices and accounts that are yours or that you are allowed to use.

Protocol details: [cli-protocol.md](cli-protocol.md). Reporting a problem: [SECURITY.md](../SECURITY.md).

---

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan (MIT).
