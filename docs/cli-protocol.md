# CLI protocol (ESP32 ⇄ computer, over BLE and Wi-Fi)

The `esp32agent` CLI talks to the board over BLE next to the iOS app (both can be connected at once). It never uses
OS-level BLE pairing: a stale bond on either side (macOS `CBError 14`, "Peer removed pairing information") cannot
break it, and Windows/Linux need no pairing dialog. Instead the link is authenticated and encrypted here, in the
application layer.

## Discovery
A headless build (the default, `APP_LINK 0`) has only the CLI's identity and service: it advertises that one all
the time while a connection slot is free, and nothing below about the app applies to it.

A build with the app's side (`flash.sh --app-link`) advertises (legacy PDUs, connectable) under one of two
identities at a time:
- the app's: the board's public address, service `...0000` (docs/ble-protocol.md).
- the CLI's: a random static address picked at every boot, service `...0010`, same name `ESP32 Agent XXXX`.
  No central can hold a bond for an address that changes every boot, so the OS never tries to encrypt.

With the app connected it advertises the CLI's (which then gets `owner app`, busy); with the CLI connected, the app's
(the app can always take over); with neither, they take turns, the app's 750 ms and the CLI's 250 ms of each second.
One set, not two at once: two need extended advertising, ~9 KB of the board's internal RAM.

Both addresses lead to the same GATT server. Full UUID = `6e400001-a9e8-4b1b-9c39-6d6f8a3c` + suffix.

Service `...0010`
| Characteristic | UUID suffix | Props | Purpose |
|---|---|---|---|
| `cli_in`  | `...0011` | write | frames from the computer |
| `cli_out` | `...0012` | notify | frames from the board |
| `cli_in_nr` | `...0013` | write without response | the same frames, when one fits a packet (newer firmware; the CLI uses it if present) |

A frame that fits `MTU − 3` goes to `cli_in_nr`: a write request waits for its ATT response, and behind the board's
video that took seconds. Longer frames (the OS splits them into a long write) use `cli_in`. One write at a time, so
frames reach the board in counter order.

A connection that is neither link-encrypted (the app) nor authenticated here within 15 s is dropped, so a stranger
cannot sit on one of the board's connection slots.

## Identity
- `board_id`: the board's 6-byte BT MAC. The CLI shows it as 12 hex digits (`device id`).
- `client_id`: 16 random bytes made by the CLI once per install.
- `K`: 32-byte key shared by one client and one board. The board keeps up to 4 (NVS, oldest replaced);
  `unpair` forgets them all (headless: from the CLI or USB serial; with the app's side: from the app).

## Frames
`[type u8][body]`, one frame per write / notify (writes may be long writes; notifies are at most MTU-3 bytes).

| type | dir | body |
|---|---|---|
| `0x01` PAIR | → | `c_pub[32] client_id[16]` |
| `0x81` PAIRED | ← | `b_pub[32] board_id[6] mac[16]` |
| `0x02` HELLO | → | `client_id[16] c_nonce[16]` |
| `0x82` WELCOME | ← | `b_nonce[16] board_id[6] mac[16]` |
| `0x10` DATA (control) | ↔ | `ctr u32 LE, ciphertext, tag[12]` |
| `0x11` DATA (video) | ← | `ctr u32 LE, ciphertext, tag[12]` |
| `0x12` DATA (audio) | ← | `ctr u32 LE, ciphertext, tag[12]` (Wi-Fi only, after `listen raw on`) |
| `0xE0` ERROR | ← | UTF-8 reason, plain (only before a session exists) |

### Pairing (only within 3 min of board power-on, like the app's)
```
shared = X25519(b_priv, c_pub)                       # b_priv: fresh per pairing
K      = HKDF-SHA256(ikm=shared, salt="esp32agent-pair-v1", info=c_pub || b_pub, L=32)
mac    = HMAC-SHA256(K, "paired" || client_id)[0:16]
```
Outside the window the board answers `0xE0 "pairing window closed: unplug and replug the board, then pair within 3 minutes"`.
An unauthenticated exchange, like BLE Just Works: it guards against everyone who is not there during the window.

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
- `C`: `'c'` + one command line `<id> <command>`, exactly the app's command set (docs/ble-protocol.md). `stream on|off`
  and `snap` act on the CLI's own video, independent of the app's.
- `B`: `'r'` + reply line `<id> ok…|<id> err…` (only replies to the CLI's own commands), or `'s'` + status JSON
  (once a second; the compact form when the full one does not fit). Both statuses carry `owner`
  (`app`, `cli` or `none`: who drives the phone); the CLI's has `busy` too (1 while the app's link is up).
- `V`: one video chunk exactly as on the app's `video` characteristic: `[seq u16 LE][flags u8][payload]`.
- `A`: one phone audio record as on the app's Wi-Fi link (docs/ble-protocol.md, "Wi-Fi audio"), its own seq.

## Phone audio
What the iPhone plays while it mirrors (AirPlay audio), two ways; the CLI's `listen` picks one.
- **Words from the app** (default). The app transcribes the audio on the phone and, when it listens and shares,
  sends the board `hear on|off` and one `heard <text>` per segment (`[en] …` / `[zh] …`, at most 400 bytes). These
  don't count as the app using the phone. The CLI's `heard` answers
  `ok {"app":0|1,"lines":[{"ago":ms,"t":"…"},…]}`: newest first, the last 60 s, as many as fit its link's reply.
  `app` 0: the app doesn't share (not running, listening off, or sharing off).
- **Raw audio** (`listen raw on|off`, CLI only, Wi-Fi link only: `err …` over BLE). The board also sends the CLI
  the audio records (stream `A`, frame `0x12`), and the CLI transcribes them. Off with every new session.
- Status `aud`: `hear` (1: the app shares), `raw` (1: records go to this CLI), with the packet and segment counts.

## Wi-Fi link
When this computer can reach the board's IP (both on the same Wi-Fi, or the computer joined the board's hotspot),
the session moves there and BLE stays connected as the way back.
- **TCP port 7202**, one computer at a time. Records `[u16 LE length][frame]` carry the frames above, unchanged.
  A zero-length record is a keepalive: the CLI sends one every 2 s, and the board drops a link quiet for 6 s.
  The board sends status every second, so the CLI drops a link quiet for 5 s.
- **HELLO/WELCOME as over BLE**, with the paired key. PAIR answers `err pair over Bluetooth`: pairing needs the
  board's pairing window, so whoever pairs is near the board.
- **Moving.** The CLI learns the IP from the status (`net` `ap`/`sta`, `ip`), opens a session on TCP, and sends
  `stream on` on it. A session's first DATA frame on a new link takes over from the same client's session on the
  other link: the old one stops (no replies, status or video), its link stays up. Video starts over from the GOP's
  IDR. Another client's session can't take over. When the TCP link drops, the CLI opens a new session on its BLE
  link; after a failed try it waits 10 s, doubling up to a minute.
- **`net`** (CLI only): `ok {"mode","ssid","pass","ip"}`, the board's Wi-Fi. `net {"mode":"ap"}` or
  `net {"mode":"sta","ssid":…,"pass":…}` changes it, as the app's `net` write does (the iPhone has to mirror over
  the same network). The CLI's status has `link`: `{"via":"ble"|"wifi","mtu":…,"kbps":…}` for its own link.
  - `sta`: stored and joined at once, without the BOOT button (a paired computer is trusted already); a name over
    32 bytes or a password that is neither empty nor 8-63 bytes is ignored. A network that is out of reach or has
    the wrong password is retried forever (status `net` `none`); the board does not fall back to its hotspot.
  - `ap`: forgets the stored Wi-Fi and opens the **setup hotspot** (mode `ap`; the same as holding BOOT 10 s):
    WPA2, the board's own random password, and on it a setup page (port 80, captive portal) where a network is
    chosen and then confirmed with the BOOT button. Port 80 is closed in every other mode.
  - In a headless build `pass` is the setup hotspot's password in mode `ap` and empty in mode `sta`: a stored
    Wi-Fi password never leaves the board.
- `ESP32AGENT_TRANSPORT=ble` (or `esp32agent net via ble`) keeps the session on BLE.

## Sharing the phone with the app
One side drives the phone at a time, and the app wins (`hardware/hid_bridge/src/hid/arbiter.h`). The app runs
unchanged and never sees a new error.
- **While the app's link is up** (an encrypted connection) every CLI command except `crash` answers
  `err busy: the app is using the board`, and the board drops the CLI's link so it doesn't share the radio.
- **The app takes over.** When the app connects during a CLI session, the board stops the CLI's video, releases a
  button it held, sends it a status with `owner: app`, `busy: 1`, and drops its link about 300 ms later. The CLI
  waits 30 s before it tries again.
- **When the app's link goes down** the CLI may connect and drive; it re-homes its pointer on every new session.
- **Settings stay the app's.** `res fps txpower coex app log unpair` answer `err not available from the CLI`
  (`net` is allowed: the Wi-Fi link needs it). Only in a build with the app's side; see below.
- **Video.** The CLI's video runs on its own task and buffers; it only runs while the CLI owns the board.
- **Intents** stay in the app: what the CLI does is not recorded there.

## Headless build (no app, the default)
Built with `APP_LINK 0` the board has no app side at all: no service `...0000`, no bonding, no Wi-Fi video
(7200/7201), no OTA server (7300; updates go over USB). The CLI never gets `err busy`, `owner` is `cli` or `none`,
`busy` is 0, and `heard` answers `{"app":0,"lines":[]}` (`listen raw on` works). The settings are the CLI's:
- `res W H`, `fps N`: the mirroring format, stored on the board; it applies to the next mirroring session.
- `txpower 8-84` (0.25 dBm steps), `coex [bt|balance|wifi]`: radio A/B knobs, not stored; a reboot resets them.
- `log TEXT`: writes a marker line into the board's log (`logs`) and to USB serial.
- `app …`: `err no app: this firmware is driven by the CLI only`.
- `unpair`: the board answers `<id> ok` on the session that asked, then forgets **every** paired computer (all
  keys in NVS, and any BLE bond), drops all BLE and Wi-Fi links, and opens a new 3-minute pairing window. The
  computer's stored key is now useless (`0xE0 "not paired"`): forget the board there too (`esp32agent unpair`)
  and `pair` again within the window. Without `unpair`, the same reset is `<id> unpair` on USB serial.

Test vectors: `cli/testdata/vectors.json` (checked by `hardware/tests` and `go test ./cli/...`).
