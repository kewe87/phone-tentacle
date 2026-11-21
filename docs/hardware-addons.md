# Optional hardware add-ons

**English** · [Deutsch](hardware-addons.de.md)

Each needs the part physically wired. The firmware support is in every image (0.4.0): the board finds the OLED
by itself, the battery pin is a setting (`tentacle device set`, or Settings › Hardware on the web interface; see
the [manual](manual.md#board-settings)). Not yet tested on hardware. (The stop button needs no parts and is on by default; see
[hardening.md](hardening.md).)

## Status OLED (128×64 SSD1306, I²C) — setting `display`
A 0.96″ SSD1306 OLED (a few euros) shows board name + firmware, network mode + IP, connection count, and
**running / STOPPED** (tied to the BOOT stop button). It also shows what you need at the board: the setup
Wi-Fi's name and password, `PAIRING OPEN` after a double press of BOOT, and the **pairing code** with the
computer's name while a request waits for the button. **Recommended:** without it the code cannot be compared.

Wiring (the XIAO ESP32-S3's I²C pins; the firmware uses GPIO5/GPIO6 on every board):

| OLED | XIAO |
|------|------|
| VCC  | 3V3  |
| GND  | GND  |
| SDA  | D4 (GPIO5) |
| SCL  | D5 (GPIO6) |

<p align="center"><img src="images/wiring-oled.svg" alt="Wiring the OLED to the XIAO ESP32S3: 3V3 to VCC, GND to GND, D4 to SDA, D5 to SCL" width="820"></p>


At boot the firmware asks addresses 0x3C and 0x3D on these pins; the display is used when one answers. Nothing
to build or set: `display auto` is the default. `display on` draws even when none answers (a module that does not
acknowledge), `display off` leaves the pins alone. The texts are English or German (setting `lang`).

## Battery gauge (LiPo) — setting `battery`
Reads the LiPo voltage through a divider on an ADC pin and shows a rough % on the OLED.

<p align="center"><img src="images/wiring-battery.svg" alt="Wiring the battery gauge: BAT+ through 100 kΩ to D0, D0 through 100 kΩ to GND, the battery on the BAT pads" width="820"></p>

- Wire `BAT+ → 100 kΩ → D0 (GPIO1) → 100 kΩ → GND` (two equal resistors = 2:1 divider; keeps the pin ≤ 3.3 V).
- Set the pin: `tentacle device set battery 1` (GPIO1-4 or 7-10; 5 and 6 are the OLED's). With a different
  divider ratio: `tentacle device set battery_div 3.00` (battery volts per pin volt; default 2.00).
- The full status (`tentacle device info`) has the charge as `bat` too.
- The % curve is a rough 3.3 V = 0 % … 4.2 V = 100 %; calibrate for your cell.
- The XIAO ESP32-S3 charges a LiPo on its BAT pads, so board + phone can run portable.

## Waveshare ESP32-S3-ePaper-1.54 (e-paper, clock, microSD)

A ready board instead of the XIAO plus parts: [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm)
(V2: ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM, native USB), a 1.54″ 200×200 black and white e-paper, a
PCF85063 real-time clock, a microSD slot (and a codec, microphone and speaker the firmware does not use). The same
firmware image runs on it; it finds the board by itself at boot (the chip's package and the clock on I²C 47/48) and
then uses:

- **The e-paper** as the status display, in symbols ([manual](manual.md#10-the-display)): no wiring, setting
  `display` as for the OLED. The setup Wi-Fi's QR code fills it.
- **The clock:** the CLI sets the time when a session starts, the web page when it opens (`time EPOCH OFFSET`,
  [protocol](cli-protocol.md#commands)); the clock keeps it while the board is off (as long as it has power from
  USB or the battery connector; without a coin cell it starts again from the last time set). It stamps the
  recording.
- **The microSD card** (FAT32; the board never formats it): the setting `sdlog`, or REC on the web page.
  - `log`: `/tentacle/sNNNNN.log`, a new number each time it starts: the board's log and every input the phone
    gets (from the CLI, the web page, USB serial: the command and the answer), stamped with the time. Typed text
    never: only its length.
  - `video`: the log, and the mirrored screen as `/tentacle/sNNNNN.ts` (H.264 in an MPEG transport stream: VLC,
    mpv, ffmpeg play it; a file cut off by pulling the card still plays). It starts at the last keyframe, possibly
    minutes back when mirroring was already running, and runs in real time. Over 3.9 GB (FAT32) it goes on in
    `sNNNNN_2.ts`, `_3` …; join the parts to play across (`copy /b` on Windows, `cat` elsewhere).
  - **The video holds whatever the screen shows**, passwords too as far as the phone shows them. It is off unless
    switched on, and the e-paper and the web page show `REC` while it runs.
  - A slow card never holds up the phone or the mirroring: the board buffers minutes; only when the card falls a
    whole buffer behind does the video wait for the next keyframe (the log says so).
  - Switch it off (`sdlog off`, or REC again) before pulling the card, so the files are closed.

Flashing it: the board shows up as two USB ports, the Tentacle firmware's and the chip's own (download mode). If an
upload does not find the board, hold BOOT while plugging it in and use the port that appears then. The setup
hotspot runs at 15 dBm on this board (5 dBm on the XIAO): its chip antenna did not reach a phone at 5 dBm.

## Case
Etsy/Printables have XIAO ESP32-S3 cases (plain version, USB-C end open). With an OLED or LiPo, pick a case
with an OLED window / battery bay, or have one printed.
