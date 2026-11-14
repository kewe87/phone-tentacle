// Package proto is the CLI side of docs/cli-protocol.md: pairing, session keys and DATA frames.
package proto

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hkdf"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
)

// Frame types.
const (
	TPair    = 0x01
	TPaired  = 0x81
	THello   = 0x02
	TWelcome = 0x82
	TData    = 0x10
	TVideo   = 0x11
	TAudio   = 0x12 // phone audio records, Wi-Fi only (docs/cli-protocol.md, "Phone audio")
	TError   = 0xE0
	// Pairing over Wi-Fi with the BOOT button (docs/cli-protocol.md, "Pairing").
	TPairNonce = 0x03 // → n_c[16], after PAIR_KEY
	TPairKey   = 0x83 // ← b_pub[32]
	TPairWait  = 0x84 // ← seconds u8: the board shows the code and waits for BOOT
)

// Streams, the first nonce byte of a DATA frame.
const (
	StreamC = 'C' // client → board
	StreamB = 'B' // board control
	StreamV = 'V' // board video
	StreamA = 'A' // board audio
)

const (
	tagSize   = 12
	hdrSize   = 5
	keySize   = 32
	idSize    = 16
	nonceLen  = 16
	boardLen  = 6
	macLen    = 16
	sha256Len = 32
)

var errAuth = errors.New("frame does not authenticate")

func derive(ikm, salt, info []byte) []byte {
	k, err := hkdf.Key(sha256.New, ikm, salt, string(info), keySize)
	if err != nil {
		panic(err) // only for impossible lengths
	}
	return k
}

func mac(k []byte, parts ...[]byte) []byte {
	h := hmac.New(sha256.New, k)
	for _, p := range parts {
		h.Write(p)
	}
	return h.Sum(nil)[:macLen]
}

// PairKey is K = HKDF(shared, "esp32agent-pair-v1", c_pub || b_pub).
func PairKey(shared, cPub, bPub []byte) []byte {
	return derive(shared, []byte("esp32agent-pair-v1"), append(append([]byte{}, cPub...), bPub...))
}

// PairCommit is what PAIR carries instead of the nonce itself: SHA-256(n_c). The computer shows n_c only after the
// board's key, so neither side can pick its key to steer the code.
func PairCommit(nonce []byte) []byte {
	h := sha256.Sum256(nonce)
	return h[:]
}

// PairCode is the 6 digits both sides show: HMAC-SHA256(K, "code" || n_c)[0:4], big-endian, mod 1 000 000.
func PairCode(k, nonce []byte) string {
	m := mac(k, []byte("code"), nonce)
	return fmt.Sprintf("%06d", binary.BigEndian.Uint32(m[:4])%1000000)
}

// PairNameMax is the longest computer name a PAIR frame carries (bytes).
const PairNameMax = 32

// PairFrame builds PAIR: c_pub[32] client_id[16] commit[32] name[0..32]. The name is cut at a rune boundary.
func PairFrame(cPub, clientID, commit []byte, name string) []byte {
	for len(name) > PairNameMax {
		name = name[:len(name)-1]
		for len(name) > 0 && name[len(name)-1]&0xC0 == 0x80 { // inside a multi-byte rune
			name = name[:len(name)-1]
		}
		if len(name) > 0 && name[len(name)-1]&0xC0 == 0xC0 { // its lead byte
			name = name[:len(name)-1]
		}
	}
	f := append([]byte{TPair}, cPub...)
	f = append(f, clientID...)
	f = append(f, commit...)
	return append(f, name...)
}

// PairMAC is the board's proof in PAIRED.
func PairMAC(k, clientID []byte) []byte { return mac(k, []byte("paired"), clientID) }

// HelloMAC is the board's proof in WELCOME.
func HelloMAC(k, cNonce, bNonce []byte) []byte { return mac(k, []byte("hello"), cNonce, bNonce) }

// SessionKeys returns the AES-128 keys client→board and board→client.
func SessionKeys(k, cNonce, bNonce []byte) (c2b, b2c []byte) {
	okm := derive(k, append(append([]byte{}, cNonce...), bNonce...), []byte("esp32agent-session-v1"))
	return okm[:16], okm[16:]
}

// AEAD is AES-128-GCM with a 12-byte tag.
type AEAD struct{ g cipher.AEAD }

func NewAEAD(key []byte) (*AEAD, error) {
	b, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	g, err := cipher.NewGCMWithTagSize(b, tagSize)
	if err != nil {
		return nil, err
	}
	return &AEAD{g}, nil
}

func nonce(stream byte, ctr uint32) []byte {
	n := make([]byte, 12)
	n[0] = stream
	binary.LittleEndian.PutUint32(n[8:], ctr)
	return n
}

// Seal builds a DATA frame: [type][ctr u32 LE][ciphertext][tag 12].
func (a *AEAD) Seal(stream, typ byte, ctr uint32, plain []byte) []byte {
	hdr := make([]byte, hdrSize, hdrSize+len(plain)+tagSize)
	hdr[0] = typ
	binary.LittleEndian.PutUint32(hdr[1:], ctr)
	return a.g.Seal(hdr, nonce(stream, ctr), plain, hdr[:hdrSize])
}

// Open checks and decrypts a DATA frame; it returns the counter and plaintext.
func (a *AEAD) Open(stream byte, frame []byte) (uint32, []byte, error) {
	if len(frame) < hdrSize+tagSize {
		return 0, nil, errAuth
	}
	ctr := binary.LittleEndian.Uint32(frame[1:hdrSize])
	plain, err := a.g.Open(nil, nonce(stream, ctr), frame[hdrSize:], frame[:hdrSize])
	if err != nil {
		return 0, nil, errAuth
	}
	return ctr, plain, nil
}
