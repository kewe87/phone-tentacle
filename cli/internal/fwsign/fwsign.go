// Package fwsign signs Tentacle firmware images and reads what `tentacle update` needs from them
// (docs/cli-protocol.md, "Firmware update"). The board takes over the network only images signed with the release
// key whose public half is compiled into it (hardware/hid_bridge/ota_pubkey.h); a debug build takes unsigned ones too.
//
//	signed message = "tentacle-fw-v1" || SHA-256(image)
//	signature      = Ed25519(release key, signed message), 64 bytes
//	signed file    = image || signature || "TNTSIG1\n" (8 bytes)
//
// The signature travels inside the file, behind the image: 64 bytes, then the 8-byte trailer Magic. Nothing that
// reads the file needs to know more than Split; the board never sees the trailer, the uploaders (tentacle update,
// the board's web page, tentacle-flash) cut it off and send image and signature apart, as before. Older releases
// had the signature in FILE.bin.sig next to the image; Load still takes that.
package fwsign

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const label = "tentacle-fw-v1"

// SigExt is the legacy signature file next to an image: FILE.bin.sig (read for older files, no longer written).
const SigExt = ".sig"

// Magic ends a signed file: image || 64-byte signature || Magic.
const Magic = "TNTSIG1\n"

// TrailerSize is what signing adds to an image.
const TrailerSize = ed25519.SignatureSize + len(Magic)

// Split cuts a file into the image and the signature embedded behind it. Without the trailer, data is the image
// and signed is false. It does not check the signature.
func Split(data []byte) (image, sig []byte, signed bool) {
	n := len(data) - TrailerSize
	if n < 0 || string(data[len(data)-len(Magic):]) != Magic {
		return data, nil, false
	}
	return data[:n], data[n : n+ed25519.SignatureSize], true
}

// Embed puts a signature behind an image (one already there is replaced).
func Embed(image, sig []byte) []byte {
	image, _, _ = Split(image)
	out := make([]byte, 0, len(image)+TrailerSize)
	out = append(append(append(out, image...), sig...), Magic...)
	return out
}

// SignFile signs an image file in place: the signature goes into the file (an old one is replaced), written to a
// temporary file next to it and renamed over it. It returns the image without the trailer.
func SignFile(key ed25519.PrivateKey, path string) ([]byte, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	image, _, _ := Split(data)
	if err := CheckImage(image); err != nil {
		return nil, err
	}
	mode := os.FileMode(0o644)
	if st, err := os.Stat(path); err == nil {
		mode = st.Mode().Perm()
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return nil, err
	}
	defer os.Remove(tmp.Name())
	_, err = tmp.Write(Embed(image, Sign(key, image)))
	if err2 := tmp.Close(); err == nil {
		err = err2
	}
	if err == nil {
		err = os.Chmod(tmp.Name(), mode)
	}
	if err == nil {
		err = os.Rename(tmp.Name(), path)
	}
	if err != nil {
		return nil, err
	}
	return image, nil
}

// Load reads an image file: the image and its signature, embedded or (older files) in FILE.sig next to it; sig
// is nil when there is neither. It does not check the signature.
func Load(path string) (image, sig []byte, err error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, err
	}
	if image, sig, ok := Split(data); ok {
		return image, sig, nil
	}
	sig, err = ReadSig(path)
	return data, sig, err
}

// Message is what the signature covers.
func Message(sha [32]byte) []byte { return append([]byte(label), sha[:]...) }

// Sign signs an image with a private key.
func Sign(key ed25519.PrivateKey, image []byte) []byte {
	return ed25519.Sign(key, Message(sha256.Sum256(image)))
}

// Verify checks an image's signature against a public key.
func Verify(pub ed25519.PublicKey, image, sig []byte) bool {
	return len(pub) == ed25519.PublicKeySize && len(sig) == ed25519.SignatureSize &&
		ed25519.Verify(pub, Message(sha256.Sum256(image)), sig)
}

// Keygen writes a new key pair into dir: tentacle-release.key (the private seed as hex, mode 0600: keep it out of
// any repository) and tentacle-release.pub (hex). It returns the public key.
func Keygen(dir string) (ed25519.PublicKey, string, error) {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, "", err
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, "", err
	}
	keyPath := filepath.Join(dir, "tentacle-release.key")
	if _, err := os.Stat(keyPath); err == nil {
		return nil, "", fmt.Errorf("%s exists: not overwritten", keyPath)
	}
	if err := os.WriteFile(keyPath, []byte(hex.EncodeToString(priv.Seed())+"\n"), 0o600); err != nil {
		return nil, "", err
	}
	if err := os.WriteFile(filepath.Join(dir, "tentacle-release.pub"), []byte(hex.EncodeToString(pub)+"\n"), 0o644); err != nil {
		return nil, "", err
	}
	return pub, keyPath, nil
}

// LoadKey reads a private key file written by Keygen.
func LoadKey(path string) (ed25519.PrivateKey, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	seed, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil || len(seed) != ed25519.SeedSize {
		return nil, fmt.Errorf("%s is not a tentacle release key (64 hex digits)", path)
	}
	return ed25519.NewKeyFromSeed(seed), nil
}

// ReleaseKey is the public half of the release key, the same 32 bytes as OTA_RELEASE_PUBKEY in
// hardware/hid_bridge/ota_pubkey.h (a test keeps the two equal). tentacle-flash checks images against it before
// it writes them over USB.
var ReleaseKey = ed25519.PublicKey{
	0x8c, 0xe6, 0x21, 0x6b, 0x55, 0x4c, 0x38, 0xf3,
	0x30, 0x13, 0xaa, 0xad, 0x5a, 0x94, 0x4d, 0x59,
	0x94, 0x83, 0xa5, 0xd8, 0x23, 0x01, 0x6d, 0x16,
	0x9f, 0x04, 0xf5, 0x6f, 0xe8, 0xce, 0x84, 0x36}

// CArray is a public key as the C initializer ota_pubkey.h takes.
func CArray(pub []byte) string {
	var b strings.Builder
	for i, c := range pub {
		switch {
		case i > 0 && i%8 == 0:
			b.WriteString(",\n    ")
		case i > 0:
			b.WriteString(", ")
		}
		fmt.Fprintf(&b, "0x%02x", c)
	}
	return b.String()
}

// ReadSig reads a legacy FILE.bin.sig next to an image: nil, nil when there is none.
func ReadSig(image string) ([]byte, error) {
	sig, err := os.ReadFile(image + SigExt)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if len(sig) != ed25519.SignatureSize {
		return nil, fmt.Errorf("%s%s is not a signature (64 bytes)", image, SigExt)
	}
	return sig, nil
}

// Version finds the firmware version a Tentacle image carries ("tentacle-fw:0.2.0;", version.h's FW_MARKER);
// "" if it has none.
func Version(image []byte) string {
	const marker = "tentacle-fw:"
	i := bytes.Index(image, []byte(marker))
	if i < 0 {
		return ""
	}
	rest := image[i+len(marker):]
	j := bytes.IndexByte(rest, ';')
	if j <= 0 || j > 24 {
		return ""
	}
	v := string(rest[:j])
	for _, r := range v {
		if r <= ' ' || r >= 0x7f {
			return ""
		}
	}
	return v
}

// CheckImage says whether data looks like an ESP32 app image (magic byte, ESP32-S3 chip id in its extended header).
func CheckImage(data []byte) error {
	if len(data) < 24 || data[0] != 0xE9 {
		return errors.New("not an ESP32 firmware image (use the .ino.bin, not the merged or bootloader file)")
	}
	if chip := uint16(data[12]) | uint16(data[13])<<8; chip != 9 { // ESP_CHIP_ID_ESP32S3
		return fmt.Errorf("firmware for another chip (id %d), the board is an ESP32-S3", chip)
	}
	return nil
}
