package esprom

import (
	"fmt"
	"net"
	"time"
)

// Chip ids from GET_SECURITY_INFO (and the image header's chip id).
const ChipESP32S3 = 9

var chipNames = map[uint32]string{0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3", 12: "ESP32-C2",
	13: "ESP32-C6", 16: "ESP32-H2", 18: "ESP32-P4", 20: "ESP32-C61", 23: "ESP32-C5"}

// ChipName names a chip id.
func ChipName(id uint32) string {
	if n, ok := chipNames[id]; ok {
		return n
	}
	return fmt.Sprintf("unknown chip (id %d)", id)
}

// The value at chipDetectMagicReg tells chips apart on ROMs that have no GET_SECURITY_INFO (ESP32, ESP32-S2 and
// early ESP32-C3).
const chipDetectMagicReg = 0x40001000

var magicChips = map[uint32]uint32{0x00F01D83: 0, 0x000007C6: 2, 0x6921506F: 5, 0x1B31506F: 5, 0x09: 9}

// ESP32-S3 registers (as esptool's ESP32S3ROM uses them).
const (
	efuseBase      = 0x60007000
	efuseBlock1    = efuseBase + 0x44 // MAC words 0/1, then the chip's package, flash and PSRAM data
	spiBase        = 0x60002000
	spiCmdReg      = spiBase + 0x00
	spiUsrReg      = spiBase + 0x18
	spiUsr1Reg     = spiBase + 0x1C
	spiUsr2Reg     = spiBase + 0x20
	spiMisoDlenReg = spiBase + 0x28
	spiW0Reg       = spiBase + 0x58
	spiCmdUsr      = 1 << 18
	spiUsrCommand  = 1 << 31
	spiUsrMiso     = 1 << 28

	rtcCntlBase          = 0x60008000
	rtcOption1Reg        = rtcCntlBase + 0x012C
	forceDownloadBoot    = 0x1
	rtcWdtConfig0Reg     = rtcCntlBase + 0x0098
	rtcWdtProtectReg     = rtcCntlBase + 0x00B0
	rtcWdtKey            = 0x50D83AA1
	rtcSwdConfReg        = rtcCntlBase + 0x00B4
	rtcSwdProtectReg     = rtcCntlBase + 0x00B8
	rtcSwdKey            = 0x8F1D312A
	rtcSwdAutoFeed       = 1 << 31
	uartDevBufNo         = 0x3FCEF14C // ROM .bss: the port the ROM talks through
	uartDevUSBJTAGSerial = 4
)

var psramCaps = map[uint32]string{1: "8 MB", 2: "2 MB", 3: "16 MB", 4: "4 MB"}
var flashCaps = map[uint32]string{1: "8 MB", 2: "4 MB"}

// ChipInfo is what the flasher reads from the chip before it writes anything.
type ChipInfo struct {
	ChipID        uint32
	Name          string
	Revision      string
	MAC           net.HardwareAddr
	PSRAM         string // embedded PSRAM from the eFuses ("8 MB"), "" if the eFuses name none
	EmbeddedFlash string // flash inside the chip package, "" if external
	FlashID       uint32 // JEDEC id: manufacturer, memory type, capacity
	FlashSize     uint32 // from the JEDEC capacity byte; 0 if unknown
	USBJTAG       bool   // the ROM talks over the USB-Serial/JTAG
	SecureBoot    bool
	FlashEncrypt  bool
}

// FlashSizeText is the flash size for people ("8 MB", "unknown").
func (c *ChipInfo) FlashSizeText() string {
	if c.FlashSize == 0 {
		return "unknown"
	}
	return SizeText(c.FlashSize)
}

// SizeText formats a power-of-two size.
func SizeText(n uint32) string {
	if n >= 1<<20 {
		return fmt.Sprintf("%d MB", n>>20)
	}
	return fmt.Sprintf("%d KB", n>>10)
}

// DetectChip finds out which chip answers: GET_SECURITY_INFO first, the magic register as a fallback.
func (l *Loader) DetectChip() (uint32, *SecurityInfo, error) {
	si, err := l.SecurityInfo()
	if err == nil && si.HasChipID {
		return si.ChipID, si, nil
	}
	v, rerr := l.ReadReg(chipDetectMagicReg)
	if rerr != nil {
		if err != nil {
			return 0, nil, err
		}
		return 0, nil, rerr
	}
	id, ok := magicChips[v]
	if !ok {
		return 0, si, fmt.Errorf("unknown chip (magic value 0x%08x)", v)
	}
	return id, si, nil
}

// ReadChipInfo reads the chip id, security state, MAC, PSRAM and the flash's JEDEC id. Everything after the chip id
// is read only on an ESP32-S3 (the register map is the S3's). It attaches the SPI flash on the way.
func (l *Loader) ReadChipInfo() (*ChipInfo, error) {
	id, si, err := l.DetectChip()
	if err != nil {
		return nil, err
	}
	c := &ChipInfo{ChipID: id, Name: ChipName(id)}
	if si != nil {
		c.SecureBoot, c.FlashEncrypt = si.SecureBoot, si.FlashEncrypt
	}
	if id != ChipESP32S3 {
		return c, nil
	}
	w := func(i uint32) uint32 {
		v, err2 := l.ReadReg(efuseBlock1 + 4*i)
		if err2 != nil && err == nil {
			err = err2
		}
		return v
	}
	mac0, mac1, w3, w4, w5 := w(0), w(1), w(3), w(4), w(5)
	if err != nil {
		return nil, err
	}
	c.MAC = net.HardwareAddr{byte(mac1 >> 8), byte(mac1), byte(mac0 >> 24), byte(mac0 >> 16), byte(mac0 >> 8), byte(mac0)}
	c.PSRAM = psramCaps[(w5>>19&1)<<2|(w4>>3&3)]
	c.EmbeddedFlash = flashCaps[w3>>27&7]
	major := w5 >> 24 & 3
	minor := (w5>>23&1)<<3 | (w3 >> 18 & 7)
	c.Revision = fmt.Sprintf("v%d.%d", major, minor)
	if v, err := l.ReadReg(uartDevBufNo); err == nil {
		c.USBJTAG = v&0xFF == uartDevUSBJTAGSerial
	}
	if err := l.SPIAttach(); err != nil {
		return nil, err
	}
	fid, err := l.FlashID()
	if err != nil {
		return nil, err
	}
	c.FlashID = fid
	if sz := fid >> 16 & 0xFF; sz >= 0x12 && sz <= 0x1A && fid != 0xFFFFFF {
		c.FlashSize = 1 << sz
	}
	return c, nil
}

// FlashID reads the SPI flash's JEDEC id (command 0x9F) through the SPI controller's user command registers.
func (l *Loader) FlashID() (uint32, error) {
	oldUsr, err := l.ReadReg(spiUsrReg)
	if err != nil {
		return 0, err
	}
	oldUsr2, err := l.ReadReg(spiUsr2Reg)
	if err != nil {
		return 0, err
	}
	steps := []struct{ addr, val uint32 }{
		{spiMisoDlenReg, 24 - 1},
		{spiUsrReg, spiUsrCommand | spiUsrMiso},
		{spiUsr2Reg, 7<<28 | 0x9F},
		{spiW0Reg, 0},
		{spiCmdReg, spiCmdUsr},
	}
	for _, s := range steps {
		if err := l.WriteReg(s.addr, s.val, 0xFFFFFFFF); err != nil {
			return 0, err
		}
	}
	done := false
	for range 10 {
		v, err := l.ReadReg(spiCmdReg)
		if err != nil {
			return 0, err
		}
		if v&spiCmdUsr == 0 {
			done = true
			break
		}
	}
	if !done {
		return 0, fmt.Errorf("read the flash id: the SPI command did not finish")
	}
	id, err := l.ReadReg(spiW0Reg)
	if err != nil {
		return 0, err
	}
	_ = l.WriteReg(spiUsrReg, oldUsr, 0xFFFFFFFF)
	_ = l.WriteReg(spiUsr2Reg, oldUsr2, 0xFFFFFFFF)
	return id & 0xFFFFFF, nil
}

// DisableWatchdogs stops the RTC watchdog and lets the super watchdog feed itself. Over the USB-Serial/JTAG the
// ROM leaves them running, and they would reset the chip in the middle of a long write.
func (l *Loader) DisableWatchdogs() error {
	steps := []struct{ addr, val uint32 }{
		{rtcWdtProtectReg, rtcWdtKey},
		{rtcWdtConfig0Reg, 0},
		{rtcWdtProtectReg, 0},
		{rtcSwdProtectReg, rtcSwdKey},
	}
	for _, s := range steps {
		if err := l.WriteReg(s.addr, s.val, 0xFFFFFFFF); err != nil {
			return err
		}
	}
	v, err := l.ReadReg(rtcSwdConfReg)
	if err != nil {
		return err
	}
	if err := l.WriteReg(rtcSwdConfReg, v|rtcSwdAutoFeed, 0xFFFFFFFF); err != nil {
		return err
	}
	return l.WriteReg(rtcSwdProtectReg, 0, 0xFFFFFFFF)
}

// ClearForceDownload clears the "force download boot" bit a running firmware sets when it restarts into the ROM
// (the 1200 baud touch). Left set, the chip would come back into download mode after the final reset.
func (l *Loader) ClearForceDownload() error {
	return l.WriteReg(rtcOption1Reg, 0, forceDownloadBoot)
}

// EnterDownloadMode resets the chip into the ROM bootloader through the USB-Serial/JTAG's DTR/RTS lines (esptool's
// USBJTAGSerialReset). On a TinyUSB port of a running Arduino firmware the same sequence makes the firmware
// restart into the bootloader.
func EnterDownloadMode(p Lines) {
	_ = p.SetRTS(false)
	_ = p.SetDTR(false) // idle
	time.Sleep(100 * time.Millisecond)
	_ = p.SetDTR(true) // GPIO0 low
	_ = p.SetRTS(false)
	time.Sleep(100 * time.Millisecond)
	_ = p.SetRTS(true) // reset; this order goes through (1,1) instead of (0,0)
	_ = p.SetDTR(false)
	_ = p.SetRTS(true) // set RTS again: Windows passes DTR on only with an RTS change
	time.Sleep(100 * time.Millisecond)
	_ = p.SetDTR(false)
	_ = p.SetRTS(false) // out of reset
}

// HardReset restarts the chip into its firmware: RTS pulses the reset with GPIO0 (DTR) released.
func HardReset(p Lines) {
	_ = p.SetDTR(false)
	_ = p.SetRTS(true)
	time.Sleep(200 * time.Millisecond)
	_ = p.SetRTS(false)
	time.Sleep(200 * time.Millisecond)
}
