# /// script
# dependencies = ["cryptography>=42"]
# ///
"""Drives the simulated board (cli_sim) as a CLI would, with an independent client (Python's cryptography):
pairing with the BOOT button and the code, sessions, replays, commands, status and video. uv run test_cli_sim.py"""
import hashlib, hmac, json, os, struct, subprocess, sys, tempfile, time
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0

def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += not cond

def hkdf(ikm, salt, info): return HKDF(hashes.SHA256(), 32, salt, info).derive(ikm)
def mac(k, m): return hmac.new(k, m, hashlib.sha256).digest()[:16]

class Board:
    def __init__(self, nvs, **env):
        self.p = subprocess.Popen([os.path.join(HERE, "cli_sim"), os.path.join(HERE, "screen.h264")], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={**os.environ, "CLI_SIM_NVS": nvs, **env})
        os.set_blocking(self.p.stdout.fileno(), False)
        os.set_blocking(self.p.stderr.fileno(), False)
        self.buf = b""
        self.err = b""

    def log(self):
        """What the board printed so far (commands, what its display shows)."""
        chunk = self.p.stderr.read()
        if chunk: self.err += chunk
        return self.err.decode()

    def send(self, frame):
        self.p.stdin.write(struct.pack("<H", len(frame)) + frame)
        self.p.stdin.flush()

    def recv(self, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            if len(self.buf) >= 2:
                n = struct.unpack("<H", self.buf[:2])[0]
                if len(self.buf) >= 2 + n:
                    f, self.buf = self.buf[2:2 + n], self.buf[2 + n:]
                    return f
            chunk = self.p.stdout.read(65536)
            if chunk: self.buf += chunk
            else: time.sleep(0.005)
        return None

    def close(self):
        time.sleep(0.05)
        self.log()
        self.p.kill()
        self.p.wait()
        return self.log()

class Client:
    def __init__(self):
        self.priv = X25519PrivateKey.generate()
        self.pub = self.priv.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        self.id = os.urandom(16)
        self.k = None

    def pair(self, b, name=b"Test-PC", wait=6.0, nonce=None):
        """PAIR -> PAIR_KEY -> PAIR_NONCE -> PAIR_WAIT -> (BOOT) -> PAIRED. Returns the last frame; self.code is the
        code this side shows."""
        self.code = None
        nc = os.urandom(16)
        b.send(b"\x01" + self.pub + self.id + hashlib.sha256(nc).digest() + name)
        f = b.recv()
        if not f or f[0] != 0x83: return f
        b_pub = f[1:33]
        k = hkdf(self.priv.exchange(X25519PublicKey.from_public_bytes(b_pub)), b"esp32agent-pair-v1", self.pub + b_pub)
        b.send(b"\x03" + (nonce if nonce is not None else nc))
        f = b.recv()
        if not f or f[0] != 0x84: return f
        self.code = "%06d" % (int.from_bytes(hmac.new(k, b"code" + nc, hashlib.sha256).digest()[:4], "big") % 1000000)
        f = b.recv(wait)
        if not f or f[0] != 0x81: return f
        self.k = k if mac(k, b"paired" + self.id) == f[39:55] and f[1:33] == b_pub else None
        return f

    def hello(self, b):
        cn = os.urandom(16)
        b.send(b"\x02" + self.id + cn)
        f = b.recv()
        if not f or f[0] != 0x82: return f
        bn = f[1:17]
        self.proved = mac(self.k, b"hello" + cn + bn) == f[23:39]
        okm = hkdf(self.k, cn + bn, b"esp32agent-session-v1")
        self.c2b, self.b2c, self.kb2c = AESGCM(okm[:16]), AESGCM(okm[16:]), okm[16:]
        self.tx, self.rx = 0, {"B": 0, "V": 0}
        return f

    def seal(self, plain, ctr=None):
        self.tx = ctr if ctr is not None else self.tx + 1
        hdr = b"\x10" + struct.pack("<I", self.tx)
        ct = self.c2b.encrypt(b"C" + bytes(7) + struct.pack("<I", self.tx), plain, hdr)
        return hdr + ct[:-16] + ct[-16:-4]

    def open(self, f):
        stream = "V" if f[0] == 0x11 else "B"
        ctr = struct.unpack("<I", f[1:5])[0]
        # a 12-byte tag can't be checked by AESGCM directly: recompute the full tag and compare its first 12 bytes
        nonce = stream.encode() + bytes(7) + f[1:5]
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        ks = Cipher(algorithms.AES(self.kb2c), modes.CTR(nonce + b"\x00\x00\x00\x02")).encryptor()
        plain = ks.update(f[5:-12]) + ks.finalize()
        if self.b2c.encrypt(nonce, plain, f[:5])[-16:-4] != f[-12:]: return stream, ctr, None
        return stream, ctr, plain

    def cmd(self, b, line):
        b.send(self.seal(b"c" + line.encode()))

def frames(b, client, seconds):
    out = []
    end = time.time() + seconds
    while time.time() < end:
        f = b.recv(0.2)
        if f is None: continue
        out.append(client.open(f) if f[:1] in (b"\x10", b"\x11") else ("plain", 0, f))
    return out

tmp = tempfile.mkdtemp()
nvs = os.path.join(tmp, "nvs")

# The old PAIR (no commitment, no button) is refused; an unpaired client gets nothing.
b = Board(nvs)
c = Client()
b.send(b"\x01" + c.pub + c.id)
f = b.recv()
check(f and f[0] == 0xE0 and b"too old" in f, "the old PAIR frame (no code, no button) is refused")
f = c.hello(b)
check(f and f[0] == 0xE0 and f[1:] == b"not paired", "an unpaired client gets 'not paired'")
b.close()

# Nobody presses BOOT: an error, nothing stored, and a lock before the next request.
b = Board(nvs, CLI_SIM_BOOT="never", CLI_SIM_CONFIRM_MS="1500")
f = c.pair(b)
check(f and f[0] == 0xE0 and b"not confirmed" in f, "pairing without the button press times out with an error")
shown = c.code
f = c.pair(b)
check(f and f[0] == 0xE0 and b"pairing locked" in f, "... and the next request is locked out for a few seconds")
f = c.hello(b)
check(f and f[0] == 0xE0 and f[1:] == b"not paired", "... and nothing was stored")
log = b.close()
check("confirm: Test-PC | Code " + (shown or "?") in log, "the board showed the computer's name and the same code")

# A press that comes too soon to be an answer (within a second of the request) does not pair.
b = Board(nvs, CLI_SIM_BOOT="300", CLI_SIM_CONFIRM_MS="2000")
f = c.pair(b)
check(f and f[0] == 0xE0 and b"not confirmed" in f, "a press within a second of the request is not a confirmation")
check("boot: pressed, not an answer" in b.close(), "... the button was pressed, and it counted as a normal press")

# A wrong nonce (not the one committed to) ends the request.
b = Board(nvs)
f = c.pair(b, nonce=os.urandom(16))
check(f and f[0] == 0xE0 and b"bad pairing nonce" in f, "a nonce that does not match the commitment is refused")
check("confirm:" not in b.close(), "... before anything is shown on the board")

# Only one open request: a second PAIR is refused and does not replace the first one's name, code or key.
b = Board(nvs, CLI_SIM_BOOT="2500")
s2 = Client()
nc = os.urandom(16)
b.send(b"\x01" + c.pub + c.id + hashlib.sha256(nc).digest() + b"First-PC")
f1 = b.recv()
b.send(b"\x01" + s2.pub + s2.id + hashlib.sha256(os.urandom(16)).digest() + b"Stranger")
f2 = b.recv()
check(f1 and f1[0] == 0x83 and f2 and f2[0] == 0xE0 and b"another pairing request is open" in f2,
      "a second PAIR while one is open gets 'busy'")
k1 = hkdf(c.priv.exchange(X25519PublicKey.from_public_bytes(f1[1:33])), b"esp32agent-pair-v1", c.pub + f1[1:33])
b.send(b"\x03" + nc)
f = b.recv()
b.send(b"\x01" + s2.pub + s2.id + hashlib.sha256(os.urandom(16)).digest() + b"Stranger")
f2 = b.recv()
check(f and f[0] == 0x84 and f2 and f2[0] == 0xE0 and b"another pairing request is open" in f2,
      "... also while the code is shown")
f = b.recv(6.0)
check(f and f[0] == 0x81 and mac(k1, b"paired" + c.id) == f[39:55], "... and the first request completes with its own key")
log = b.close()
check("confirm: First-PC" in log and "Stranger" not in log, "... the display never showed the second one")

# The link drops while the request waits: withdrawn, nothing stored.
b = Board(nvs, CLI_SIM_BOOT="never")
d = Client()
nc = os.urandom(16)
b.send(b"\x01" + d.pub + d.id + hashlib.sha256(nc).digest() + b"Gone-PC")
f = b.recv()
b.send(b"\x03" + nc)
f = b.recv()
check(f and f[0] == 0x84, "a request waits for the button")
b.send(b"")  # the link dropped (a zero-length record on the sim's pipe)
time.sleep(0.3)
check("pairing request withdrawn" in b.log(), "the link drops while waiting: the request is withdrawn")
f = d.hello(b)
check(f and f[0] == 0xE0 and f[1:] == b"not paired", "... and nothing was stored")
b.close()

# Pairing is closed until BOOT is double-pressed at the board: a request then gets an error and nothing else
# happens (no display, no lock). After the double press one computer can pair, which closes it again.
b = Board(nvs, CLI_SIM_OPEN="1500")
e = Client()
f = e.pair(b)
check(f and f[0] == 0xE0 and f[1:] == b"pairing closed: double-press BOOT on the board, then pair within 60 s",
      "PAIR before BOOT was double-pressed: 'pairing closed'")
f = e.pair(b)
check(f and f[0] == 0xE0 and b"pairing closed" in f, "... again at once: still closed, and no lock counted up")
check("confirm:" not in b.log(), "... and nothing is shown on the board")
time.sleep(1.7)
f = e.pair(b)
check(f and f[0] == 0x81 and e.k is not None, "after the double press the request goes through (no lock from the refused ones)")
check("boot: double press, pairing open" in b.log(), "... the board was double-pressed")
g = Client()
f = g.pair(b)
check(f and f[0] == 0xE0 and b"pairing closed" in f, "the window closed with that pairing: the next computer needs a new double press")
b.close()

# A double press while a paired computer's session is up opens nothing (two ordinary presses of the stop button).
b = Board(nvs, CLI_SIM_OPEN="700")
e.hello(b)
e.cmd(b, "1 ping")
frames(b, e, 1.2)
check("boot: double press, not opened (board in use)" in b.log(), "a double press during a session opens no pairing window")
b.close()

# Pair (BOOT pressed), then a session: board proves K, commands get replies, status arrives.
b = Board(nvs)
f = c.pair(b)
check(f and f[0] == 0x81 and c.k is not None, "pairing gives a key both sides derive (mac verifies)")
check(f[33:39] == bytes.fromhex("246f28aa6ad5"), "PAIRED carries the board id")
check("confirm: Test-PC | Code " + c.code in b.log(), "board and computer show the same 6-digit code")
c.hello(b)
check(c.proved, "WELCOME proves the board holds K")
c.cmd(b, "1 ping")
got = frames(b, c, 1.5)
replies = [p for s, _, p in got if s == "B" and p and p[:1] == b"r"]
status = [p for s, _, p in got if s == "B" and p and p[:1] == b"s"]
check(replies == [b"r1 ok"], f"reply to the CLI's own command ({replies})")
check(status and json.loads(status[0][1:])["ap"] == "streaming", "status reaches the CLI")
ctrs = [n for s, n, p in got if s == "B"]
check(ctrs == list(range(1, len(ctrs) + 1)), "board control counters go 1, 2, 3 …")
check(all(p is not None for _, _, p in got), "every board frame verifies")

# While a paired computer's session is up, a pairing request is refused at once (BOOT stays the stop button).
x = Client()
b.send(b"\x01" + x.pub + x.id + hashlib.sha256(os.urandom(16)).digest() + b"Stranger")
got = frames(b, c, 0.8)
check(any(s == "plain" and p[:1] == b"\xe0" and b"board in use" in p for s, _, p in got), "PAIR during a session: 'busy: board in use'")
check("confirm: Stranger" not in b.log(), "... and nothing is shown on the board")

# Video: stream on, reassemble, decode.
c.cmd(b, "2 stream on")
got = frames(b, c, 2.5)
video = [p for s, _, p in got if s == "V"]
h264 = b""
for p in video:
    flags, payload = p[2], p[3:]
    if flags & 1: payload = payload[4:]  # START: board ms first
    h264 += payload
check(len(video) > 10 and h264.startswith(b"\x00\x00\x00\x01"), f"video chunks arrive ({len(video)}, {len(h264)} bytes)")
png = subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "h264", "-i", "pipe:0", "-frames:v", "1", "-f", "image2pipe", "-vcodec", "png", "pipe:1"],
                     input=h264, capture_output=True).stdout
open(os.path.join(tmp, "got.h264"), "wb").write(h264)
check(png[:8] == b"\x89PNG\r\n\x1a\n", f"the streamed H.264 decodes to a PNG ({tmp}/got.h264)")

# Replay: the board drops the session.
b.send(c.seal(b"c3 ping", ctr=c.tx))  # same counter again
got = frames(b, c, 0.8)
check(any(s == "plain" and p == b"" for s, _, p in got), "a replayed frame drops the session")
log = b.close()
check("cmd: 3 ping" not in log, "the replayed command never ran")

# The pairing survives a restart (NVS); a forged frame before auth gets nothing.
b = Board(nvs)
c.hello(b)
b.send(b"\x10" + struct.pack("<I", 1) + os.urandom(20))
check(frames(b, c, 1.2) == [], "a forged first frame: no auth, nothing sent back")
c.cmd(b, "1 ping")  # the counter is still 1 for the board: the forged frame did not count
got = frames(b, c, 1.2)
check([p for s, _, p in got if s == "B" and p and p[:1] == b"r"] == [b"r1 ok"], "pairing kept across a restart")
b.close()

# A second client is not paired; a stranger can't use the first one's id without K.
b = Board(nvs)
s = Client(); s.id, s.k = c.id, os.urandom(32)
s.hello(b)
check(not s.proved, "a stranger with a copied client id can't verify the board")
s.cmd(b, "1 ping")
check(frames(b, s, 1.0) == [], "… and its commands are ignored")
b.close()

# Frozen GOP: snap says so, status fz 1.
b = Board(nvs, CLI_SIM_FROZEN="1")
c.hello(b)
c.cmd(b, "1 snap")
got = frames(b, c, 1.3)
check(any(p and b"err video frozen" in p for s, _, p in got if s == "B"), "snap on a frozen GOP says restart mirroring")
check(any(p and p[:1] == b"s" and json.loads(p[1:])["fz"] == 1 for s, _, p in got if s == "B"), "status fz 1")
b.close()

# wait_idr without AirPlay streaming (boot): keep the "start mirroring" hint.
b = Board(nvs, CLI_SIM_WAIT_IDR="1", CLI_SIM_AP="idle")
c.hello(b)
c.cmd(b, "1 snap")
got = frames(b, c, 1.3)
check(any(p and b"err no video" in p for s, _, p in got if s == "B"), "snap wait_idr + ap idle → err no video")
b.close()

# wait_idr while AirPlay is streaming: distinct keyframe wait (ports #68 / #25).
b = Board(nvs, CLI_SIM_WAIT_IDR="1", CLI_SIM_AP="streaming")
c.hello(b)
c.cmd(b, "1 snap")
got = frames(b, c, 1.3)
check(any(p and b"err waiting for keyframe" in p for s, _, p in got if s == "B"), "snap wait_idr + ap streaming → waiting for keyframe")
b.close()

print("all passed" if not fails else f"{fails} FAILED")
sys.exit(fails != 0)
