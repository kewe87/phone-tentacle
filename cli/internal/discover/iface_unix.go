//go:build !windows

package discover

import (
	"net"
	"syscall"
)

// setMulticastIface makes the socket's multicast leave through the interface that has ip: binding to the address
// alone does not choose it on Linux and macOS.
func setMulticastIface(c *net.UDPConn, ip net.IP) error {
	rc, err := c.SyscallConn()
	if err != nil {
		return err
	}
	var serr error
	var a [4]byte
	copy(a[:], ip.To4())
	if err := rc.Control(func(fd uintptr) {
		serr = syscall.SetsockoptInet4Addr(int(fd), syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, a)
	}); err != nil {
		return err
	}
	return serr
}
