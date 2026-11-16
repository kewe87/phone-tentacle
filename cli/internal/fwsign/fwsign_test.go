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
	if img, s, err := Load(p); err != nil || string(img) != string(image) || string(s) != string(sig) {
		t.Fatal("load with a legacy .sig")
	}
}

func TestEmbedSplitAndSignFile(t *testing.T) {
	_, priv, _ := ed25519.GenerateKey(nil)
	pub := priv.Public().(ed25519.PublicKey)
	image := append([]byte{0xE9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0}, []byte("....tentacle-fw:0.4.1;....")...)
	if img, sig, ok := Split(image); ok || sig != nil || string(img) != string(image) {
		t.Fatal("an unsigned image split")
	}
	if _, _, ok := Split([]byte("x")); ok {
		t.Fatal("short data")
	}
	sig := Sign(priv, image)
	file := Embed(image, sig)
	if len(file) != len(image)+TrailerSize || string(file[len(file)-8:]) != "TNTSIG1\n" {
		t.Fatalf("layout: %d", len(file))
	}
	img, s, ok := Split(file)
	if !ok || string(img) != string(image) || string(s) != string(sig) || !Verify(pub, img, s) {
		t.Fatal("split")
	}
	// the signature covers the image only, as the board checks it
	sum := sha256.Sum256(image)
	if !ed25519.Verify(pub, append([]byte("tentacle-fw-v1"), sum[:]...), s) {
		t.Fatal("message")
	}
	// embedding again replaces the trailer
	sig2 := Sign(priv, append([]byte{}, image...)) // ed25519 is deterministic: same bytes
	again := Embed(file, sig2)
	if len(again) != len(file) {
		t.Fatal("re-embedding stacked a second trailer")
	}

	dir := t.TempDir()
	p := filepath.Join(dir, "fw.bin")
	os.WriteFile(p, image, 0o644)
	os.WriteFile(p+SigExt, make([]byte, 64), 0o644) // a stale legacy .sig: the embedded one wins
	for i := 0; i < 2; i++ {                        // signing a signed file replaces its signature
		if got, err := SignFile(priv, p); err != nil || string(got) != string(image) {
			t.Fatalf("sign file: %v", err)
		}
		data, _ := os.ReadFile(p)
		if len(data) != len(image)+TrailerSize {
			t.Fatalf("round %d: %d bytes", i, len(data))
		}
	}
	img, s, err := Load(p)
	if err != nil || string(img) != string(image) || !Verify(pub, img, s) {
		t.Fatalf("load: %v", err)
	}
	if entries, _ := os.ReadDir(dir); len(entries) != 2 {
		t.Fatalf("temporary files left: %v", entries)
	}
	os.WriteFile(p, []byte("not an image"), 0o644)
	if _, err := SignFile(priv, p); err == nil {
		t.Fatal("signed something that is no image")
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
