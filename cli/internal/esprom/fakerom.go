package esprom

import (
	"bytes"
	"compress/zlib"
	"crypto/md5"
	"encoding/binary"
	"encoding/hex"
	"io"
	"sync"
	"time"
)

// FakeROM emulates an ESP32-S3 in its ROM bootloader on the far end of a serial port, for tests without
// hardware: SLIP framing, the commands the flasher uses, checksums, a flash array with erase, inflate and MD5,
// eFuse words and the USB-Serial/JTAG's DTR/RTS reset logic. It implements Port, Lines and io.Closer.
type FakeROM struct {
	mu sync.Mutex

	ChipID       uint32 // reported by GET_SECURITY_INFO
	OldROM       bool   // no GET_SECURITY_INFO: the chip is told by its magic register
	Magic        uint32 // value of the magic register on an OldROM
	SecureBoot   bool
	Flash        []byte // its length is the flash size the JEDEC id reports
	FlashMfr     byte
	CorruptWrite bool // flip one byte of every write (to test the MD5 check)
	Download     bool // in the ROM bootloader (answers commands); false: running the firmware
	// OnBoot, if set, runs (with the lock held) when the chip leaves reset; download says which mode it came up in.
	OnBoot func(download bool)

	regs  map[uint32]uint32
	dec   slipReaderState
	out   []byte
	dtr   bool
	rts   bool
	strap bool
	reset bool
	defl  struct {
		active          bool
		offset, size    uint32
		blocks, nextSeq uint32
		comp            []byte
	}
	// Ops lists every command received, in order.
	Ops []byte
	// Boots counts the chip's restarts, AppBoots those into the firmware.
	Boots, AppBoots int
	closed          bool
	timeout         time.Duration
}

type slipReaderState struct {
	inside, esc bool
	frame       []byte
}

// NewFakeROM makes an ESP32-S3 with 8 MB flash and 8 MB embedded PSRAM, waiting in download mode.
func NewFakeROM() *FakeROM {
	f := &FakeROM{ChipID: ChipESP32S3, Flash: bytes.Repeat([]byte{0xFF}, 8<<20), FlashMfr: 0xC8, Download: true,
		regs: map[uint32]uint32{}, timeout: 10 * time.Millisecond}
	f.SetMAC([6]byte{0x24, 0xEC, 0x4A, 0x01, 0x02, 0x03})
	f.SetPSRAMCap(1)
	f.regs[efuseBlock1+4*3] |= 2 << 18 // revision v0.2
	f.regs[uartDevBufNo] = uartDevUSBJTAGSerial
	return f
}

// SetMAC writes the MAC into the eFuse words.
func (f *FakeROM) SetMAC(m [6]byte) {
	f.regs[efuseBlock1] = uint32(m[2])<<24 | uint32(m[3])<<16 | uint32(m[4])<<8 | uint32(m[5])
	f.regs[efuseBlock1+4] = uint32(m[0])<<8 | uint32(m[1])
}

// SetPSRAMCap writes the eFuse PSRAM capacity field (0 = none, 1 = 8 MB, ...).
func (f *FakeROM) SetPSRAMCap(c uint32) {
	f.regs[efuseBlock1+16] = f.regs[efuseBlock1+16]&^(3<<3) | (c&3)<<3
	f.regs[efuseBlock1+20] = f.regs[efuseBlock1+20]&^(1<<19) | (c>>2&1)<<19
}

// Reg reads one emulated register (for tests).
func (f *FakeROM) Reg(addr uint32) uint32 {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.regs[addr]
}

// SetReg sets one emulated register (for tests).
func (f *FakeROM) SetReg(addr, v uint32) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.regs[addr] = v
}

// SetReadTimeout implements Port.
func (f *FakeROM) SetReadTimeout(t time.Duration) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.timeout = t
	return nil
}

// Read implements Port: what the chip sent, or nothing after a short wait.
func (f *FakeROM) Read(p []byte) (int, error) {
	f.mu.Lock()
	if f.closed {
		f.mu.Unlock()
		return 0, io.ErrClosedPipe
	}
	if len(f.out) == 0 {
		t := min(f.timeout, 5*time.Millisecond)
		f.mu.Unlock()
		time.Sleep(t)
		return 0, nil
	}
	n := copy(p, f.out)
	f.out = f.out[n:]
	f.mu.Unlock()
	return n, nil
}

// ResetInputBuffer drops what the chip sent and nobody read.
func (f *FakeROM) ResetInputBuffer() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.out = nil
	return nil
}

// Close implements io.Closer.
func (f *FakeROM) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	return nil
}

// Reopen makes a closed fake usable again (the same chip on a newly opened port).
func (f *FakeROM) Reopen() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed, f.out = false, nil
}

// SetDTR implements Lines.
func (f *FakeROM) SetDTR(v bool) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.dtr = v
	f.lines()
	return nil
}

// SetRTS implements Lines.
func (f *FakeROM) SetRTS(v bool) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.rts = v
	f.lines()
	return nil
}

// lines applies the USB-Serial/JTAG's reset logic: RTS without DTR holds the chip in reset, DTR without RTS pulls
// GPIO0 low; the chip samples it (or the force-download bit) when it leaves reset.
func (f *FakeROM) lines() {
	if f.dtr && !f.rts {
		f.strap = true
	}
	if f.rts && !f.dtr {
		f.reset = true
		return
	}
	if f.reset {
		f.reset = false
		f.boot(f.strap || f.regs[rtcOption1Reg]&forceDownloadBoot != 0)
		f.strap = false
	}
}

// RestartToBootloader is what a running Arduino firmware does on a 1200 baud touch: set the force-download bit
// and restart, so the ROM comes up in download mode.
func (f *FakeROM) RestartToBootloader() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.regs[rtcOption1Reg] |= forceDownloadBoot
	f.boot(true)
}

func (f *FakeROM) boot(download bool) {
	f.Boots++
	f.Download = download
	f.defl.active = false
	f.dec = slipReaderState{}
	if download {
		f.out = append(f.out, "ESP-ROM:esp32s3-20210327\r\nboot:0x0 (DOWNLOAD(USB/UART0))\r\nwaiting for download\r\n"...)
	} else {
		f.AppBoots++
	}
	if f.OnBoot != nil {
		f.OnBoot(download)
	}
}

// Write implements Port: SLIP frames from the host.
func (f *FakeROM) Write(p []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return 0, io.ErrClosedPipe
	}
	if !f.Download {
		return len(p), nil // the firmware ignores it
	}
	for _, b := range p {
		d := &f.dec
		if !d.inside {
			if b == slipEnd {
				d.inside, d.frame = true, d.frame[:0]
			}
			continue
		}
		switch {
		case d.esc:
			d.esc = false
			if b == slipEscEnd {
				d.frame = append(d.frame, slipEnd)
			} else {
				d.frame = append(d.frame, slipEsc)
			}
		case b == slipEsc:
			d.esc = true
		case b == slipEnd:
			if len(d.frame) > 0 {
				d.inside = false
				f.handle(append([]byte(nil), d.frame...))
			}
		default:
			d.frame = append(d.frame, b)
		}
	}
	return len(p), nil
}

func (f *FakeROM) respond(op byte, val uint32, data []byte, status, code byte) {
	body := append(append([]byte(nil), data...), status, code, 0, 0)
	pkt := make([]byte, 8, 8+len(body))
	pkt[0], pkt[1] = 0x01, op
	binary.LittleEndian.PutUint16(pkt[2:], uint16(len(body)))
	binary.LittleEndian.PutUint32(pkt[4:], val)
	f.out = append(f.out, slipEncode(append(pkt, body...))...)
}

func (f *FakeROM) handle(pkt []byte) {
	if len(pkt) < 8 || pkt[0] != 0 {
		return
	}
	op := pkt[1]
	size := int(binary.LittleEndian.Uint16(pkt[2:]))
	chk := binary.LittleEndian.Uint32(pkt[4:])
	data := pkt[8:]
	f.Ops = append(f.Ops, op)
	ok := func(val uint32, d []byte) { f.respond(op, val, d, 0, 0) }
	fail := func(code byte) { f.respond(op, 0, nil, 1, code) }
	if size != len(data) {
		fail(0x05)
		return
	}
	w := func(i int) uint32 {
		if len(data) < 4*(i+1) {
			return 0
		}
		return binary.LittleEndian.Uint32(data[4*i:])
	}
	switch op {
	case cmdSync:
		for range 4 {
			ok(0x20120707, nil)
		}
	case cmdReadReg:
		if f.OldROM && w(0) == chipDetectMagicReg {
			ok(f.Magic, nil)
			return
		}
		ok(f.regs[w(0)], nil)
	case cmdWriteReg:
		addr, val, mask := w(0), w(1), w(2)
		f.regs[addr] = f.regs[addr]&^mask | val&mask
		if addr == spiCmdReg && val&spiCmdUsr != 0 {
			if f.regs[spiUsr2Reg]&0xFF == 0x9F {
				sz := byte(0)
				for n := len(f.Flash); n > 1; n >>= 1 {
					sz++
				}
				f.regs[spiW0Reg] = uint32(f.FlashMfr) | 0x40<<8 | uint32(sz)<<16
			}
			f.regs[spiCmdReg] &^= spiCmdUsr
		}
		ok(0, nil)
	case cmdSecurityInfo:
		if f.OldROM {
			fail(0x05)
			return
		}
		flags := uint32(0)
		if f.SecureBoot {
			flags = 1
		}
		d := le32(flags, 0, 0, f.ChipID, 0)
		ok(0, d)
	case cmdSPIAttach, cmdSPISetParams:
		ok(0, nil)
	case cmdFlashBegin:
		size, offset := w(0), w(3)
		if !f.erase(offset, size) {
			fail(0x06)
			return
		}
		f.defl.active = false
		ok(0, nil)
	case cmdFlashDeflBegin:
		size, blocks, offset := w(0), w(1), w(3)
		if !f.erase(offset, size) {
			fail(0x06)
			return
		}
		f.defl.active, f.defl.offset, f.defl.size, f.defl.blocks, f.defl.nextSeq, f.defl.comp =
			true, offset, size, blocks, 0, nil
		ok(0, nil)
	case cmdFlashDeflData:
		n, seq := int(w(0)), w(1)
		blk := data[min(16, len(data)):]
		switch {
		case !f.defl.active, seq != f.defl.nextSeq, n != len(blk):
			fail(0x05)
		case checksum(blk) != chk:
			fail(0x07)
		default:
			f.defl.comp = append(f.defl.comp, blk...)
			f.defl.nextSeq++
			if f.defl.nextSeq == f.defl.blocks {
				if !f.inflate() {
					fail(0x0b)
					return
				}
			}
			ok(0, nil)
		}
	case cmdFlashDeflEnd, cmdFlashEnd:
		f.defl.active = false
		ok(0, nil)
		if w(0) == 0 {
			f.boot(false)
		}
	case cmdSPIFlashMD5:
		addr, size := w(0), w(1)
		if uint64(addr)+uint64(size) > uint64(len(f.Flash)) {
			fail(0x0a)
			return
		}
		sum := md5.Sum(f.Flash[addr : addr+size])
		ok(0, []byte(hex.EncodeToString(sum[:])))
	default:
		fail(0x05)
	}
}

func (f *FakeROM) erase(offset, size uint32) bool {
	end := uint64(offset) + uint64(size)
	if end > uint64(len(f.Flash)) {
		return false
	}
	start := offset &^ 0xFFF
	end = (end + 0xFFF) &^ 0xFFF
	for i := uint64(start); i < end && i < uint64(len(f.Flash)); i++ {
		f.Flash[i] = 0xFF
	}
	return true
}

func (f *FakeROM) inflate() bool {
	zr, err := zlib.NewReader(bytes.NewReader(f.defl.comp))
	if err != nil {
		return false
	}
	raw, err := io.ReadAll(zr)
	if err != nil || uint32(len(raw)) > f.defl.size {
		return false
	}
	if f.CorruptWrite && len(raw) > 0 {
		raw[len(raw)/2] ^= 0x01
	}
	copy(f.Flash[f.defl.offset:], raw)
	f.defl.active = false
	return true
}
