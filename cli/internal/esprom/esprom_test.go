package esprom

import (
	"bytes"
	"crypto/rand"
	"errors"
	"testing"
)

func TestSlipRoundTrip(t *testing.T) {
	f := NewFakeROM()
	in := []byte{0x01, slipEnd, 0x02, slipEsc, slipEsc, slipEnd, 0x03}
	enc := slipEncode(in)
	if bytes.Count(enc, []byte{slipEnd}) != 2 {
		t.Fatalf("END inside the frame: % x", enc)
	}
	// boot text, an empty frame, then the frame
	f.out = append(append([]byte("garbage\r\n"), slipEnd, slipEnd), enc[1:]...)
	r := slipReader{p: f}
	got, err := r.next(defaultTimeout)
	if err != nil || !bytes.Equal(got, in) {
		t.Fatalf("got % x, %v; want % x", got, err, in)
	}
}

func connect(t *testing.T, f *FakeROM) *Loader {
	t.Helper()
	l := New(f)
	if err := l.Sync(5); err != nil {
		t.Fatal(err)
	}
	return l
}

func TestChipInfo(t *testing.T) {
	f := NewFakeROM()
	l := connect(t, f)
	c, err := l.ReadChipInfo()
	if err != nil {
		t.Fatal(err)
	}
	if c.ChipID != ChipESP32S3 || c.Name != "ESP32-S3" {
		t.Errorf("chip %d %q", c.ChipID, c.Name)
	}
	if c.MAC.String() != "24:ec:4a:01:02:03" {
		t.Errorf("mac %s", c.MAC)
	}
	if c.PSRAM != "8 MB" || c.EmbeddedFlash != "" || c.Revision != "v0.2" || !c.USBJTAG {
		t.Errorf("psram %q flash %q rev %q jtag %v", c.PSRAM, c.EmbeddedFlash, c.Revision, c.USBJTAG)
	}
	if c.FlashSize != 8<<20 || c.FlashID != 0x1740C8 {
		t.Errorf("flash id %06x size %d", c.FlashID, c.FlashSize)
	}
	// the SPI registers the id read touched are put back
	if f.Reg(spiUsr2Reg) != 0 || f.Reg(spiUsrReg) != 0 {
		t.Errorf("SPI registers not restored")
	}
}

func TestChipInfoOtherChips(t *testing.T) {
	f := NewFakeROM()
	f.ChipID = 5
	c, err := connect(t, f).ReadChipInfo()
	if err != nil || c.Name != "ESP32-C3" {
		t.Fatalf("%v %v", c, err)
	}
	f = NewFakeROM()
	f.OldROM, f.Magic = true, 0x00F01D83
	c, err = connect(t, f).ReadChipInfo()
	if err != nil || c.Name != "ESP32" {
		t.Fatalf("old ROM: %v %v", c, err)
	}
	f = NewFakeROM()
	f.SetPSRAMCap(0)
	f.Flash = bytes.Repeat([]byte{0xFF}, 4<<20)
	c, err = connect(t, f).ReadChipInfo()
	if err != nil || c.PSRAM != "" || c.FlashSize != 4<<20 {
		t.Fatalf("no psram, 4 MB: %+v %v", c, err)
	}
}

func TestWriteVerify(t *testing.T) {
	f := NewFakeROM()
	l := connect(t, f)
	if err := l.SPIAttach(); err != nil {
		t.Fatal(err)
	}
	// an image with incompressible parts and a long run of 0xFF in the middle
	data := make([]byte, 300_000)
	rand.Read(data[:100_000])
	for i := 100_000; i < 250_000; i++ {
		data[i] = 0xFF
	}
	rand.Read(data[250_000:])
	data = data[:len(data)-3] // not a multiple of 4
	copy(f.Flash[0x10000:], bytes.Repeat([]byte{0x5A}, 400_000))
	var last, total int
	if err := l.WriteCompressed(0x10000, data, func(d, t int) { last, total = d, t }); err != nil {
		t.Fatal(err)
	}
	if last != total || total != len(data)+3 {
		t.Errorf("progress %d/%d", last, total)
	}
	if !bytes.Equal(f.Flash[0x10000:0x10000+len(data)], data) {
		t.Fatal("flash differs")
	}
	// the rest of the last sector is erased, after it the old content stays
	end := 0x10000 + len(data)
	if f.Flash[end+3] != 0xFF || f.Flash[(end+0xFFF)&^0xFFF] != 0x5A {
		t.Errorf("erase range wrong")
	}
	if err := l.Verify(0x10000, data); err != nil {
		t.Fatal(err)
	}
	if err := l.Finish(); err != nil {
		t.Fatal(err)
	}
	if !f.Download {
		t.Error("Finish left the bootloader")
	}
}

func TestVerifyCatchesCorruption(t *testing.T) {
	f := NewFakeROM()
	f.CorruptWrite = true
	l := connect(t, f)
	data := bytes.Repeat([]byte("tentacle"), 5000)
	if err := l.WriteCompressed(0x1000, data, nil); err != nil {
		t.Fatal(err)
	}
	var ve *VerifyError
	if err := l.Verify(0x1000, data); !errors.As(err, &ve) {
		t.Fatalf("got %v, want a VerifyError", err)
	}
}

func TestChecksumAndStatus(t *testing.T) {
	f := NewFakeROM()
	l := connect(t, f)
	if _, _, err := l.check("begin", cmdFlashDeflBegin, le32(0x1000, 1, FlashBlock, 0, 0), 0, defaultTimeout); err != nil {
		t.Fatal(err)
	}
	blk := []byte{1, 2, 3}
	_, _, err := l.check("data", cmdFlashDeflData, append(le32(3, 0, 0, 0), blk...), checksum(blk)^1, defaultTimeout)
	var re *ROMError
	if !errors.As(err, &re) || re.Code != 0x07 {
		t.Fatalf("bad checksum: %v", err)
	}
	if err := l.EraseRange(0, 16<<20); err == nil {
		t.Fatal("erase beyond the flash went through")
	}
}

func TestResetSequences(t *testing.T) {
	f := NewFakeROM()
	f.Download = false
	EnterDownloadMode(f)
	if !f.Download || f.Boots != 1 {
		t.Fatalf("not in download mode after the USB-JTAG reset (boots %d)", f.Boots)
	}
	l := connect(t, f)
	// a firmware restart into the bootloader sets the force-download bit: the reset after flashing must clear it
	f.SetReg(rtcOption1Reg, forceDownloadBoot)
	HardReset(f)
	if !f.Download {
		t.Fatal("force-download bit ignored by the fake")
	}
	l = connect(t, f)
	if err := l.ClearForceDownload(); err != nil {
		t.Fatal(err)
	}
	HardReset(f)
	if f.Download || f.AppBoots != 1 {
		t.Fatalf("hard reset did not start the firmware")
	}
}

func TestWatchdogs(t *testing.T) {
	f := NewFakeROM()
	f.SetReg(rtcWdtConfig0Reg, 0xABCD)
	if err := connect(t, f).DisableWatchdogs(); err != nil {
		t.Fatal(err)
	}
	if f.Reg(rtcWdtConfig0Reg) != 0 || f.Reg(rtcSwdConfReg)&rtcSwdAutoFeed == 0 || f.Reg(rtcWdtProtectReg) != 0 {
		t.Fatal("watchdogs not disabled")
	}
}

func TestSyncFailsWithoutBootloader(t *testing.T) {
	f := NewFakeROM()
	f.Download = false
	if err := New(f).Sync(2); err == nil {
		t.Fatal("sync with a running firmware")
	}
}

func TestInflatedEnds(t *testing.T) {
	f := NewFakeROM()
	l := connect(t, f)
	// 2 MB of 0xFF compress into a few blocks: the timeouts must cover the inflated size
	data := bytes.Repeat([]byte{0xFF}, 2<<20)
	if err := l.WriteCompressed(0, data, nil); err != nil {
		t.Fatal(err)
	}
}
