package flash

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"testing"
	"testing/fstest"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
)

// testImage builds a merged image like arduino-cli's (default_8MB partitions, padded to 8 MB).
func testImage(t testing.TB, chip uint16) []byte {
	t.Helper()
	img := bytes.Repeat([]byte{0xFF}, 8<<20)
	boot := make([]byte, 20000)
	rand.Read(boot)
	copy(boot, []byte{0xE9, 3, 2, 0x3F})
	binary.LittleEndian.PutUint16(boot[12:], chip)
	copy(img, boot)
	pt := img[0x8000:]
	parts := []struct {
		typ, sub  byte
		off, size uint32
		label     string
	}{
		{1, 2, 0x9000, 0x5000, "nvs"}, {1, 0, 0xe000, 0x2000, "otadata"}, {0, 0x10, 0x10000, 0x330000, "app0"},
		{0, 0x11, 0x340000, 0x330000, "app1"}, {1, 0x82, 0x670000, 0x180000, "spiffs"},
		{1, 3, 0x7F0000, 0x10000, "coredump"},
	}
	for i, p := range parts {
		e := pt[32*i : 32*i+32]
		copy(e, []byte{0xAA, 0x50, p.typ, p.sub})
		binary.LittleEndian.PutUint32(e[4:], p.off)
		binary.LittleEndian.PutUint32(e[8:], p.size)
		copy(e[12:28], make([]byte, 16))
		copy(e[12:], p.label)
		copy(e[28:], []byte{0, 0, 0, 0})
	}
	copy(pt[32*len(parts):], []byte{0xEB, 0xEB})
	copy(img[0xe000:], bootApp0())
	app := make([]byte, 300_000)
	rand.Read(app)
	copy(app, []byte{0xE9, 6, 2, 0x3F})
	binary.LittleEndian.PutUint16(app[12:], chip)
	copy(app[1000:], "tentacle-fw:0.4.0;")
	copy(img[0x10000:], app)
	return img
}

func TestParseMerged(t *testing.T) {
	im, err := ParseImage("hid_bridge.ino.merged.bin", testImage(t, 9), nil)
	if err != nil {
		t.Fatal(err)
	}
	if im.Kind != "merged" || im.Version != "0.4.0" || im.Signed || im.FlashNeed != 8<<20 {
		t.Fatalf("%+v", im)
	}
	want := []struct {
		off  uint32
		size int
	}{{0, 0x5000}, {0x8000, 0x1000}, {0xe000, 0x2000}, {0x10000, 0x4a000}}
	if len(im.Regions) != len(want) {
		t.Fatalf("regions %+v", im.Regions)
	}
	for i, w := range want {
		r := im.Regions[i]
		if r.Offset != w.off || len(r.Data) != w.size {
			t.Errorf("region %s: 0x%x+0x%x, want 0x%x+0x%x", r.Name, r.Offset, len(r.Data), w.off, w.size)
		}
	}
	if len(im.Notes) != 1 || im.Notes[0].Code != "unsigned" {
		t.Errorf("notes %v", im.Notes)
	}
}

func TestParseOtherImages(t *testing.T) {
	code := func(err error) string {
		var m *Msg
		if errors.As(err, &m) {
			return m.Code
		}
		return fmt.Sprint(err)
	}
	if _, err := ParseImage("c3.bin", testImage(t, 5), nil); code(err) != "bad_image" || !strings.Contains(err.Error(), "ESP32-C3") {
		t.Errorf("C3 image: %v", err)
	}
	if _, err := ParseImage("x.elf", []byte("\x7fELF..."), nil); code(err) != "bad_image" {
		t.Errorf("elf: %v", err)
	}
	merged := testImage(t, 9)
	app := merged[0x10000:0x60000]
	im, err := ParseImage("hid_bridge.ino.bin", app, nil)
	if err != nil || im.Kind != "app" || len(im.Regions) != 2 || im.Regions[1].Offset != 0x10000 || im.Regions[0].Offset != 0xe000 {
		t.Fatalf("app only: %+v %v", im, err)
	}
	if _, err := ParseImage("hid_bridge.ino.bootloader.bin", merged[:20000], nil); code(err) != "bad_image" {
		t.Errorf("bootloader: %v", err)
	}
	// signatures: one that matches the release key, one that does not
	pub, priv, _ := ed25519.GenerateKey(nil)
	old := releaseKey
	releaseKey = pub
	defer func() { releaseKey = old }()
	im, err = ParseImage("m.bin", merged, fwsign.Sign(priv, merged))
	if err != nil || !im.Signed || len(im.Notes) != 0 {
		t.Fatalf("signed: %v %v", im, err)
	}
	bad := append([]byte(nil), merged...)
	bad[0x20000] ^= 1
	if _, err := ParseImage("m.bin", bad, fwsign.Sign(priv, merged)); code(err) != "bad_sig" {
		t.Errorf("changed image passed: %v", err)
	}
}

func TestEmbedded(t *testing.T) {
	if _, _, _, err := embeddedFrom(fstest.MapFS{"release/README.txt": {}}, "release"); err != ErrNoImage {
		t.Fatalf("empty: %v", err)
	}
	fs := fstest.MapFS{"release/README.txt": {}, "release/tentacle-0.4.0.merged.bin": {Data: []byte("img")},
		"release/tentacle-0.4.0.merged.bin.sig": {Data: []byte("sig")}}
	name, data, sig, err := embeddedFrom(fs, "release")
	if err != nil || name != "tentacle-0.4.0.merged.bin" || string(data) != "img" || string(sig) != "sig" {
		t.Fatalf("%q %q %q %v", name, data, sig, err)
	}
	var gz bytes.Buffer
	zw := gzip.NewWriter(&gz)
	zw.Write([]byte("packed"))
	zw.Close()
	fs["release/tentacle-0.5.0.merged.bin.gz"] = &fstest.MapFile{Data: gz.Bytes()}
	fs["release/tentacle-0.5.0.merged.bin.sig"] = &fstest.MapFile{Data: []byte("sig5")}
	name, data, sig, err = embeddedFrom(fs, "release")
	if err != nil || name != "tentacle-0.5.0.merged.bin" || string(data) != "packed" || string(sig) != "sig5" {
		t.Fatalf("gzip: %q %q %q %v", name, data, sig, err)
	}
	if _, err := Open(context.Background(), Embedded{}); err != ErrNoImage {
		t.Logf("this build has an image built in: %v", err)
	}
}

const setupJSON = `{"setup":1,"ssid":"Tentacle-1A2B","pass":"abcdefghijkl","url":"http://192.168.4.1/","qr":"` +
	"fe03f8,82020" + `"}`

func TestSetupLine(t *testing.T) {
	s, ok := ParseSetupLine("wifi-setup " + setupJSON + "\r")
	if !ok || !s.Setup || s.SSID != "Tentacle-1A2B" || s.Pass != "abcdefghijkl" || len(s.QR) != 2 {
		t.Fatalf("%+v %v", s, ok)
	}
	if _, ok := ParseSetupLine("ok 0"); ok {
		t.Fatal("other line")
	}
	rows := make([]string, 21)
	for i := range rows {
		rows[i] = "800008"
	}
	m := QRModules(rows)
	if m == nil || !m[0][0] || m[0][1] || !m[0][20] || m[0][19] {
		t.Fatalf("qr modules: %v", m[0])
	}
	if QRModules([]string{"zz"}) != nil {
		t.Fatal("bad qr accepted")
	}
}

func newFlasher(sys System) (*Flasher, *[]Stage) {
	var stages []Stage
	f := &Flasher{Sys: sys, Wait: 1500 * time.Millisecond, Progress: func(s Stage, _, _ int) {
		if len(stages) == 0 || stages[len(stages)-1] != s {
			stages = append(stages, s)
		}
	}}
	return f, &stages
}

func codeOf(err error) string {
	var m *Msg
	if errors.As(err, &m) {
		return m.Code
	}
	return fmt.Sprint(err)
}

func TestInstallFromRunningFirmware(t *testing.T) {
	rom := esprom.NewFakeROM()
	rom.Download = false
	copy(rom.Flash[0x9000:], "stored wifi and pairing")
	copy(rom.Flash[0x340000:], "old app in app1")
	sys := NewFakeSystem(rom, "wifi-setup "+setupJSON)
	raw := testImage(t, 9)
	im, err := ParseImage("t.merged.bin", raw, nil)
	if err != nil {
		t.Fatal(err)
	}
	f, stages := newFlasher(sys)
	res, err := f.Install(context.Background(), Options{Image: im, SetupWait: 3 * time.Second})
	if err != nil {
		t.Fatal(err)
	}
	if sys.Touches != 1 || res.Port != "COM5" {
		t.Errorf("touches %d, port %s", sys.Touches, res.Port)
	}
	for _, r := range im.Regions {
		if !bytes.Equal(rom.Flash[r.Offset:int(r.Offset)+len(r.Data)], raw[r.Offset:int(r.Offset)+len(r.Data)]) {
			t.Errorf("region %s differs", r.Name)
		}
	}
	// what the image leaves empty stays: nvs (Wi-Fi, pairing) and the other app slot
	if !bytes.HasPrefix(rom.Flash[0x9000:], []byte("stored wifi and pairing")) || !bytes.HasPrefix(rom.Flash[0x340000:], []byte("old app in app1")) {
		t.Error("nvs or app1 touched")
	}
	if rom.Download || rom.AppBoots < 1 || rom.Reg(0x6000812C)&1 != 0 {
		t.Errorf("not restarted into the firmware (download %v, force bit %x)", rom.Download, rom.Reg(0x6000812C))
	}
	if res.Setup == nil || res.Setup.SSID != "Tentacle-1A2B" || res.Chip.Name != "ESP32-S3" {
		t.Fatalf("setup %+v", res.Setup)
	}
	got := fmt.Sprint(*stages)
	if got != "[connect check write verify restart setup]" {
		t.Errorf("stages %s", got)
	}
}

func TestInstallErase(t *testing.T) {
	rom := esprom.NewFakeROM()
	copy(rom.Flash[0x9000:], "stored wifi")
	sys := NewFakeSystem(rom, "wifi-setup "+setupJSON)
	im, _ := ParseImage("t.merged.bin", testImage(t, 9), nil)
	f, stages := newFlasher(sys)
	if _, err := f.Install(context.Background(), Options{Image: im, Erase: true, SetupWait: -1}); err != nil {
		t.Fatal(err)
	}
	if rom.Flash[0x9000] != 0xFF {
		t.Error("nvs not erased")
	}
	if fmt.Sprint(*stages) != "[connect check erase write verify restart]" {
		t.Errorf("stages %v", *stages)
	}
}

func TestInstallUnknownPortSameName(t *testing.T) {
	// macOS: no USB ids, the board keeps its name across the restart
	rom := esprom.NewFakeROM()
	rom.Download = false
	sys := NewFakeSystem(rom, "wifi-setup "+setupJSON)
	sys.Product, sys.ROMPort, sys.FWPort = false, "/dev/cu.usbmodem1101", "/dev/cu.usbmodem1101"
	im, _ := ParseImage("t.merged.bin", testImage(t, 9), nil)
	f, _ := newFlasher(sys)
	res, err := f.Install(context.Background(), Options{Image: im, SetupWait: 3 * time.Second})
	if err != nil {
		t.Fatal(err)
	}
	if sys.Touches != 1 || res.Setup == nil {
		t.Fatalf("touches %d setup %v", sys.Touches, res.Setup)
	}
}

func TestInstallBlankBoardOnJTAG(t *testing.T) {
	// a blank board (or one stuck in its bootloader) is the USB-Serial/JTAG: reset into download mode, no touch
	rom := esprom.NewFakeROM()
	rom.Download = false
	sys := NewFakeSystem(rom, "wifi-setup "+setupJSON)
	sys.FWPort, sys.Blank = sys.ROMPort, true // a blank board has no firmware port
	sys.Product = false
	rom.OnBoot = nil
	im, _ := ParseImage("t.merged.bin", testImage(t, 9), nil)
	f, _ := newFlasher(sys)
	b, err := f.Connect(context.Background(), "")
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()
	if sys.Touches != 0 || !rom.Download {
		t.Fatalf("touches %d", sys.Touches)
	}
	if err := b.Check(im); err != nil {
		t.Fatal(err)
	}
}

func TestRefusals(t *testing.T) {
	im, _ := ParseImage("t.merged.bin", testImage(t, 9), nil)
	run := func(rom *esprom.FakeROM, sys *FakeSystem) (*Result, error) {
		f, _ := newFlasher(sys)
		return f.Install(context.Background(), Options{Image: im, SetupWait: -1})
	}
	rom := esprom.NewFakeROM()
	rom.ChipID = 5
	if _, err := run(rom, NewFakeSystem(rom, "wifi-setup "+setupJSON)); codeOf(err) != "wrong_chip" || !strings.Contains(err.Error(), "ESP32-C3") {
		t.Errorf("C3: %v", err)
	}
	if rom.Ops[len(rom.Ops)-1] == 0x11 {
		t.Error("wrote to a C3")
	}
	rom = esprom.NewFakeROM()
	rom.Flash = bytes.Repeat([]byte{0xFF}, 4<<20)
	if _, err := run(rom, NewFakeSystem(rom, "wifi-setup "+setupJSON)); codeOf(err) != "flash_small" || !strings.Contains(err.Error(), "4 MB") {
		t.Errorf("4 MB: %v", err)
	}
	rom = esprom.NewFakeROM()
	rom.SecureBoot = true
	if _, err := run(rom, NewFakeSystem(rom, "wifi-setup "+setupJSON)); codeOf(err) != "secure" {
		t.Errorf("secure boot: %v", err)
	}
	rom = esprom.NewFakeROM()
	rom.SetPSRAMCap(0)
	res, err := run(rom, NewFakeSystem(rom, "wifi-setup "+setupJSON))
	if err != nil || len(res.Notes) != 1 || res.Notes[0].Code != "no_psram" {
		t.Errorf("no PSRAM is a warning: %v %v", res, err)
	}
	rom = esprom.NewFakeROM()
	rom.CorruptWrite = true
	if _, err := run(rom, NewFakeSystem(rom, "wifi-setup "+setupJSON)); codeOf(err) != "verify" {
		t.Errorf("corrupt write: %v", err)
	}
}

func TestPick(t *testing.T) {
	sys := &FakeSystem{ROM: esprom.NewFakeROM(), Gap: 1 << 30}
	f, _ := newFlasher(sys)
	if _, _, err := f.Pick(""); codeOf(err) != "no_board" {
		t.Errorf("empty: %v", err)
	}
	sys.Extra = []PortInfo{{Name: "COM3", VID: "1A86", PID: "7523"}, {Name: "COM1"}}
	if _, _, err := f.Pick(""); codeOf(err) != "uart_board" || !strings.Contains(err.Error(), "CH340") {
		t.Errorf("CH340: %v", err)
	}
	sys.Extra = append(sys.Extra, PortInfo{Name: "COM7", VID: "303a", Product: "Tentacle"}, PortInfo{Name: "COM8", VID: "303A"})
	if _, _, err := f.Pick(""); codeOf(err) != "several" {
		t.Errorf("two boards: %v", err)
	}
	if p, _, err := f.Pick("COM8"); err != nil || p.Name != "COM8" || p.Kind != KindUnknown {
		t.Errorf("chosen port: %+v %v", p, err)
	}
	if _, err := f.Connect(context.Background(), "COM9"); codeOf(err) != "port_open" {
		t.Errorf("missing port: %v", err)
	}
}

func TestMsgLanguages(t *testing.T) {
	for code, t2 := range messages {
		for i, f := range t2 {
			if f == "" || strings.Count(f, "%") != strings.Count(t2[0], "%") {
				t.Errorf("%s[%d]: %q", code, i, f)
			}
		}
	}
	m := msg("wrong_chip", nil, "ESP32")
	if !strings.Contains(m.Text("de"), "Falscher Chip") || !strings.Contains(m.Error(), "Wrong chip") {
		t.Error(m.Text("de"))
	}
}
