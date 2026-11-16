package flash

import (
	"strings"

	"go.bug.st/serial"
)

// listPorts lists macOS's USB modem ports. The USB ids need IOKit through cgo, which this build does without: the
// board shows up as /dev/cu.usbmodem*, and the flasher finds out by trying what is behind it.
func listPorts() ([]PortInfo, error) {
	names, err := serial.GetPortsList()
	if err != nil {
		return nil, err
	}
	var ps []PortInfo
	for _, n := range names {
		if strings.HasPrefix(n, "/dev/cu.usbmodem") || strings.HasPrefix(n, "/dev/cu.usbserial") ||
			strings.HasPrefix(n, "/dev/cu.wchusbserial") || strings.HasPrefix(n, "/dev/cu.SLAB_USBtoUART") {
			p := PortInfo{Name: n}
			if !strings.HasPrefix(n, "/dev/cu.usbmodem") {
				p.Product = productUART // a USB serial adapter: a board without native USB
			}
			ps = append(ps, p)
		}
	}
	return ps, nil
}
