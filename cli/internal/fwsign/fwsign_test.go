package fwsign

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestSignVerifyAndKeyFiles(t *testing.T) {
	dir := t.TempDir()
	pub, keyPath, err := Keygen(dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := Keygen(dir); err == nil {
		t.Fatal("a second keygen overwrote the key")
	}
	key, err := LoadKey(keyPath)
	if err != nil || !key.Public().(ed25519.PublicKey).Equal(pub) {
		t.Fatalf("key file round trip: %v", err)
	}
	image := append([]byte{0xE9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0}, []byte("....tentacle-fw:0.2.0;....")...)
	sig := Sign(key, image)
	if !Verify(pub, image, sig) {
		t.Fatal("good signature refused")
	}
	bad := append([]byte{}, image...)
	bad[20] ^= 1
	if Verify(pub, bad, sig) {
		t.Fatal("a changed image passed")
	}
	other, _, _ := ed25519.GenerateKey(nil)
	if Verify(other, image, sig) {
		t.Fatal("another key passed")
	}
	// the signed message is what the board checks (src/ota/ota_sig.c)
	sum := sha256.Sum256(image)
	if !ed25519.Verify(pub, append([]byte("tentacle-fw-v1"), sum[:]...), sig) {
		t.Fatal("message layout")
	}
	if Version(image) != "0.2.0" || Version([]byte("nothing")) != "" {
		t.Fatal("version marker")
	}
	if CheckImage(image) != nil || CheckImage([]byte("hello world, not an image")) == nil {
		t.Fatal("image check")
	}
	p := filepath.Join(dir, "fw.bin")
	os.WriteFile(p, image, 0o644)
	if s, err := ReadSig(p); s != nil || err != nil {
		t.Fatal("no .sig is not an error")
	}
	os.WriteFile(p+SigExt, sig, 0o644)
	if s, err := ReadSig(p); err != nil || hex.EncodeToString(s) != hex.EncodeToString(sig) {
		t.Fatal("read .sig")
	}
}

func TestReleaseKeyMatchesFirmware(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("..", "..", "..", "hardware", "hid_bridge", "ota_pubkey.h"))
	if err != nil {
		t.Skip("firmware source not next to the CLI:", err)
	}
	s := string(src)
	i := strings.Index(s, "OTA_RELEASE_PUBKEY")
	if i < 0 {
		t.Fatal("no OTA_RELEASE_PUBKEY in ota_pubkey.h")
	}
	s = s[i:]
	s = s[strings.Index(s, "{")+1 : strings.Index(s, "}")]
	var got []byte
	for _, f := range strings.Split(s, ",") {
		b, err := strconv.ParseUint(strings.TrimSpace(f), 0, 8)
		if err != nil {
			t.Fatalf("ota_pubkey.h: %q: %v", f, err)
		}
		got = append(got, byte(b))
	}
	if !ReleaseKey.Equal(ed25519.PublicKey(got)) {
		t.Fatalf("fwsign.ReleaseKey differs from ota_pubkey.h:\n%x\n%x", []byte(ReleaseKey), got)
	}
}
