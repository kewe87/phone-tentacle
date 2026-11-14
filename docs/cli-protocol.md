# CLI protocol (ESP32 ⇄ computer, over Wi-Fi)

The `esp32agent` CLI finds the board on the local network (mDNS), pairs with it and talks to it on **TCP port
7202**. That is the board's only link to a computer: the firmware has no Bluetooth and there is no phone app. The
link is authenticated and encrypted here, in the application layer; pairing is confirmed by a person at the board,
with its BOOT button.

Open ports: 7202 (this protocol), the AirPlay receiver's, mDNS, and port 80 only while
the setup hotspot is up. Firmware updates go over USB.

## Discovery
The board announces itself by **mDNS / DNS-SD** on whatever network it is on (its Wi-Fi, or its own setup
hotspot), through the same responder as its AirPlay receiver (host `iphone-agent.local`):

| | |
|---|---|
| Service | `_esp32agent._tcp.local`, port 7202 |
| Instance | the board's name and id suffix, e.g. `ESP32 Agent 6AD5` |
| TXT `id` | board id, 12 hex digits |
| TXT `name` | as the instance |
| TXT `fw` | firmware version |
| TXT `paired` | how many computers are paired (0-4) |
| TXT `v` | `1` |

Nothing in it is secret; who answers at that address is proven by the session keys, not by the record.

The CLI asks on every IPv4 interface that is up (a computer often has several: cable, Wi-Fi, VPN, virtual
switches): one PTR question for the service with the unicast-response bit, sent from an ephemeral port to
224.0.0.251:5353 at 0, 0.25, 1 and 2.5 s. The board answers straight to that port (PTR, and SRV, TXT, A in the
additional section). The CLI takes the answer's source address as the board's, stops 0.6 s after the first answer
(4 s without one), and also listens on the mDNS group where it can. In this order it uses:
1. an address given by hand: `--host IP[:PORT]` or `ESP32AGENT_HOST` (`--host auto` goes back to finding);
2. mDNS;
3. the address each board had last time (`hosts.json` in the CLI's config dir);
4. if all that finds nothing: a try of TCP 7202 on every address of the computer's private /24 networks
   (`ESP32AGENT_NO_PROBE=1` turns it off). It finds a board behind a router that keeps multicast from crossing
   between Wi-Fi and cable.

`esp32agent devices` lists what was found; `--verbose` shows every step.

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

### Pairing (confirmed with the BOOT button)
```
C → B  PAIR        c_pub, client_id, commit = SHA-256(n_c), name     n_c: 16 random bytes, kept back
B → C  PAIR_KEY    b_pub                                             b_priv: fresh per request
C → B  PAIR_NONCE  n_c                                               the board checks it against commit
B → C  PAIR_WAIT   30                                                code on the display, LED blinks fast
        ... a person presses BOOT on the board (short press) ...
B → C  PAIRED      b_pub, board_id, mac                              only now is K stored (NVS)

shared = X25519(b_priv, c_pub)
K      = HKDF-SHA256(ikm=shared, salt="esp32agent-pair-v1", info=c_pub || b_pub, L=32)
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
- While it waits the board sends a keepalive record every 2 s, and the computer its own: the link stays up.
- The board's answers to PAIR instead of PAIR_KEY (`0xE0`):
  - `busy: another pairing request is open`: only one at a time. A second PAIR never replaces the first one's
    name, code or key.
  - `busy: board in use …`: a paired computer has a session, or had one in the last 10 s. No request opens then,
    so the BOOT button stays the stop button while something drives the phone. (The CLI asks again for 20 s: its
    own session, closed for the pairing, counts too.)
  - `busy: pairing locked for N s …`: after a request that ended without the button (timeout, dropped link, bad
    nonce) pairing is locked for 5 s, then 10, 20 … up to 5 min; a pairing that succeeds, or 10 min without a
    failed one, starts over at 5 s. So the LED and the display cannot be kept busy from the network.
  - `this CLI is too old to pair: update esp32agent`: a PAIR without `commit` (the 48-byte form of older CLIs).
- Without a display the code cannot be compared: the press then only says "someone at the board agrees that a
  computer pairs now". Pair right after you asked for it yourself, or fit the OLED (docs/hardware-addons.md).

### Session (every connection)
```
okm   = HKDF-SHA256(ikm=K, salt=c_nonce || b_nonce, info="esp32agent-session-v1", L=32)
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
- `C`: `'c'` + one command line `<id> <command>` (see "Commands"), at most 511 bytes.
- `B`: `'r'` + reply line `<id> ok…|<id> err…`, or `'s'` + status JSON (once a second, see "Status"). At most 514
  bytes each.
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
| `abs X Y [STEP]` | Only in `flash.sh --abs` builds (status has `"abs":1`): absolute position, 0-32767 across the screen; while the button is held it glides (STEP per report). |
| `press`, `release`, `click` | The left button. |
| `press_for MS` | Holds the left button; it comes up by itself after MS (1-60000), or at `release`, `click` or `reset`. Moves in between drag. |
| `flick DX DY [SPEED]` | Press, move, release with no pause between: a fast swipe. |
| `scroll V H` | Wheel. |
| `type TEXT` | Types printable ASCII. |
| `key COMBO` | Keys joined by `+`: `enter esc tab backspace space up down left right cmd ctrl alt shift` or one character, e.g. `cmd+h`. `key eject` toggles the on-screen keyboard (do not retry it after a lost reply: a second press reverses the first). |
| `homescreen` | The consumer-control Home key: iOS goes to the Home Screen. |
| `reset` | Releases all buttons, keys and consumer controls in place. Safe to retry. |

All but `ping` and `pace` answer `err stopped (safety button)` while the stop toggle is on (BOOT, short press), and
`err phone usb asleep (unlock the phone)` when the phone suspended USB and does not wake. When the session ends,
the board lets go of a button the session held.

Board:

| Command | Behavior |
|---|---|
| `stream on\|off` | Sends every access unit as it comes (`V`), starting with the config and the whole GOP since the last IDR. Off again when the session ends. |
| `snap` | Sends the config and the current GOP once (chunks flagged SNAP), then replies `ok`. `err no video (start screen mirroring)`, `err waiting for keyframe` (mirroring is up, no IDR yet), `err video frozen (restart screen mirroring)`. |
| `bench KB` | Sends KB kilobytes (1-4096) of filler chunks (flag BENCH), then `ok <ms>`. |
| `res W H`, `fps N` | The mirroring format (default 480 1040, 5 fps), stored on the board; it applies to the next mirroring session. |
| `listen raw on\|off` | Phone audio records on stream `A`. Off again with every new session. |
| `net`, `net {…}` | The board's Wi-Fi, see "Wi-Fi link". |
| `result ID` | `ok <the reply>` the board gave to request ID, from this boot's last 8 replies; `err unknown id …` otherwise. Read only: ask it when a reply is late instead of sending the command again. |
| `logs [OFFSET [MAX]]` | The board's log in RAM, a page at a time: `ok <next> <end> <text>` (`\n` escaped); read until next = end. |
| `log TEXT` | Writes a marker line into that log and to USB serial. |
| `crash` | `ok rst <reset reason>, last dump: <task, pc, backtrace of the last crash \| none>`. |
| `vdbg` | The video stream's state in short form, for a frozen screen. |
| `txpower 8-84` | The Wi-Fi's TX power in 0.25 dBm steps; an A/B knob, not stored. |
| `unpair` | The board answers `<id> ok` on the session that asked, then forgets **every** paired computer (all keys in NVS) and closes the connection. A computer's stored key is now useless (`0xE0 "not paired"`): it has to `pair` again, with the BOOT button. `esp32agent unpair --board` sends it and forgets the board on the computer too; plain `esp32agent unpair` only forgets it on the computer. |

Anything else: `err unknown command`. A line without a numeric id: `err missing request id`.

### USB serial
The board's USB serial port (115200) takes the same `<id> <command>` lines, for debugging and recovery without a
paired computer; replies come back on the port. `stream`, `snap`, `bench`, `listen` and `result` need a session
(`err needs the computer's session …`). Besides those lines:
- `0 setup`: `wifi-setup {…}`, the setup hotspot's name and password, or the stored Wi-Fi and the board's address
  (flash.sh / flash.ps1 read it after flashing).
- `0 net {…}`: as `net {…}`; `0 netinfo`: as `net`; `0 status`: the status JSON.
- `0 unpair`: forgets every paired computer.

## Status
Once a second on stream `B`, one JSON object:
```json
{"fw":"0.2.5","up":123456,"rst":"power-on","heap":81234,"psram":6200000,"usb":"ok",
 "net":"sta","ip":"192.168.1.50","sta":1,
 "ap":"streaming","w":480,"h":1040,"kbps":180,"fps":4.8,"idr":2000,"gop":42000,"vage":120,"ovf":0,"fz":0,
 "link":{"kbps":350},"aud":{"pk":0,"seg":0,"snd":0,"vol":0,"drop":0,"raw":0},"lost":0}
```
- `fw` firmware version, `up` ms since boot, `rst` reset reason of this boot
  (`power-on|brownout|watchdog|crash|software|usb|other`), `heap` / `psram` free bytes.
- `usb`: `ok`, `asleep` (the phone suspended USB) or `off` (not plugged into a host). `abs` 1 in `--abs` builds.
- `net`: `sta` (on a Wi-Fi), `none` (a Wi-Fi is stored but not connected) or `ap` (setup hotspot); `ip`; `sta`:
  stations on the hotspot, or 1/0 connected.
- `ap`: the AirPlay receiver, `idle|connected|streaming`. `w`, `h`: the mirroring format. `kbps`, `fps`: video
  coming in; `idr` ms between the last two IDRs; `gop` bytes buffered since the last IDR; `vage` ms since the last
  frame; `ovf` times the GOP buffer filled; `fz` 1: the GOP is full and the stream is not going on from it, no new
  picture until the next IDR (restart Screen Mirroring).
- `lost`: AirPlay video packets the board dropped for good since boot. After one, the frames that follow may not
  decode until the next IDR.
- `link.kbps`: bytes out to the computer in the last second.
- `aud`: see "Phone audio".

Should the object ever outgrow a reply (514 bytes), the board sends a short form instead:
`{"fw","up","rst","usb","net","ip","ap","fz","lost"}`. The CLI merges every status into what it has.

## Video
Each chunk: `[seq u16 LE][flags u8][payload]`, payload up to 1380 bytes.
flags: bit0 START of access unit, bit1 END of access unit, bit2 IDR, bit3 CONFIG (payload is SPS+PPS),
bit4 BENCH (filler, ignore), bit5 SNAP (part of a `snap` reply).
Payload is Annex-B (`00 00 00 01` start codes). On a START chunk the first 4 payload bytes are a u32 LE
board timestamp (ms), followed by the Annex-B data.
`seq` increments per chunk; after a gap the CLI waits for the next IDR/CONFIG.

The board keeps the GOP since the last IDR (up to 4 MB) and, once that is full, a ring of the newest units after
it: a stream that is in step goes on from the ring, anyone else needs the next IDR (`fz`). A new session, and every
`stream on`, starts from the GOP's IDR. A still screen gets an IDR only rarely, so the GOP can hold minutes.

## Phone audio
What the iPhone plays while it mirrors (AirPlay audio). After `listen raw on` the board sends the audio records on
stream `A` (frame `0x12`), and the CLI transcribes them (`esp32agent listen`, whisper.cpp on the computer).
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
    chosen and then confirmed with the BOOT button. Port 80 is closed in every other mode. A new board is set up
    there; the CLI reaches a board on its hotspot too (join it, the board is at 192.168.4.1).
  - `pass` is the setup hotspot's password in mode `ap` and empty in mode `sta`: a stored Wi-Fi password never
    leaves the board.

Test vectors: `cli/testdata/vectors.json` (checked by `hardware/tests` and `go test ./cli/...`): `pair_nonce`,
`pair_commit` and `pair_code` are `n_c`, `SHA-256(n_c)` and the code for the vectors' `K`.
