# CLI protocol (ESP32 ⇄ computer, over Wi-Fi)

The `tentacle` CLI finds the board on the local network (mDNS), pairs with it and talks to it on **TCP port
7202**. That is the board's only link to a computer: the firmware has no Bluetooth and there is no phone app. The
link is authenticated and encrypted here, in the application layer; pairing is confirmed by a person at the board,
with its BOOT button.

Open ports: 7202 (this protocol), the AirPlay receiver's, mDNS, and port 80 only while
the setup hotspot is up. Firmware updates go over this protocol too (see "Firmware update"), or over USB.

## Discovery
The board announces itself by **mDNS / DNS-SD** on whatever network it is on (its Wi-Fi, or its own setup
hotspot), through the same responder as its AirPlay receiver (host `tentacle.local`):

| | |
|---|---|
| Service | `_tentacle._tcp.local`, port 7202 |
| Instance | the board's name and id suffix, e.g. `Tentacle 6AD5` |
| TXT `id` | board id, 12 hex digits |
| TXT `name` | as the instance |
| TXT `fw` | firmware version |
| TXT `paired` | how many computers are paired (0-4) |
| TXT `open` | `1` while the board takes a pairing request (BOOT was double-pressed, see "Pairing"), else `0` |
| TXT `v` | `1` |

Nothing in it is secret; who answers at that address is proven by the session keys, not by the record.

The CLI asks on every IPv4 interface that is up (a computer often has several: cable, Wi-Fi, VPN, virtual
switches): one PTR question for the service with the unicast-response bit, sent from an ephemeral port to
224.0.0.251:5353 at 0, 0.25, 1 and 2.5 s. The board answers straight to that port (PTR, and SRV, TXT, A in the
additional section). The CLI takes the answer's source address as the board's, stops 0.6 s after the first answer
(4 s without one), and also listens on the mDNS group where it can. In this order it uses:
1. an address given by hand: `--host IP[:PORT]` or `TENTACLE_HOST` (`--host auto` goes back to finding);
2. mDNS;
3. the address each board had last time (`hosts.json` in the CLI's config dir);
4. if all that finds nothing: a try of TCP 7202 on every address of the computer's private /24 networks
   (`TENTACLE_NO_PROBE=1` turns it off). It finds a board behind a router that keeps multicast from crossing
   between Wi-Fi and cable.

`tentacle devices` lists what was found; `--verbose` shows every step.

## Identity
- `board_id`: 6 bytes derived from the chip's factory MAC address (`esp_read_mac(…, ESP_MAC_BT)`: only the name of
  the address, nothing uses a Bluetooth radio). The CLI shows it as 12 hex digits (`device id`); the board's name
  ends in its last 4.
- `client_id`: 16 random bytes made by the CLI once per install.
- `K`: 32-byte key shared by one client and one board. The board keeps up to 4 (NVS, oldest replaced);
  `unpair` forgets them all (from a paired CLI, or `0 unpair` on USB serial).

## Frames
`[type u8][body]`, one frame per record (see "Wi-Fi link").

| type | dir | body |
|---|---|---|
| `0x01` PAIR | → | `c_pub[32] client_id[16] commit[32] name[0..32]` |
| `0x83` PAIR_KEY | ← | `b_pub[32]` |
| `0x03` PAIR_NONCE | → | `n_c[16]` |
| `0x84` PAIR_WAIT | ← | `seconds u8` (how long the board waits for its button) |
| `0x81` PAIRED | ← | `b_pub[32] board_id[6] mac[16]` |
| `0x02` HELLO | → | `client_id[16] c_nonce[16]` |
| `0x82` WELCOME | ← | `b_nonce[16] board_id[6] mac[16]` |
| `0x10` DATA (control) | ↔ | `ctr u32 LE, ciphertext, tag[12]` |
| `0x11` DATA (video) | ← | `ctr u32 LE, ciphertext, tag[12]` |
| `0x12` DATA (audio) | ← | `ctr u32 LE, ciphertext, tag[12]` (after `listen raw on`) |
| `0xE0` ERROR | ← | UTF-8 reason, plain (only before a session exists) |

### Pairing (opened and confirmed with the BOOT button)
The board takes a pairing request only while its **pairing window** is open: a **double press of BOOT** (two
presses within 0.5 s) opens it for 60 s. The LED then flashes twice a second, the display says `PAIRING OPEN`,
and the mDNS TXT `open` is `1`. The window closes when a computer has paired, after the 60 s, or when a paired
computer's session comes up. Outside it PAIR is answered `0xE0 "pairing closed: double-press BOOT on the board,
then pair within 60 s"` and nothing else happens: nothing on the display, no LED, no lock (below). So nothing on
the network can make the board ask for its button; that takes someone at the board first.
```
        ... a person double-presses BOOT on the board: the window is open for 60 s ...
C → B  PAIR        c_pub, client_id, commit = SHA-256(n_c), name     n_c: 16 random bytes, kept back
B → C  PAIR_KEY    b_pub                                             b_priv: fresh per request
C → B  PAIR_NONCE  n_c                                               the board checks it against commit
B → C  PAIR_WAIT   30                                                code on the display, LED blinks fast
        ... a person presses BOOT on the board (short press) ...
B → C  PAIRED      b_pub, board_id, mac                              only now is K stored (NVS)

shared = X25519(b_priv, c_pub)
K      = HKDF-SHA256(ikm=shared, salt="tentacle-pair-v1", info=c_pub || b_pub, L=32)
code   = first 4 bytes of HMAC-SHA256(K, "code" || n_c), big-endian, mod 1 000 000, as 6 digits
mac    = HMAC-SHA256(K, "paired" || client_id)[0:16]
```
- `name` is the computer's host name (UTF-8, at most 32 bytes, optional). The board's display shows it (ASCII; other
  bytes as `?`) with `Code 123456`; the CLI prints the same code. **Compare them, then press BOOT.** A different
  code or name means someone else is asking, or sits in the middle: do not press.
- Why the commitment: the code depends on `n_c`, which the computer reveals only after the board fixed its key, and
  which the computer fixed (by `commit`) before it saw that key. So neither side, and nobody between them, can
  pick a key until the two codes match; a guess is right once in a million.
- No press within 30 s: `0xE0 "pairing not confirmed: BOOT was not pressed within 30 s"`. Nothing was stored.
  The same if the link drops while the board waits (the request is withdrawn), or `n_c` does not match.
- A press counts as an answer only once the request has waited a second; before that it is an ordinary press (the
  stop toggle), so a request from the network cannot catch a press that was already on its way.
- A request that got in while the window was open keeps its 30 s for the button, also when the window's 60 s end
  meanwhile.
- While it waits the board sends a keepalive record every 2 s, and the computer its own: the link stays up.
- The board's answers to PAIR instead of PAIR_KEY (`0xE0`), checked in this order:
  - `this CLI is too old to pair: update tentacle`: a PAIR without `commit` (the 48-byte form without it).
  - `busy: board in use …`: a paired computer has a session, or had one in the last 10 s. No request opens then,
    so the BOOT button stays the stop button while something drives the phone. (The CLI asks again for 20 s: its
    own session, closed for the pairing, counts too.)
  - `pairing closed: double-press BOOT …`: the window is not open. (The CLI stops at once and says to double-press
    BOOT, then pair again.)
  - `busy: another pairing request is open`: only one at a time. A second PAIR never replaces the first one's
    name, code or key.
  - `busy: pairing locked for N s …`: after a request that ended without the button (timeout, dropped link, bad
    nonce) pairing is locked for 5 s, then 10, 20 … up to 5 min; a pairing that succeeds, or 10 min without a
    failed one, starts over at 5 s. Only a request that got through the open window can fail this way.
- Without a display the code cannot be compared: the presses then only say "someone at the board agrees that a
  computer pairs now". Pair right after you asked for it yourself, or fit the OLED (docs/hardware-addons.md).

The double press and the stop button (BOOT, short press: the stop toggle):
- The stop is never delayed: the **first** press of a double press toggles it at once, like any press. The second
  press takes that toggle back and opens the window, so a double press leaves the stop as it was. Three quick
  presses are a double press and then one ordinary press.
- **While a paired computer's session is up** (or was in the last 10 s) a double press opens nothing: it is two
  ordinary presses (stop on, stop off). End the session first.
- **While a request waits for the button** (a pairing code, or the Wi-Fi setup page's network) presses are never
  a double press: the one that comes after the request has waited a second confirms it, any other is an ordinary
  press.
- A double press while the window is open starts its 60 s over. Holding BOOT (10 s: forget the Wi-Fi) is not part
  of a double press.
- In a build without the stop toggle (`flash.sh --no-safety`) a single press does nothing and a double press opens
  the window all the same.
- The setup hotspot does not change any of this: a board that is being set up can be paired over its hotspot.

### Session (every connection)
```
okm   = HKDF-SHA256(ikm=K, salt=c_nonce || b_nonce, info="tentacle-session-v1", L=32)
k_c2b = okm[0:16]   k_b2c = okm[16:32]               # AES-128-GCM keys
mac   = HMAC-SHA256(K, "hello" || c_nonce || b_nonce)[0:16]   # the board proves it holds K
```
Unknown `client_id`: `0xE0 "not paired"`. The client proves it holds K with its first DATA frame: until one decrypts,
the board sends it nothing else.

### DATA
```
nonce = stream u8 || 7 zero bytes || ctr u32 LE      # stream: 'C' client→board, 'B' board control, 'V' board video, 'A' board audio
aad   = the 5 header bytes (type, ctr)
key   = k_c2b for 'C', k_b2c for 'B', 'V' and 'A'
```
Each stream's `ctr` starts at 1 and must grow by exactly 1; anything else is dropped (replay or loss). A gap in `V` is
handled like a video seq gap (wait for the next IDR/CONFIG), one in `A` loses that much sound; a gap in `C` or `B` ends the session (reconnect).

Plaintext:
- `C`: `'c'` + one command line `<id> <command>` (see "Commands"), at most 511 bytes; or `'u'` + offset (u32 LE) +
  up to 4096 bytes of a firmware image (see "Firmware update"). Both share the stream's counter.
- `B`: `'r'` + reply line `<id> ok…|<id> err…`, `'s'` + status JSON (once a second, see "Status"), or `'l'` + new
  lines of the board's log (`logs follow on`, debug builds; `\n` escaped as in `logs`). At most 514 bytes each.
- `V`: one video chunk (see "Video").
- `A`: one phone audio record (see "Phone audio").

## Commands
`<id>` is a decimal request id chosen by the CLI; the reply echoes it: `<id> ok [text]` or `<id> err <reason>`.

Input (the board is the phone's USB mouse and keyboard):

| Command | Behavior |
|---|---|
| `ping` | `ok`. Answers also while stopped or asleep. |
| `pace` | `ok <n>`: the version of how moves are paced (the CLI recalibrates when it changes). |
| `home` | Slams the pointer into the top-left corner (left first, then up). |
| `move DX DY [SPEED]` | Relative move, paced; SPEED 1-40 mouse units per report (default 10). |
| `abs X Y [STEP]` | With the absolute pointer (the setting `abs`, on by default since 0.4.0; before, `flash.sh --abs` builds; the status has `"abs":1`): absolute position, 0-32767 across the screen; while the button is held it glides (STEP per report). With the relative pointer: `err the absolute pointer is off (config abs on, then restart the board)`. |
| `press`, `release`, `click` | The left button. |
| `press_for MS` | Holds the left button; it comes up by itself after MS (1-60000), or at `release`, `click` or `reset`. Moves in between drag. |
| `flick DX DY [SPEED]` | Press, move, release with no pause between: a fast swipe. |
| `scroll V H` | Wheel. |
| `type TEXT` | Types UTF-8 text with the keys of the phone's hardware keyboard layout (`kbd layout`). `us` types printable ASCII; `de` (Apple's German layout) also `ä ö ü Ä Ö Ü ß § ° € ´`, and `á é í ó ú à è ì ò ù â ê î ô û ñ ã õ` and their capitals by a dead key and the letter (`^`, `` ` `` and `~` too: the dead key, then space). A character the layout has no keys for: `err cannot type on layout de: <the characters>`, and nothing is typed. |
| `key COMBO` | Keys joined by `+`: `enter esc tab backspace delete home end pageup pagedown space up down left right cmd ctrl alt shift` or one character, e.g. `cmd+h`. A character is the key that types it on the layout (`cmd+z` on `de` is the key labelled Z; iOS matches shortcuts by the character): characters that need a dead key cannot be in a combo. `key eject` toggles the on-screen keyboard (do not retry it after a lost reply: a second press reverses the first). |
| `homescreen` | The consumer-control Home key: iOS goes to the Home Screen. |
| `reset` | Releases all buttons, keys and consumer controls in place. Safe to retry. |

All but `ping` and `pace` answer `err stopped (safety button)` while the stop toggle is on (BOOT, short press), and
`err phone usb asleep (unlock the phone)` when the phone suspended USB and does not wake. When the session ends,
the board lets go of a button the session held.

While the board's web interface has control of the phone (firmware 0.3.0, docs/manual.md "The web interface"),
every command of this session except `ping` and `pace` that reaches the HID code answers
`err busy: the web interface has control of the phone (release it there, or it lapses 5 min after the page's last
input)`. Control goes back when the page releases it, closes, or sends no input for 5 minutes; the page takes it by
itself only after 60 s without input from the session. Board commands (`stream`, `snap`, `net`, `update`, …) are
not affected.

Board:

| Command | Behavior |
|---|---|
| `stream on\|off` | Sends every access unit as it comes (`V`), starting with the config and the whole GOP since the last IDR. Off again when the session ends. |
| `stream on at IDR_TS N` | From 0.2.3: the same, but the computer's decoder already has the first N units of the GOP whose IDR is stamped IDR_TS (it kept them across a reconnect): the stream goes on with unit N, without config and IDR. `ok resumed`; `ok` when that GOP is gone (a newer IDR, a reboot, unit N was dropped already, or it has fewer units): then it starts from the config and the IDR as `stream on`. Older boards answer `err usage`; send `stream on` then. |
| `snap` | Sends the config and the current GOP once (chunks flagged SNAP), then replies `ok`. `err no video (start screen mirroring)`, `err waiting for keyframe` (mirroring is up, no IDR yet), `err video frozen (restart screen mirroring)`. |
| `bench KB` | Sends KB kilobytes (1-4096) of filler chunks (flag BENCH), then `ok <ms>`. |
| `res W H`, `fps N` | The mirroring format (default 480 1040, 15 fps since 0.3.2; 30 in 0.2.2, 5 before, and a stored 5 is dropped once), stored on the board; it applies to the next mirroring session. `tentacle video fps N`, `tentacle video res W H`. |
| `kbd layout [us\|de]` | The phone's hardware keyboard layout `type` and `key` use (Settings > General > Keyboard > Hardware Keyboard on the iPhone), stored on the board, `us` until set; `en-US`, `de-DE` are taken too. `ok <layout>`; without a layout only the answer. Unknown: `err unknown layout (us de)`. `tentacle keyboard layout [us\|de]`. |
| `time [EPOCH [OFFSET]]` | From 0.4.1: the board's wall clock (it stamps the recording on the microSD card; boards with a clock keep it while off). EPOCH: UTC seconds (2024-2100), OFFSET: minutes east of UTC, a multiple of 15 (-720 to 840; default 0). `ok EPOCH OFFSET`, or `ok unset` before it was set. The CLI sends it when a session starts, the web page when it opens. |
| `config [KEY VALUE]` | From 0.4.0: the board's settings, kept in NVS. `ok {json}`, after a change with the new values: `{"display":"auto","oled":1,"abs":"on","abs_active":1,"battery":"none","battery_div":"2.00","lang":"auto","name":"Tentacle","restart":0}`. Keys: `display auto\|on\|off` (the status OLED: `auto` looks for one on I2C, `on` draws whether one answers or not, `off` leaves the bus alone; applies at once), `abs on\|off` (the absolute or the relative pointer; after a restart, as it changes the USB descriptor), `battery none\|GPIO` (the LiPo voltage pin, an ADC pin 1-10 except 5 and 6, the OLED's), `battery_div 1.00-10.00` (battery volts per pin volt, 2.00 for a 2:1 divider), `lang auto\|en\|de` (the OLED's texts; `auto` follows the web interface's language, else English), `sdlog off\|log\|video` (from 0.4.1, boards with a microSD slot: the recording on the card, `on` = `video`; read only beside it: `sd` the card's state `none` (no slot) `off` `nocard` `rec` `err`, and `sd_file` the file being written), `name NAME` (as the web interface: 1-20 of `A-Z a-z 0-9 space . _ -`; after a restart). Read only: `oled` 1 while a display is in use, `abs_active` the pointer this boot runs with, `restart` 1 while a stored `abs` or `name` waits for a restart, `bat` the battery in % with a pin set. Wrong: `err unknown setting …`, `err bad value for KEY: …`, `err usage: …`. A paired session only (on USB serial: `err needs the computer's session …`). `tentacle device settings`, `tentacle device set KEY VALUE`. |
| `listen raw on\|off` | Phone audio records on stream `A`. Off again with every new session. |
| `net`, `net {…}` | The board's Wi-Fi, see "Wi-Fi link". |
| `result ID` | `ok <the reply>` the board gave to request ID, from this boot's last 8 replies; `err unknown id …` otherwise. Read only: ask it when a reply is late instead of sending the command again. |
| `logs [OFFSET [MAX]]` | The board's log in RAM, a page at a time: `ok <next> <end> <text>` (`\n` escaped); read until next = end. |
| `log TEXT` | Writes a marker line into that log and to USB serial. |
| `crash` | `ok rst <reset reason>, last dump: <task, pc, backtrace of the last crash \| none>`. |
| `vdbg` | The video stream's state in short form, for a frozen screen. |
| `update begin\|state\|end\|abort` | A firmware update, see "Firmware update". |
| `logs follow on\|off` | Debug builds: every new line of the board's log comes as `'l'` on stream `B` (about every 0.1 s), until `off` or the session ends. Other builds: `err debug build only`. |
| `dbg ARGS` | Debug builds: the serial debug command `dbg ARGS` (`state`, `nvs`, `wifi [scan]`, `reboot`); its output is kept (4 KB at most) and comes back as a page like `logs`: `ok <next> <end> <text>`, the rest with `dbgout <next>`. `dbg btn` and `dbg forget` stay on USB serial (pressing BOOT from the network would skip the person at the board). Other builds: `err debug build only`. |
| `txpower 8-84` | The Wi-Fi's TX power in 0.25 dBm steps; an A/B knob, not stored. |
| `unpair` | The board answers `<id> ok` on the session that asked, then forgets **every** paired computer (all keys in NVS) and closes the connection. A computer's stored key is now useless (`0xE0 "not paired"`): it has to `pair` again, with the BOOT button. `tentacle unpair --board` sends it and forgets the board on the computer too; plain `tentacle unpair` only forgets it on the computer. |

Anything else: `err unknown command`. A line without a numeric id: `err missing request id`.

### USB serial
The board's USB serial port (115200) takes the same `<id> <command>` lines, for debugging and recovery without a
paired computer; replies come back on the port. `stream`, `snap`, `bench`, `listen`, `result`, `update`, `dbg`,
`dbgout`, `logs follow` and `config` need a session
(`err needs the computer's session …`). Besides those lines:
- `0 setup`: `wifi-setup {…}`, the setup hotspot's name and password, or the stored Wi-Fi and the board's address
  (flash.sh / flash.ps1 read it after flashing).
- `0 net {…}`: as `net {…}`; `0 netinfo`: as `net`; `0 status`: the status JSON.
- `0 unpair`: forgets every paired computer.

## Status
Once a second on stream `B`, one JSON object:
```json
{"fw":"0.2.2","up":123456,"rst":"power-on","heap":81234,"psram":6200000,"usb":"ok",
 "net":"sta","ip":"192.168.1.50","sta":1,
 "ap":"streaming","kbd":"de","w":480,"h":1040,"kbps":180,"fps":4.8,"idr":2000,"gop":42000,"vbl":0,"vage":120,"ovf":0,"fz":0,
 "link":{"kbps":350},"aud":{"pk":0,"seg":0,"snd":0,"vol":0,"drop":0,"raw":0},"lost":0,
 "hb":512,"mgap":1100,"rxkbps":240,"rssi":-58,"txp":78,"ps":"none","stall":0}
```
- `fw` firmware version, `up` ms since boot, `rst` reset reason of this boot
  (`power-on|brownout|watchdog|crash|software|usb|other`), `heap` / `psram` free bytes.
- `usb`: `ok`, `asleep` (the phone suspended USB) or `off` (not plugged into a host). `abs` 1 while the board runs with the absolute pointer (the setting `abs`; missing with the relative one).
  `bat` (0.4.0, full status only): the battery in %, with a battery pin set.
- `net`: `sta` (on a Wi-Fi), `none` (a Wi-Fi is stored but not connected) or `ap` (setup hotspot); `ip`; `sta`:
  stations on the hotspot, or 1/0 connected.
- `ap`: the AirPlay receiver, `idle|connected|streaming`. `w`, `h`: the mirroring format. `kbps`, `fps`: video
  coming in; `idr` ms between the last two IDRs; `gop` bytes buffered since the last IDR (the part still kept); `vbl` (0.2.3) bytes
  of the buffer this session's stream has not sent yet: above 0 the computer is still
  catching up with what was buffered, 0 it is at the live edge (also 0 without `stream on`); `vage` ms since the last
  frame; `ovf` times the buffer was full and dropped the start of the chain since an IDR; `fz` 1: that happened and
  this session's stream is not on the chain, no new picture until the next IDR (restart Screen Mirroring).
- `lost`: AirPlay video packets the board dropped for good since boot. After one, the frames that follow may not
  decode until the next IDR.
- `link.kbps`: bytes out to the computer in the last second.
- `kbd`: the keyboard layout (`kbd layout`).
- The mirror data socket (why the picture pauses): `hb` heartbeat and streaming reports (types 0x02, 0x05) since
  boot; `mgap` ms of the longest silence on it in the last 60-120 s (with the one going on); `rxkbps` kbit/s on it.
  The Wi-Fi: `rssi` dBm (0 without a network), `txp` TX power in 0.25 dBm, `ps` power save `none|min|max` (modem
  sleep is off while the phone mirrors, from 0.2.2).
- `stall` 1: the sender keeps the session up (heartbeats) but sent no picture for 15 s, although the phone took input
  (any HID command but `ping`, `pace`, `reset`) at least 2 s ago and after the last picture. The CLI then marks a
  screenshot stale and says to restart Screen Mirroring. A tap that changes nothing on screen sends no picture
  either, so it is a hint, not proof.
- `aud`: see "Phone audio".

Should the object ever outgrow a reply (514 bytes), the board sends a short form instead:
`{"fw","up","rst","usb","net","ip","ap","kbd","fz","stall","lost","vbl"}`. The CLI merges every status into what it has.

## Video
Each chunk: `[seq u16 LE][flags u8][payload]`, payload up to 1380 bytes.
flags: bit0 START of access unit, bit1 END of access unit, bit2 IDR, bit3 CONFIG (payload is SPS+PPS),
bit4 BENCH (filler, ignore), bit5 SNAP (part of a `snap` reply), bit6 BEHIND (0.2.3: when the unit went out, newer
units were already waiting on the board behind it: the computer is replaying buffered video, not at the live edge;
the last unit of a replay goes out without it). Older CLIs ignore unknown bits.
Payload is Annex-B (`00 00 00 01` start codes). On a START chunk the first 4 payload bytes are a u32 LE
board timestamp (ms since boot, when the board got the unit from the iPhone), followed by the Annex-B data. With
`up` from the status (the board's ms when it sent the status) the CLI turns it into the picture's true age: a unit
replayed from the GOP keeps the time it was taken, not when it reached the computer.
`seq` increments per chunk; after a gap the CLI waits for the next IDR/CONFIG.

The board keeps the chain of units since the last IDR in a ring (4.5 MB, numbered from the IDR on, 0): while the
IDR is still in it, a stream can start from it; once the ring was full and dropped the oldest units, a stream that is
on the chain goes on (however far behind, as long as its next unit is still kept), anyone else needs the next IDR
(`fz`). The board never skips ahead for a slow receiver. A new session, and every `stream on`, starts from the IDR
(unless `stream on at` goes on from a unit the computer has, which also works after the IDR was dropped). A still screen
gets an IDR only rarely, so the GOP can hold minutes: a blinking text cursor at 30 fps adds about 300 KB a minute,
and over a weak link (50 kbit/s) the replay of minutes takes minutes. Chunks are sent as fast as the TCP link takes
them (no pacing); BEHIND and `vbl` say how far behind the computer still is.

## Phone audio
What the iPhone plays while it mirrors (AirPlay audio). After `listen raw on` the board sends the audio records on
stream `A` (frame `0x12`), and the CLI transcribes them (`tentacle listen`, whisper.cpp on the computer).
- A record is `[seq u16 LE][flags u8][payload]` with flag `0x80` set and its own seq; payload
  `[u32 LE board ms][u32 LE RTP ts][AAC-ELD frame]`.
- The board decrypts the mirroring audio (AAC-ELD, 44.1 kHz stereo, 480 samples per frame, AudioSpecificConfig
  `F8 E8 50 00`). Only sound goes out: a segment starts with the first sounding frame (`START`, 0x01) and ends
  500 ms after the last one with an empty `END` (0x02) record. A session that joins mid-segment gets `START` on its
  first frame.
- Status `aud`: `pk` packets received, `seg` segments, `snd` 1 while one goes on, `vol` the sender's volume in dB
  (-144 muted), `drop` records lost on the way to the computer (queue full, a failed send, or nobody asked), `raw`
  1: records go to this session.

## Wi-Fi link
- **TCP port 7202**, one computer at a time (a second connection waits until the first is gone). Records
  `[u16 LE length][frame]` carry the frames above. A zero-length record is a keepalive: the CLI sends one every
  2 s, and the board drops a link quiet for 6 s. The board sends status every second (keepalives while a pairing
  request waits), so the CLI drops a link quiet for 5 s.
- A connection that has neither a session (HELLO, then a DATA frame that decrypts) nor an open pairing request
  within 10 s is closed, so a stranger cannot hold the board's only link.
- **Reconnecting.** When the link drops (Wi-Fi gone, the board moved), the CLI's daemon opens a new session on the
  next request: it finds the board again (the steps under "Discovery") and sends `stream on`; video starts over
  from the GOP's IDR. A board that is not found or not reachable is tried again after 1 s, then 2, 4 … 15 s at
  most; requests in between get the last error at once.
- **`net`**: `ok {"mode","ssid","pass","ip"}`, the board's Wi-Fi. `net {"mode":"ap"}` or
  `net {"mode":"sta","ssid":…,"pass":…}` changes it (the iPhone has to mirror over the same network). The board
  answers `ok` first and changes 0.4 s later, which ends the session that asked; the CLI finds the board again on
  the new network.
  - `sta`: stored and joined at once, without the BOOT button (a paired computer is trusted already); a name over
    32 bytes or a password that is neither empty nor 8-63 bytes is ignored. A network that is out of reach or has
    the wrong password is retried forever (status `net` `none`); the board does not fall back to its hotspot, and
    is not reachable meanwhile: hold BOOT 10 s for the setup hotspot.
  - `ap`: forgets the stored Wi-Fi and opens the **setup hotspot** (mode `ap`; the same as holding BOOT 10 s):
    WPA2, the board's own random password, and on it a setup page (port 80, captive portal) where a network is
    chosen and then confirmed with the BOOT button. In mode `sta` port 80 only forwards to the web interface on `https://` (443), once the board has an address. A new board is set up
    there; the CLI reaches a board on its hotspot too (join it, the board is at 192.168.4.1).
  - `pass` is the setup hotspot's password in mode `ap` and empty in mode `sta`: a stored Wi-Fi password never
    leaves the board.

## Firmware update
`tentacle update FILE.bin` sends a new firmware (the sketch's `.ino.bin`, not the merged image) over the session;
the board writes it into its other OTA slot, checks it and restarts into it.

1. `update begin SIZE SHA256 SIG [VERSION]`: SIZE bytes, SHA256 the image's SHA-256 (64 hex digits), SIG its
   signature (128 hex digits) or `-` for none, VERSION what the CLI found in the image (`tentacle-fw:0.2.0;`,
   version.h's `FW_MARKER`). The board checks the signature **before it takes a byte** and answers `ok ready`, or
   `err …`: `unsigned firmware: …`, `bad signature: …`, `busy: an update is already running`, `image too big …`.
2. The image in `'u'` records on stream `C`, in order, 4096 bytes each (the last one less). The board writes each to
   flash on the task that reads the socket, so TCP's flow control paces the sender; video and status keep going in
   between. A chunk out of order, too large or past SIZE ends the update.
3. `update state`: `ok ready <got> <size>` while receiving, `ok done`, `ok idle`, or `err <why the last one
   stopped>`. The CLI asks every 256 KB.
4. `update end`: the board checks that it got SIZE bytes with that SHA-256, the signature once more, and the image
   itself (`esp_ota_end`: header, chip, segments, checksum), then sets the boot partition, answers
   `ok restarting into <VERSION>` and restarts 0.3 s later. Any failure: `err …`, and the board keeps running the
   old firmware.

`update abort`, the session ending, the link dropping or 15 s without a chunk stop an update (`esp_ota_abort`); the
old firmware stays. Only one update at a time.

**Signature.** A release build takes only images signed with the Tentacle release key, Ed25519:
```
message   = "tentacle-fw-v1" || SHA-256(image)          # 14 + 32 bytes
signature = Ed25519(release private key, message)       # 64 bytes, raw
signed file = image || signature || "TNTSIG1\n"         # the last 72 bytes: signature + 8-byte magic
```
The public key is compiled into the firmware (`hardware/hid_bridge/ota_pubkey.h`). `tentacle keygen DIR` makes a key
pair (`DIR/tentacle-release.key`, the 32-byte seed as hex: keep it out of any repository; `.pub` and the C array
for ota_pubkey.h are printed), `tentacle sign FILE.bin --key KEYFILE` (or `TENTACLE_RELEASE_KEY`) writes
the signature into the file (in place; signing again replaces it), and `tentacle update FILE.bin` cuts it off and
sends `SIZE`, `SHA256` and `SIG` of the image alone, as do the board's web page and `tentacle-flash`: the board
never sees the trailer. A file without the trailer goes out unsigned (`SIG` = `-`). A **debug build**
(`TENTACLE_DEBUG`) takes unsigned images too (`SIG` = `-`); a signature that is there but wrong is refused by every
build. The status of `dbg state` has `ota.unsigned_ok`.

**Rollback.** The Arduino core's bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. The new firmware starts on
trial (`ESP_OTA_IMG_PENDING_VERIFY`): it is kept (`esp_ota_mark_app_valid_cancel_rollback`) once a computer's
session comes up, or once its network (station with an address, or the setup hotspot) has been up for 30 s. Merely
running is not enough, so an image that lost its Wi-Fi is not kept. A restart before that (a crash, the watchdog, a
hang reset by hand) boots the previous firmware again, and so does 2 min without network and session
(`esp_ota_mark_app_invalid_rollback_and_reboot`). The CLI waits up to 90 s for the board to come back and compares the `fw`
of its first status with the version it sent: the same → `updated: OLD -> NEW`; the old one → the new firmware did
not start properly and the board went back. `dbg state` shows `ota.rolled_back` (the slot that was given up).

Test vectors: `cli/testdata/vectors.json` (checked by `hardware/tests` and `go test ./cli/...`): `pair_nonce`,
`pair_commit` and `pair_code` are `n_c`, `SHA-256(n_c)` and the code for the vectors' `K`.
