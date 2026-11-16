package esprom

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"time"
)

// Port is the serial port the loader talks through. Read returns 0, nil when the read timeout passes without data
// (go.bug.st/serial behaves like that).
type Port interface {
	io.ReadWriter
	SetReadTimeout(time.Duration) error
}

// Lines are a port's modem control lines. On the ESP32-S3's USB-Serial/JTAG they reach the chip: RTS resets it,
// DTR holds GPIO0 low (download mode) while the reset is released.
type Lines interface {
	SetDTR(bool) error
	SetRTS(bool) error
}

// ROM loader commands.
const (
	cmdFlashBegin     = 0x02
	cmdFlashData      = 0x03
	cmdFlashEnd       = 0x04
	cmdSync           = 0x08
	cmdWriteReg       = 0x09
	cmdReadReg        = 0x0A
	cmdSPISetParams   = 0x0B
	cmdSPIAttach      = 0x0D
	cmdFlashDeflBegin = 0x10
	cmdFlashDeflData  = 0x11
	cmdFlashDeflEnd   = 0x12
	cmdSPIFlashMD5    = 0x13
	cmdSecurityInfo   = 0x14
)

const (
	// statusLen is the length of the status at the end of every ROM response (the ESP32 family's ROMs use 4
	// bytes, the flasher stub 2).
	statusLen = 4
	// FlashBlock is the block size the ROM takes for flash writes.
	FlashBlock = 0x400
	// checksumSeed starts the XOR checksum over a data packet's payload.
	checksumSeed = 0xEF

	defaultTimeout = 3 * time.Second
	syncTimeout    = 100 * time.Millisecond
	// The ROM erases the whole range in FLASH_BEGIN; writes and MD5 are paced per megabyte.
	eraseTimeoutPerMB = 30 * time.Second
	writeTimeoutPerMB = 40 * time.Second
	md5TimeoutPerMB   = 8 * time.Second
)

// ROMError is a command the ROM answered with a failure status.
type ROMError struct {
	Op     string
	Status byte
	Code   byte
}

func (e *ROMError) Error() string {
	reason := map[byte]string{0x05: "invalid message", 0x06: "failed to act on the message", 0x07: "invalid CRC",
		0x08: "flash write error", 0x09: "flash read error", 0x0a: "flash read length error",
		0x0b: "deflate error"}[e.Code]
	if reason == "" {
		reason = fmt.Sprintf("code 0x%02x", e.Code)
	}
	return fmt.Sprintf("%s: the chip answered %s", e.Op, reason)
}

// Loader talks to the ROM bootloader on one port.
type Loader struct {
	p Port
	r slipReader
	// Trace, if set, gets a line per command (for the log in the advanced view).
	Trace func(string)
}

// New starts a loader on a port that is already open.
func New(p Port) *Loader { return &Loader{p: p, r: slipReader{p: p}} }

func (l *Loader) trace(format string, args ...any) {
	if l.Trace != nil {
		l.Trace(fmt.Sprintf(format, args...))
	}
}

func checksum(data []byte) uint32 {
	c := byte(checksumSeed)
	for _, b := range data {
		c ^= b
	}
	return uint32(c)
}

// command sends one command and waits for its response: the response's value field and its data (status
// included).
func (l *Loader) command(op byte, data []byte, chk uint32, timeout time.Duration) (uint32, []byte, error) {
	pkt := make([]byte, 8, 8+len(data))
	pkt[1] = op
	binary.LittleEndian.PutUint16(pkt[2:], uint16(len(data)))
	binary.LittleEndian.PutUint32(pkt[4:], chk)
	pkt = append(pkt, data...)
	if _, err := l.p.Write(slipEncode(pkt)); err != nil {
		return 0, nil, err
	}
	return l.response(op, timeout)
}

// response reads frames until the answer to op arrives; answers to other commands (late SYNC replies) are skipped.
func (l *Loader) response(op byte, timeout time.Duration) (uint32, []byte, error) {
	deadline := time.Now().Add(timeout)
	for range 100 {
		f, err := l.r.next(time.Until(deadline))
		if err != nil {
			return 0, nil, err
		}
		if len(f) < 8 || f[0] != 0x01 || f[1] != op {
			continue
		}
		size := int(binary.LittleEndian.Uint16(f[2:]))
		val := binary.LittleEndian.Uint32(f[4:])
		body := f[8:]
		if size < len(body) {
			body = body[:size]
		}
		return val, body, nil
	}
	return 0, nil, errTimeout
}

// check runs a command and checks the status the ROM appends; it returns the value field and the data before
// the status.
func (l *Loader) check(what string, op byte, data []byte, chk uint32, timeout time.Duration) (uint32, []byte, error) {
	val, body, err := l.command(op, data, chk, timeout)
	if err != nil {
		if errors.Is(err, errTimeout) {
			return 0, nil, fmt.Errorf("%s: no answer from the chip", what)
		}
		return 0, nil, fmt.Errorf("%s: %w", what, err)
	}
	if len(body) < statusLen {
		return 0, nil, fmt.Errorf("%s: short answer from the chip (%d bytes)", what, len(body))
	}
	st := body[len(body)-statusLen:]
	if st[0] != 0 {
		return 0, nil, &ROMError{Op: what, Status: st[0], Code: st[1]}
	}
	return val, body[:len(body)-statusLen], nil
}

// Sync sends the SYNC packet until the ROM answers (a few tries, 100 ms each), then swallows the ROM's extra SYNC
// replies.
func (l *Loader) Sync(tries int) error {
	payload := append([]byte{0x07, 0x07, 0x12, 0x20}, make([]byte, 32)...)
	for i := 4; i < len(payload); i++ {
		payload[i] = 0x55
	}
	for range tries {
		l.r.reset()
		if rp, ok := l.p.(interface{ ResetInputBuffer() error }); ok {
			_ = rp.ResetInputBuffer()
		}
		if _, _, err := l.command(cmdSync, payload, 0, syncTimeout); err != nil {
			continue
		}
		// The ROM answers one SYNC up to eight times: read the rest so they do not pile up.
		for range 7 {
			if _, _, err := l.response(cmdSync, syncTimeout); err != nil {
				break
			}
		}
		l.trace("sync ok")
		return nil
	}
	return errors.New("no answer to SYNC")
}

// ReadReg reads a 32-bit register.
func (l *Loader) ReadReg(addr uint32) (uint32, error) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], addr)
	v, _, err := l.check(fmt.Sprintf("read register 0x%08x", addr), cmdReadReg, b[:], 0, defaultTimeout)
	return v, err
}

// WriteReg writes the bits of mask in a 32-bit register.
func (l *Loader) WriteReg(addr, value, mask uint32) error {
	b := le32(addr, value, mask, 0)
	_, _, err := l.check(fmt.Sprintf("write register 0x%08x", addr), cmdWriteReg, b, 0, defaultTimeout)
	return err
}

// SecurityInfo is the answer to GET_SECURITY_INFO.
type SecurityInfo struct {
	Flags        uint32
	CryptCount   uint8
	KeyPurposes  [7]uint8
	ChipID       uint32 // 9 = ESP32-S3; 0 if the ROM's answer has no chip id
	APIVersion   uint32
	HasChipID    bool
	SecureBoot   bool
	FlashEncrypt bool
}

// SecurityInfo asks the ROM for its security state and chip id (ESP32-S2 and later).
func (l *Loader) SecurityInfo() (*SecurityInfo, error) {
	_, d, err := l.check("get security info", cmdSecurityInfo, nil, 0, defaultTimeout)
	if err != nil {
		return nil, err
	}
	if len(d) < 12 {
		return nil, fmt.Errorf("get security info: short answer (%d bytes)", len(d))
	}
	s := &SecurityInfo{Flags: binary.LittleEndian.Uint32(d), CryptCount: d[4]}
	copy(s.KeyPurposes[:], d[5:12])
	if len(d) >= 20 {
		s.ChipID = binary.LittleEndian.Uint32(d[12:])
		s.APIVersion = binary.LittleEndian.Uint32(d[16:])
		s.HasChipID = true
	}
	s.SecureBoot = s.Flags&1 != 0
	// SPI_BOOT_CRYPT_CNT: encryption is on while an odd number of its bits is set
	n := 0
	for c := s.CryptCount; c != 0; c >>= 1 {
		n += int(c & 1)
	}
	s.FlashEncrypt = n%2 == 1
	return s, nil
}

// SPIAttach connects the ROM to the default SPI flash pins.
func (l *Loader) SPIAttach() error {
	_, _, err := l.check("attach the SPI flash", cmdSPIAttach, make([]byte, 8), 0, defaultTimeout)
	return err
}

// SPISetParams tells the ROM the flash's size.
func (l *Loader) SPISetParams(size uint32) error {
	_, _, err := l.check("set flash parameters", cmdSPISetParams, le32(0, size, 64*1024, 4*1024, 256, 0xFFFF), 0, defaultTimeout)
	return err
}

// MD5 asks the ROM for the MD5 of a flash range, as 32 hex digits.
func (l *Loader) MD5(addr, size uint32) (string, error) {
	_, d, err := l.check("check the flash (MD5)", cmdSPIFlashMD5, le32(addr, size, 0, 0), 0, perMB(md5TimeoutPerMB, size))
	if err != nil {
		return "", err
	}
	switch len(d) {
	case 32:
		return string(d), nil
	case 16:
		return fmt.Sprintf("%x", d), nil
	}
	return "", fmt.Errorf("check the flash (MD5): answer of %d bytes", len(d))
}

func le32(v ...uint32) []byte {
	b := make([]byte, 4*len(v))
	for i, x := range v {
		binary.LittleEndian.PutUint32(b[4*i:], x)
	}
	return b
}

func perMB(per time.Duration, size uint32) time.Duration {
	t := time.Duration(float64(per) * float64(size) / 1e6)
	if t < defaultTimeout {
		return defaultTimeout
	}
	return t
}
