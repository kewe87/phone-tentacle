package proto

import (
	"bytes"
	"crypto/ecdh"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"io"
	"strings"
	"sync"
)

// FakeBoard is the board side of docs/cli-protocol.md, in process, for tests.
type FakeBoard struct {
	ID []byte
	// Boot stands for the BOOT button: called with what the display shows once a pairing request waits; true =
	// pressed (paired), false = not pressed in time. nil: pressed at once.
	Boot func(name, code string) bool
	// PairErr, if set, is the board's answer to PAIR (e.g. "busy: board in use").
	PairErr string
	// Handle answers one command (without the id), e.g. "ok" or "err unknown command".
	Handle func(cmd string) string
	// Upload takes a firmware piece ('u' record: offset, bytes); nil: dropped.
	Upload func(off uint32, data []byte)

	mu       sync.Mutex
	keys     map[string][]byte // client_id hex → K
	conn     *fakeConn
	Commands []string // every command accepted on stream C, in order
}

func NewFakeBoard(id []byte) *FakeBoard {
	return &FakeBoard{ID: id, keys: map[string][]byte{}, Handle: func(string) string { return "ok" }}
}

type fakeConn struct {
	b         *FakeBoard
	toClient  chan []byte
	closeOnce sync.Once
	closed    chan struct{}

	pend             *fakePair // an open pairing request
	c2b, b2c         *AEAD
	authed           bool
	ctrC             uint32
	ctrB, ctrV, ctrA uint32
	Sent             [][]byte
}

type fakePair struct {
	clientID, commit, bPub, k []byte
	name                      string
}

// Connect returns the client end of a new connection; the board serves it until Close.
func (b *FakeBoard) Connect() Link {
	c := &fakeConn{b: b, toClient: make(chan []byte, 1024), closed: make(chan struct{})}
	b.mu.Lock()
	b.conn = c
	b.mu.Unlock()
	return c
}

func (c *fakeConn) Recv() <-chan []byte { return c.toClient }

func (c *fakeConn) Close() error {
	c.closeOnce.Do(func() {
		close(c.closed)
		c.b.mu.Lock()
		close(c.toClient)
		c.b.mu.Unlock()
	})
	return nil
}

// send must hold b.mu.
func (c *fakeConn) send(f []byte) {
	select {
	case <-c.closed:
	default:
		c.Sent = append(c.Sent, f)
		c.toClient <- f
	}
}

func (c *fakeConn) Write(f []byte) error {
	select {
	case <-c.closed:
		return io.ErrClosedPipe
	default:
	}
	if len(f) == 0 {
		return nil
	}
	b := c.b
	b.mu.Lock()
	defer b.mu.Unlock()
	switch f[0] {
	case TPair:
		if b.PairErr != "" {
			c.send(append([]byte{TError}, b.PairErr...))
			return nil
		}
		if len(f) == 1+keySize+idSize {
			c.send(append([]byte{TError}, "this CLI is too old to pair: update tentacle"...))
			return nil
		}
		if len(f) < 1+keySize+idSize+sha256Len || len(f) > 1+keySize+idSize+sha256Len+PairNameMax {
			c.send(append([]byte{TError}, "bad pair request"...))
			return nil
		}
		if c.pend != nil {
			c.send(append([]byte{TError}, "busy: another pairing request is open"...))
			return nil
		}
		priv, _ := ecdh.X25519().GenerateKey(rand.Reader)
		cPub, err := ecdh.X25519().NewPublicKey(f[1 : 1+keySize])
		if err != nil {
			return nil
		}
		shared, _ := priv.ECDH(cPub)
		bPub := priv.PublicKey().Bytes()
		rest := f[1+keySize:]
		c.pend = &fakePair{clientID: bytes.Clone(rest[:idSize]), commit: bytes.Clone(rest[idSize : idSize+sha256Len]),
			name: string(rest[idSize+sha256Len:]), bPub: bPub, k: PairKey(shared, f[1:1+keySize], bPub)}
		c.send(append([]byte{TPairKey}, bPub...))
	case TPairNonce:
		p := c.pend
		if p == nil {
			c.send(append([]byte{TError}, "no pairing request open"...))
			return nil
		}
		c.pend = nil
		if len(f) != 1+nonceLen || !bytes.Equal(PairCommit(f[1:]), p.commit) {
			c.send(append([]byte{TError}, "bad pairing nonce"...))
			return nil
		}
		c.send([]byte{TPairWait, 30})
		if b.Boot != nil && !b.Boot(p.name, PairCode(p.k, f[1:])) {
			c.send(append([]byte{TError}, "pairing not confirmed: BOOT was not pressed within 30 s"...))
			return nil
		}
		b.keys[hex.EncodeToString(p.clientID)] = p.k
		c.send(append(append(append([]byte{TPaired}, p.bPub...), b.ID...), PairMAC(p.k, p.clientID)...))
	case THello:
		if len(f) != 1+idSize+nonceLen {
			return nil
		}
		k := b.keys[hex.EncodeToString(f[1:1+idSize])]
		if k == nil {
			c.send(append([]byte{TError}, "not paired"...))
			return nil
		}
		cNonce := f[1+idSize:]
		bNonce := make([]byte, nonceLen)
		io.ReadFull(rand.Reader, bNonce)
		c2b, b2c := SessionKeys(k, cNonce, bNonce)
		c.c2b, _ = NewAEAD(c2b)
		c.b2c, _ = NewAEAD(b2c)
		c.authed, c.ctrC, c.ctrB, c.ctrV, c.ctrA = false, 0, 0, 0, 0
		c.send(append(append(append([]byte{TWelcome}, bNonce...), b.ID...), HelloMAC(k, cNonce, bNonce)...))
	case TData:
		if c.c2b == nil {
			return nil
		}
		ctr, plain, err := c.c2b.Open(StreamC, f)
		if err != nil || ctr <= c.ctrC || len(plain) < 2 || (plain[0] != 'c' && plain[0] != 'u') {
			return nil // forged or replayed: dropped
		}
		if ctr != c.ctrC+1 {
			go c.Close() // a gap on C ends the session
			return nil
		}
		c.ctrC, c.authed = ctr, true
		if plain[0] == 'u' {
			if b.Upload != nil && len(plain) >= 5 {
				b.Upload(uint32(plain[1])|uint32(plain[2])<<8|uint32(plain[3])<<16|uint32(plain[4])<<24, plain[5:])
			}
			return nil
		}
		id, cmd, _ := strings.Cut(string(plain[1:]), " ")
		b.Commands = append(b.Commands, cmd)
		if reply := b.Handle(cmd); reply != "" {
			c.sealB('r', id+" "+reply)
		}
	}
	return nil
}

func (c *fakeConn) sealB(kind byte, s string) {
	c.ctrB++
	c.send(c.b2c.Seal(StreamB, TData, c.ctrB, append([]byte{kind}, s...)))
}

// SetPairErr changes PairErr while the board is in use.
func (b *FakeBoard) SetPairErr(msg string) {
	b.mu.Lock()
	b.PairErr = msg
	b.mu.Unlock()
}

// Status sends a status JSON to an authenticated client.
func (b *FakeBoard) Status(json string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if c := b.conn; c != nil && c.authed {
		c.sealB('s', json)
	}
}

// Reply sends a reply line (e.g. a late snap answer) to an authenticated client.
func (b *FakeBoard) Reply(line string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if c := b.conn; c != nil && c.authed {
		c.sealB('r', line)
	}
}

// Video sends one video chunk [seq][flags][payload] on stream V.
func (b *FakeBoard) Video(seq uint16, flags byte, payload []byte) {
	b.mu.Lock()
	defer b.mu.Unlock()
	c := b.conn
	if c == nil || !c.authed {
		return
	}
	chunk := make([]byte, 3, 3+len(payload))
	binary.LittleEndian.PutUint16(chunk, seq)
	chunk[2] = flags
	c.ctrV++
	c.send(c.b2c.Seal(StreamV, TVideo, c.ctrV, append(chunk, payload...)))
}

// Audio sends one phone audio record (stream A) to the authenticated client.
func (b *FakeBoard) Audio(rec []byte) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if c := b.conn; c != nil && c.authed {
		c.ctrA++
		c.send(c.b2c.Seal(StreamA, TAudio, c.ctrA, rec))
	}
}

// Inject delivers a raw frame to the client (replays, forgeries).
func (b *FakeBoard) Inject(f []byte) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.conn != nil {
		b.conn.send(f)
	}
}

// SentFrames returns the frames sent on the current connection.
func (b *FakeBoard) SentFrames() [][]byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.conn == nil {
		return nil
	}
	return append([][]byte{}, b.conn.Sent...)
}

// Drop ends the current connection (the board restarts, say).
func (b *FakeBoard) Drop() {
	b.mu.Lock()
	c := b.conn
	b.mu.Unlock()
	if c != nil {
		c.Close()
	}
}

// Log sends log text ('l', `logs follow on`) to an authenticated client.
func (b *FakeBoard) Log(text string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if c := b.conn; c != nil && c.authed {
		c.sealB('l', text)
	}
}

// SkipB makes the next control frame jump its counter (a lost frame).
func (b *FakeBoard) SkipB() {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.conn != nil {
		b.conn.ctrB++
	}
}
