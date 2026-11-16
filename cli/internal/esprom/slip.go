// Package esprom speaks the serial protocol of the ESP32-S3's ROM bootloader (the "download mode" esptool talks
// to): SLIP framed commands to sync, read the chip's registers and eFuses, attach the SPI flash, write it
// compressed and check it with MD5. It runs against the ROM itself, without uploading a flasher stub.
//
// Protocol reference: Espressif's "Serial Protocol" chapter of the esptool documentation.
package esprom

import (
	"errors"
	"time"
)

const (
	slipEnd    = 0xC0
	slipEsc    = 0xDB
	slipEscEnd = 0xDC
	slipEscEsc = 0xDD
)

// slipEncode wraps one packet in a SLIP frame.
func slipEncode(p []byte) []byte {
	out := make([]byte, 0, len(p)+len(p)/16+2)
	out = append(out, slipEnd)
	for _, b := range p {
		switch b {
		case slipEnd:
			out = append(out, slipEsc, slipEscEnd)
		case slipEsc:
			out = append(out, slipEsc, slipEscEsc)
		default:
			out = append(out, b)
		}
	}
	return append(out, slipEnd)
}

// errTimeout is returned when no complete frame arrived in time.
var errTimeout = errors.New("timeout")

// slipReader cuts SLIP frames out of a byte stream. Bytes outside a frame (the ROM's boot text) are skipped.
type slipReader struct {
	p      Port
	buf    []byte // bytes read but not yet consumed
	frame  []byte
	inside bool
	esc    bool
	tmp    [4096]byte
}

// next returns the next complete frame. Empty frames (two END bytes in a row) are skipped.
func (r *slipReader) next(timeout time.Duration) ([]byte, error) {
	deadline := time.Now().Add(timeout)
	for {
		for len(r.buf) > 0 {
			b := r.buf[0]
			r.buf = r.buf[1:]
			if !r.inside {
				if b == slipEnd {
					r.inside, r.esc, r.frame = true, false, r.frame[:0]
				}
				continue
			}
			switch {
			case r.esc:
				r.esc = false
				switch b {
				case slipEscEnd:
					r.frame = append(r.frame, slipEnd)
				case slipEscEsc:
					r.frame = append(r.frame, slipEsc)
				default: // invalid escape: drop the frame, resync on the next END
					r.inside = false
				}
			case b == slipEsc:
				r.esc = true
			case b == slipEnd:
				if len(r.frame) == 0 { // END END: the second one opens the frame
					continue
				}
				r.inside = false
				f := append([]byte(nil), r.frame...)
				return f, nil
			default:
				r.frame = append(r.frame, b)
			}
		}
		left := time.Until(deadline)
		if left <= 0 {
			return nil, errTimeout
		}
		if left > 100*time.Millisecond {
			left = 100 * time.Millisecond
		}
		if err := r.p.SetReadTimeout(left); err != nil {
			return nil, err
		}
		n, err := r.p.Read(r.tmp[:])
		if err != nil {
			return nil, err
		}
		r.buf = append(r.buf, r.tmp[:n]...)
	}
}

// reset drops everything buffered.
func (r *slipReader) reset() {
	r.buf, r.frame, r.inside, r.esc = r.buf[:0], r.frame[:0], false, false
}
