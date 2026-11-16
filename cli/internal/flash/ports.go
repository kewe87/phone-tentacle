package flash

import (
	"strings"
	"time"

	"go.bug.st/serial"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
)

// Kind is what is behind a serial port.
type Kind int

const (
	// KindUnknown: an Espressif (or macOS usbmodem) port whose mode the USB descriptors do not tell.
	KindUnknown Kind = iota
	// KindROM: the ESP32-S3's USB-Serial/JTAG, product "USB JTAG/serial debug unit". The ROM bootloader talks
	// here, and a blank board shows up like this.
	KindROM
	// KindFirmware: the TinyUSB port of a running firmware (product "Tentacle", hid_bridge.ino).
	KindFirmware
	// KindUART: a USB serial adapter (CH340, CP210x, FTDI): a board without native USB, not ours.
	KindUART
	// KindOther: anything else (not shown).
	KindOther
)

func (k Kind) String() string {
	return [...]string{"unknown", "bootloader", "firmware", "uart", "other"}[k]
}

// PortInfo describes a serial port.
type PortInfo struct {
	Name    string `json:"name"`
	VID     string `json:"vid,omitempty"`
	PID     string `json:"pid,omitempty"`
	Serial  string `json:"serial,omitempty"`
	Product string `json:"product,omitempty"`
	Kind    Kind   `json:"-"`
	KindStr string `json:"kind"`
}

// USB ids. The ROM's USB-Serial/JTAG and the firmware's TinyUSB port both report 303A:1001 (arduino-esp32's
// esp32s3 variant sets USB_PID 0x1001), so the product string tells them apart. On Windows they get different
// COM numbers all the same (COM5 bootloader, COM6 firmware on the maintainer's machine); macOS without cgo shows
// no USB ids at all. Where nothing tells, the flasher tries: USB-JTAG reset and SYNC first, then a 1200 baud
// touch.
const (
	vidEspressif = "303A"
	vidSeeed     = "2886"
)

var uartVIDs = map[string]string{"1A86": "a CH340/CH9102 (WCH)", "10C4": "a CP210x (Silicon Labs)", "0403": "an FTDI chip"}

func classify(p *PortInfo) {
	vid := strings.ToUpper(p.VID)
	switch {
	case vid == vidEspressif && strings.Contains(strings.ToUpper(p.Product), "JTAG"):
		p.Kind = KindROM
	case vid == vidEspressif && p.Product != "":
		p.Kind = KindFirmware
	case vid == vidEspressif, vid == vidSeeed:
		p.Kind = KindUnknown
	case uartVIDs[vid] != "", vid == "" && p.Product == productUART:
		p.Kind = KindUART
	case vid == "" && isUSBModem(p.Name):
		p.Kind = KindUnknown
	default:
		p.Kind = KindOther
	}
	p.KindStr = p.Kind.String()
}

// productUART marks an adapter port where the system gives no USB ids (macOS).
const productUART = "USB serial adapter"

// describe names a port for the "not our board" message.
func describe(p PortInfo) string {
	if d := uartVIDs[strings.ToUpper(p.VID)]; d != "" {
		return p.Name + " (" + d + ")"
	}
	return p.Name
}

func isUSBModem(name string) bool {
	return strings.HasPrefix(name, "/dev/cu.usbmodem") || strings.HasPrefix(name, "/dev/ttyACM")
}

// Candidate says whether a port may be a Tentacle board.
func (p PortInfo) Candidate() bool {
	return p.Kind == KindROM || p.Kind == KindFirmware || p.Kind == KindUnknown
}

// Conn is an open serial port.
type Conn interface {
	esprom.Port
	esprom.Lines
	ResetInputBuffer() error
	Close() error
}

// System lists and opens serial ports. Serial is the real one; tests use a fake.
type System interface {
	Ports() ([]PortInfo, error)
	// Open opens a port with DTR and RTS low (no reset). Opening at 1200 baud is the "touch" that restarts a
	// running Arduino firmware into the bootloader.
	Open(name string, baud int) (Conn, error)
}

// Serial is the computer's serial ports (go.bug.st/serial, no cgo).
type Serial struct{}

// Ports lists the USB serial ports, classified.
func (Serial) Ports() ([]PortInfo, error) {
	ps, err := listPorts()
	if err != nil {
		return nil, err
	}
	for i := range ps {
		classify(&ps[i])
	}
	return ps, nil
}

// Open implements System.
func (Serial) Open(name string, baud int) (Conn, error) {
	p, err := serial.Open(name, &serial.Mode{BaudRate: baud, DataBits: 8, Parity: serial.NoParity,
		StopBits: serial.OneStopBit, InitialStatusBits: &serial.ModemOutputBits{}})
	if err != nil {
		return nil, err
	}
	return serialConn{p}, nil
}

type serialConn struct{ serial.Port }

func (c serialConn) SetReadTimeout(t time.Duration) error { return c.Port.SetReadTimeout(t) }
