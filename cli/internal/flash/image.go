package flash

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
)

// Flash layout of the arduino-esp32 build (FlashSize=8M, PartitionScheme=default_8MB), as hardware/flash.sh
// writes it through arduino-cli: bootloader at 0x0, partition table at 0x8000, boot_app0 (otadata) at 0xe000,
// the app at 0x10000. The build's *.merged.bin holds all four at these offsets, padded with 0xFF.
const (
	bootloaderOffset = 0x0
	partTableOffset  = 0x8000
	partTableSize    = 0x1000
	otadataOffset    = 0xe000
	otadataSize      = 0x2000
	appOffset        = 0x10000
	sector           = 0x1000
	// MinFlash is the flash every Tentacle board has (Seeed XIAO ESP32-S3).
	MinFlash = 8 << 20
)

// releaseKey checks signatures (a variable so tests can sign with their own key).
var releaseKey = fwsign.ReleaseKey

// Region is one piece of the image and where it goes.
type Region struct {
	Name   string
	Offset uint32
	Data   []byte
}

// Image is a firmware file, cut into the regions that get written.
type Image struct {
	Name      string
	Size      int
	SHA256    string
	Kind      string // "merged" (bootloader, partition table, otadata, app) or "app" (only the app)
	Version   string // from the app's tentacle-fw marker; "" if none
	Signed    bool   // a signature came with it and matches the release key
	Regions   []Region
	FlashNeed uint32 // flash the layout needs (end of the last partition, the bootloader header's size)
	Notes     []*Msg // warnings to show before writing (unsigned, app only)
}

// Bytes is the total of all regions.
func (im *Image) Bytes() int {
	n := 0
	for _, r := range im.Regions {
		n += len(r.Data)
	}
	return n
}

// LoadImage reads an image file and FILE.sig next to it, if there is one.
func LoadImage(path string) (*Image, error) {
	f, err := readFile(path)
	if err != nil {
		return nil, err
	}
	return ParseImage(f.name, f.data, f.sig)
}

type fileImage struct {
	name      string
	data, sig []byte
}

func readFile(path string) (fileImage, error) {
	f := fileImage{name: filepath.Base(path)}
	var err error
	if f.data, err = os.ReadFile(path); err != nil {
		return f, msg("read_image", err, path, err.Error())
	}
	if f.sig, err = fwsign.ReadSig(path); err != nil {
		return f, msg("bad_sig", err, f.name)
	}
	return f, nil
}

// ParseImage checks an image and cuts it into regions. sig (64 bytes, may be nil) is an Ed25519 signature over
// the whole file in fwsign's scheme; one that does not match the release key is an error, none is a warning.
func ParseImage(name string, data, sig []byte) (*Image, error) {
	bad := func(why string) error { return msg("bad_image", nil, name, why) }
	sum := sha256.Sum256(data)
	im := &Image{Name: name, Size: len(data), SHA256: hex.EncodeToString(sum[:]), Version: fwsign.Version(data)}
	if sig != nil {
		if !fwsign.Verify(releaseKey, data, sig) {
			return nil, msg("bad_sig", nil, name)
		}
		im.Signed = true
	} else {
		im.Notes = append(im.Notes, msg("unsigned", nil, name))
	}
	if err := checkESPImage(data, "the file"); err != nil {
		return nil, bad(err.Error())
	}
	if isPartTable(data) {
		if err := im.cutMerged(data); err != nil {
			return nil, bad(err.Error())
		}
		return im, nil
	}
	if len(data) < 0x8000 || strings.Contains(strings.ToLower(name), "bootloader") {
		return nil, bad("it looks like the bootloader alone; use the .merged.bin")
	}
	// the app alone: boot_app0 makes the bootloader start the app in app0 (0x10000)
	im.Kind = "app"
	im.FlashNeed = MinFlash
	im.Regions = []Region{{"otadata", otadataOffset, bootApp0()}, {"app", appOffset, trimFF(data, len(data))}}
	im.Notes = append(im.Notes, msg("app_only", nil, name))
	return im, nil
}

// checkESPImage checks an ESP image header: magic byte and the ESP32-S3's chip id.
func checkESPImage(b []byte, what string) error {
	if len(b) < 24 || b[0] != 0xE9 {
		return fmt.Errorf("%s does not start with an ESP32 image", what)
	}
	if chip := uint32(binary.LittleEndian.Uint16(b[12:])); chip != esprom.ChipESP32S3 {
		return fmt.Errorf("%s is built for the %s, not the ESP32-S3", what, esprom.ChipName(chip))
	}
	return nil
}

func isPartTable(data []byte) bool {
	return len(data) >= partTableOffset+32 && data[partTableOffset] == 0xAA && data[partTableOffset+1] == 0x50
}

type partition struct {
	Type, SubType byte
	Offset, Size  uint32
	Label         string
}

// partitions reads the partition table at 0x8000.
func partitions(data []byte) ([]partition, error) {
	var ps []partition
	for off := partTableOffset; off+32 <= partTableOffset+0xC00 && off+32 <= len(data); off += 32 {
		e := data[off : off+32]
		if e[0] == 0xFF && e[1] == 0xFF || e[0] == 0xEB && e[1] == 0xEB { // end, or the MD5 entry
			break
		}
		if e[0] != 0xAA || e[1] != 0x50 {
			return nil, errors.New("its partition table is damaged")
		}
		p := partition{Type: e[2], SubType: e[3], Offset: binary.LittleEndian.Uint32(e[4:]),
			Size: binary.LittleEndian.Uint32(e[8:]), Label: strings.TrimRight(string(e[12:28]), "\x00")}
		if p.Offset%sector != 0 || p.Offset < partTableOffset+partTableSize {
			return nil, fmt.Errorf("partition %q has a bad offset 0x%x", p.Label, p.Offset)
		}
		ps = append(ps, p)
	}
	if len(ps) == 0 {
		return nil, errors.New("its partition table is empty")
	}
	return ps, nil
}

// cutMerged cuts a merged image into what hardware/flash.sh writes: bootloader, partition table, otadata (the
// build's boot_app0) and the app, each without the 0xFF padding behind it. Partitions the build leaves empty
// (nvs with the Wi-Fi and the pairing, the second app slot, spiffs, coredump) are not touched, as with an
// arduino-cli upload; --erase clears them.
func (im *Image) cutMerged(data []byte) error {
	im.Kind = "merged"
	ps, err := partitions(data)
	if err != nil {
		return err
	}
	at := func(off, size uint32) []byte { // a range of the file, padded with 0xFF past its end
		b := bytes.Repeat([]byte{0xFF}, int(size))
		if int(off) < len(data) {
			copy(b, data[off:min(int(off+size), len(data))])
		}
		return b
	}
	im.Regions = []Region{{"bootloader", bootloaderOffset, trimFF(data[:partTableOffset], partTableOffset)},
		{"partition table", partTableOffset, at(partTableOffset, partTableSize)}}
	if bs := data[3] >> 4; bs <= 7 {
		im.FlashNeed = 1 << (20 + bs) // the bootloader header's flash size: 0 = 1 MB ... 3 = 8 MB
	}
	app := false
	for _, p := range ps {
		im.FlashNeed = max(im.FlashNeed, p.Offset+p.Size)
		content := at(p.Offset, p.Size)
		switch {
		case p.Type == 1 && p.SubType == 0: // otadata: always written, it picks the app slot
			im.Regions = append(im.Regions, Region{"otadata", p.Offset, content})
		case p.Type == 0 && !app:
			if err := checkESPImage(content, "the app in "+p.Label); err != nil {
				return err
			}
			app = true
			im.Regions = append(im.Regions, Region{"app (" + p.Label + ")", p.Offset, trimFF(content, len(content))})
		default:
			if t := trimFF(content, len(content)); len(t) > 0 {
				im.Regions = append(im.Regions, Region{p.Label, p.Offset, t})
			}
		}
	}
	if !app {
		return errors.New("its partition table has no app")
	}
	if err := checkESPImage(data, "the bootloader"); err != nil {
		return err
	}
	return nil
}

// trimFF drops the 0xFF at the end and rounds up to a whole sector (at most to limit). Erased flash reads 0xFF,
// and the ROM erases whole sectors, so the bytes written plus the erased rest of the last sector equal the file.
func trimFF(b []byte, limit int) []byte {
	n := len(b)
	for n > 0 && b[n-1] == 0xFF {
		n--
	}
	if n == 0 {
		return nil
	}
	n = min((n+sector-1)/sector*sector, limit, len(b))
	return b[:n]
}

// bootApp0 is arduino-esp32's boot_app0.bin: otadata whose first entry (sequence 1, CRC) selects app0.
func bootApp0() []byte {
	b := bytes.Repeat([]byte{0xFF}, otadataSize)
	copy(b, []byte{1, 0, 0, 0})
	copy(b[28:], []byte{0x9a, 0x98, 0x43, 0x47})
	return b
}
