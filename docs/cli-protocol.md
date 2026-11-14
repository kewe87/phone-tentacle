# CLI protocol (ESP32 ⇄ computer, over Wi-Fi)

The `esp32agent` CLI finds the board on the local network (mDNS), pairs with it and talks to it on **TCP port
7202**. Nothing needs Bluetooth: no adapter on the computer, no OS pairing. The link is authenticated and
encrypted here, in the application layer; pairing is confirmed by a person at the board, with its BOOT button.

The firmware still has the same frames on a BLE service (see "Bluetooth" at the end); it is no longer needed,
cannot pair, and is going away.

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
- `board_id`: the board's 6-byte BT MAC. The CLI shows it as 12 hex digits (`device id`).
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

### Pairing (over Wi-Fi, confirmed with the BOOT button)
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
  - `busy: another pairing request is open`: only one at a time. A second PAIR (another connection, or the same)
    never replaces the first one's name, code or key.
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
- **Raw audio** (`listen raw on|off`, CLI only, on the Wi-Fi link). The board also sends the CLI
  the audio records (stream `A`, frame `0x12`), and the CLI transcribes them. Off with every new session.
- Status `aud`: `hear` (1: the app shares), `raw` (1: records go to this CLI), with the packet and segment counts.

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
  the new network. The status has `link`: `{"via":"wifi","mtu":…,"kbps":…}`.
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

## Sharing the phone with the app
Only in a build with the app's side (`flash.sh --app-link`); the default build has no app, see the next section.

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
  keys in NVS) and drops all links. A computer's stored key is now useless (`0xE0 "not paired"`): it has to
  `pair` again, with the BOOT button. `esp32agent unpair --board` sends it and forgets the board on the computer
  too; plain `esp32agent unpair` only forgets it on the computer. Without a paired computer the same reset is
  `0 unpair` on USB serial.

## Bluetooth (not needed, going away)
The firmware still advertises a BLE service with the same frames: service `6e400001-a9e8-4b1b-9c39-6d6f8a3c0010`,
characteristics `…0011` (`cli_in`, write), `…0012` (`cli_out`, notify), `…0013` (`cli_in_nr`, write without
response), on a random static address made at every boot. A paired computer could open a session there
(HELLO/WELCOME); **PAIR is refused** (`pair over Wi-Fi (TCP 7202): it needs the BOOT button`). The CLI does not
touch Bluetooth unless `ESP32AGENT_TRANSPORT=ble` is set.

Test vectors: `cli/testdata/vectors.json` (checked by `hardware/tests` and `go test ./cli/...`): `pair_nonce`,
`pair_commit` and `pair_code` are `n_c`, `SHA-256(n_c)` and the code for the vectors' `K`.
