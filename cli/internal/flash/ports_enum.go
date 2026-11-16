//go:build !darwin

package flash

import (
	"strings"

	"go.bug.st/serial/enumerator"
)

// listPorts lists USB serial ports with their USB ids. Espressif devices are asked for their product string (the
// only thing that tells the bootloader's port from the firmware's).
func listPorts() ([]PortInfo, error) {
	ds, err := enumerator.GetDetailedPortsList(func(vid, pid string) bool { return strings.EqualFold(vid, vidEspressif) })
	if err != nil {
		return nil, err
	}
	var ps []PortInfo
	for _, d := range ds {
		if !d.IsUSB {
			continue
		}
		ps = append(ps, PortInfo{Name: d.Name, VID: strings.ToUpper(d.VID), PID: strings.ToUpper(d.PID),
			Serial: d.SerialNumber, Product: d.Product})
	}
	return ps, nil
}
