package esprom

import (
	"bytes"
	"compress/zlib"
	"crypto/md5"
	"encoding/hex"
	"fmt"
	"io"
	"strings"
)

// Progress reports the bytes of a region written so far (uncompressed) out of its size.
type Progress func(done, total int)

// WriteCompressed writes data at offset with the ROM's compressed flash commands (FLASH_DEFL_*): the ROM erases
// the range first, then inflates and writes block by block. The data is padded to 4 bytes with 0xFF.
func (l *Loader) WriteCompressed(offset uint32, data []byte, progress Progress) error {
	if r := len(data) % 4; r != 0 {
		data = append(append([]byte(nil), data...), bytes.Repeat([]byte{0xFF}, 4-r)...)
	}
	var zb bytes.Buffer
	zw, _ := zlib.NewWriterLevel(&zb, zlib.BestCompression)
	if _, err := zw.Write(data); err != nil {
		return err
	}
	if err := zw.Close(); err != nil {
		return err
	}
	comp := zb.Bytes()
	blocks := (len(comp) + FlashBlock - 1) / FlashBlock
	ends, err := inflatedEnds(comp, blocks)
	if err != nil {
		return err
	}
	size := uint32(len(data))
	eraseSize := (size + FlashBlock - 1) / FlashBlock * FlashBlock
	// The ESP32-S3 ROM takes a fifth word in the BEGIN commands: 1 = write encrypted (never here).
	begin := le32(eraseSize, uint32(blocks), FlashBlock, offset, 0)
	l.trace("write 0x%06x: %d bytes, %d compressed in %d blocks", offset, size, len(comp), blocks)
	if _, _, err := l.check(fmt.Sprintf("start writing at 0x%x", offset), cmdFlashDeflBegin, begin, 0,
		perMB(eraseTimeoutPerMB, eraseSize)); err != nil {
		return err
	}
	prev := 0
	for seq := range blocks {
		blk := comp[seq*FlashBlock : min((seq+1)*FlashBlock, len(comp))]
		pkt := append(le32(uint32(len(blk)), uint32(seq), 0, 0), blk...)
		timeout := perMB(writeTimeoutPerMB, uint32(ends[seq]-prev))
		if _, _, err := l.check(fmt.Sprintf("write block %d of %d at 0x%x", seq+1, blocks, offset),
			cmdFlashDeflData, pkt, checksum(blk), timeout); err != nil {
			return err
		}
		prev = ends[seq]
		if progress != nil {
			progress(min(ends[seq], len(data)), len(data))
		}
	}
	return nil
}

// inflatedEnds says how many uncompressed bytes the ROM has after each compressed block, so each block gets a
// timeout that fits the flash it writes (a block of 0xFF can stand for a lot of flash).
func inflatedEnds(comp []byte, blocks int) ([]int, error) {
	src := bytes.NewReader(comp) // an io.ByteReader: flate reads no further than it needs
	zr, err := zlib.NewReader(src)
	if err != nil {
		return nil, err
	}
	ends := make([]int, blocks)
	out, buf := 0, make([]byte, 4096)
	for {
		n, err := zr.Read(buf)
		out += n
		if used := len(comp) - src.Len(); used > 0 {
			b := min((used-1)/FlashBlock, blocks-1) // the block flate is reading from
			ends[b] = max(ends[b], out)
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
	}
	for i := range ends { // fill blocks that produced nothing on their own
		if i > 0 && ends[i] < ends[i-1] {
			ends[i] = ends[i-1]
		}
	}
	ends[blocks-1] = out
	return ends, nil
}

// Verify compares the MD5 the ROM computes over a flash range with the data that should be there (padded to 4
// bytes like WriteCompressed).
func (l *Loader) Verify(offset uint32, data []byte) error {
	if r := len(data) % 4; r != 0 {
		data = append(append([]byte(nil), data...), bytes.Repeat([]byte{0xFF}, 4-r)...)
	}
	got, err := l.MD5(offset, uint32(len(data)))
	if err != nil {
		return err
	}
	sum := md5.Sum(data)
	want := hex.EncodeToString(sum[:])
	if !strings.EqualFold(got, want) {
		return &VerifyError{Offset: offset, Size: len(data), Got: got, Want: want}
	}
	l.trace("verify 0x%06x: md5 %s ok", offset, want)
	return nil
}

// VerifyError is flash content that differs from what was written.
type VerifyError struct {
	Offset    uint32
	Size      int
	Got, Want string
}

func (e *VerifyError) Error() string {
	return fmt.Sprintf("the flash at 0x%x (%d bytes) differs from the image (md5 %s, expected %s)", e.Offset, e.Size, e.Got, e.Want)
}

// EraseRange erases a flash range with FLASH_BEGIN (the ROM erases the whole range up front) and no data.
func (l *Loader) EraseRange(offset, size uint32) error {
	l.trace("erase 0x%06x: %d bytes", offset, size)
	_, _, err := l.check("erase the flash", cmdFlashBegin, le32(size, 0, FlashBlock, offset, 0), 0,
		perMB(eraseTimeoutPerMB, size))
	return err
}

// Finish ends the flash operation and keeps the ROM in the bootloader (the reset comes from HardReset).
func (l *Loader) Finish() error {
	if _, _, err := l.check("finish writing", cmdFlashBegin, le32(0, 0, FlashBlock, 0, 0), 0, defaultTimeout); err != nil {
		return err
	}
	_, _, err := l.check("finish writing", cmdFlashDeflEnd, le32(1), 0, defaultTimeout)
	return err
}
