// Package discover finds boards on the local network: by mDNS / DNS-SD (the board announces _tentacle._tcp on
// port 7202, docs/cli-protocol.md, "Discovery"), and as a last resort by trying that port on the local subnets.
// A small implementation of its own, so the CLI needs no mDNS library and no service on the computer.
package discover

import (
	"context"
	"encoding/binary"
	"errors"
	"net"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Service is the DNS-SD service the board registers.
const Service = "_tentacle._tcp.local."

// Port is the board's CLI link (TCP).
const Port = 7202

var mdnsGroup = &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}

// Board is one answer.
type Board struct {
	Instance string // "Tentacle 6AD5"
	ID       string // board id, 12 hex (TXT id)
	Name     string // TXT name
	FW       string // TXT fw
	Paired   int    // TXT paired: computers paired with it; -1 if not given
	Open     int    // TXT open: 1 while the board takes a pairing request (BOOT was double-pressed), 0 not; -1 if not given
	IP       net.IP // where the answer came from
	Port     int
	Iface    string // the interface it was found on
}

// Addr is host:port for a dial.
func (b Board) Addr() string { return net.JoinHostPort(b.IP.String(), strconv.Itoa(b.Port)) }

const (
	typeA   = 1
	typePTR = 12
	typeTXT = 16
	typeSRV = 33
)

// Query builds the question: PTR for Service, class IN with the "unicast response" bit, so a responder answers
// straight to our socket (which is not on port 5353: no multicast group to join, nothing to share with the OS).
func Query(id uint16) []byte {
	q := make([]byte, 12, 64)
	binary.BigEndian.PutUint16(q, id)
	binary.BigEndian.PutUint16(q[4:], 1) // one question
	for _, label := range strings.Split(strings.TrimSuffix(Service, "."), ".") {
		q = append(q, byte(len(label)))
		q = append(q, label...)
	}
	q = append(q, 0)
	q = binary.BigEndian.AppendUint16(q, typePTR)
	return binary.BigEndian.AppendUint16(q, 0x8001)
}

var errShort = errors.New("short DNS message")

// name reads a possibly compressed name at off; next is the offset after it in the record.
func name(msg []byte, off int) (s string, next int, err error) {
	var b strings.Builder
	next = -1
	for hops := 0; ; {
		if off >= len(msg) {
			return "", 0, errShort
		}
		n := int(msg[off])
		switch {
		case n == 0:
			if next < 0 {
				next = off + 1
			}
			return b.String(), next, nil
		case n&0xC0 == 0xC0:
			if off+1 >= len(msg) {
				return "", 0, errShort
			}
			if next < 0 {
				next = off + 2
			}
			off = (n&0x3F)<<8 | int(msg[off+1])
			if hops++; hops > 16 {
				return "", 0, errors.New("DNS name loops")
			}
		case n&0xC0 != 0:
			return "", 0, errors.New("bad DNS label")
		default:
			if off+1+n > len(msg) || b.Len()+n > 255 {
				return "", 0, errShort
			}
			b.Write(msg[off+1 : off+1+n])
			b.WriteByte('.')
			off += 1 + n
		}
	}
}

// Parse reads the boards out of one mDNS response received from `from`. Records of other services are ignored.
func Parse(msg []byte, from net.IP) []Board {
	if len(msg) < 12 || msg[2]&0x80 == 0 { // not a response
		return nil
	}
	qd := int(binary.BigEndian.Uint16(msg[4:]))
	rr := int(binary.BigEndian.Uint16(msg[6:])) + int(binary.BigEndian.Uint16(msg[8:])) + int(binary.BigEndian.Uint16(msg[10:]))
	off := 12
	for i := 0; i < qd; i++ {
		_, next, err := name(msg, off)
		if err != nil || next+4 > len(msg) {
			return nil
		}
		off = next + 4
	}
	type srv struct {
		port   int
		target string
	}
	var instances []string
	srvs, txts, addrs := map[string]srv{}, map[string]map[string]string{}, map[string]net.IP{}
	for i := 0; i < rr; i++ {
		owner, next, err := name(msg, off)
		if err != nil || next+10 > len(msg) {
			break
		}
		typ := binary.BigEndian.Uint16(msg[next:])
		n := int(binary.BigEndian.Uint16(msg[next+8:]))
		data := next + 10
		if data+n > len(msg) {
			break
		}
		off = data + n
		owner = strings.ToLower(owner)
		switch typ {
		case typePTR:
			if owner != Service {
				continue
			}
			if inst, _, err := name(msg, data); err == nil {
				instances = append(instances, inst)
			}
		case typeSRV:
			if n < 7 {
				continue
			}
			if target, _, err := name(msg, data+6); err == nil {
				srvs[owner] = srv{int(binary.BigEndian.Uint16(msg[data+4:])), strings.ToLower(target)}
			}
		case typeTXT:
			kv := map[string]string{}
			for p := data; p < data+n; {
				l := int(msg[p])
				if p+1+l > data+n {
					break
				}
				if k, v, ok := strings.Cut(string(msg[p+1:p+1+l]), "="); ok {
					kv[strings.ToLower(k)] = v
				}
				p += 1 + l
			}
			txts[owner] = kv
		case typeA:
			if n == 4 {
				addrs[owner] = net.IP(append([]byte{}, msg[data:data+4]...))
			}
		}
	}
	var out []Board
	seen := map[string]bool{}
	for _, inst := range instances {
		key := strings.ToLower(inst)
		if seen[key] {
			continue
		}
		seen[key] = true
		b := Board{Instance: strings.TrimSuffix(inst[:max(0, len(inst)-len(Service))], "."), Port: Port, Paired: -1, Open: -1, IP: from}
		if s, ok := srvs[key]; ok {
			if s.port != 0 {
				b.Port = s.port
			}
			if b.IP == nil {
				b.IP = addrs[s.target]
			}
		}
		if t := txts[key]; t != nil {
			b.ID, b.Name, b.FW = strings.ToLower(t["id"]), t["name"], t["fw"]
			if n, err := strconv.Atoi(t["paired"]); err == nil {
				b.Paired = n
			}
			if n, err := strconv.Atoi(t["open"]); err == nil && (n == 0 || n == 1) {
				b.Open = n
			}
		}
		if b.Name == "" {
			b.Name = b.Instance
		}
		if b.IP != nil {
			out = append(out, b)
		}
	}
	return out
}

// Iface is a local IPv4 interface worth asking on.
type Iface struct {
	Name string
	IP   net.IP
	Net  *net.IPNet
}

// Interfaces lists the IPv4 addresses of interfaces that are up and can do multicast, loopback excepted. A computer
// often has several (Ethernet, Wi-Fi, VPN, virtual switches): the question goes out on every one.
func Interfaces() []Iface {
	var out []Iface
	ifs, _ := net.Interfaces()
	for _, i := range ifs {
		if i.Flags&net.FlagUp == 0 || i.Flags&net.FlagLoopback != 0 || i.Flags&net.FlagMulticast == 0 {
			continue
		}
		addrs, _ := i.Addrs()
		for _, a := range addrs {
			if n, ok := a.(*net.IPNet); ok {
				if ip4 := n.IP.To4(); ip4 != nil && !ip4.IsLinkLocalUnicast() {
					out = append(out, Iface{i.Name, ip4, &net.IPNet{IP: ip4.Mask(n.Mask), Mask: n.Mask}})
				}
			}
		}
	}
	return out
}

// When the question is sent again on each interface: Wi-Fi drops multicast now and then.
var resend = []time.Duration{0, 250 * time.Millisecond, time.Second, 2500 * time.Millisecond}

// settle: how long to keep listening after the first answer, for other boards.
const settle = 600 * time.Millisecond

// Find asks on every interface and collects answers for up to d (less once a board has answered). logf may be nil.
func Find(ctx context.Context, d time.Duration, logf func(string, ...any)) []Board {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	ctx, cancel := context.WithTimeout(ctx, d)
	defer cancel()
	started := time.Now()
	var (
		mu    sync.Mutex
		found = map[string]Board{}
		first = make(chan struct{})
		once  sync.Once
		wg    sync.WaitGroup
	)
	listen := func(conn *net.UDPConn, in Iface) {
		defer wg.Done()
		defer conn.Close()
		go func() {
			<-ctx.Done()
			conn.SetReadDeadline(time.Now())
		}()
		buf := make([]byte, 9000)
		for {
			n, from, err := conn.ReadFromUDP(buf)
			if err != nil {
				return
			}
			for _, b := range Parse(buf[:n], from.IP.To4()) {
				b.Iface = in.Name
				key := b.ID + "|" + b.IP.String()
				mu.Lock()
				_, dup := found[key]
				found[key] = b
				mu.Unlock()
				if !dup {
					logf("discover: %q id %s fw %s paired %d at %s via %s after %v", b.Name, b.ID, b.FW, b.Paired, b.Addr(), in.Name,
						time.Since(started).Round(time.Millisecond))
				}
				once.Do(func() { close(first) })
			}
		}
	}
	ifs := Interfaces()
	if len(ifs) == 0 {
		logf("discover: no network interface with an IPv4 address")
	}
	for _, in := range ifs {
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: in.IP})
		if err != nil {
			logf("discover: %s (%s): %v", in.Name, in.IP, err)
			continue
		}
		if err := setMulticastIface(conn, in.IP); err != nil {
			logf("discover: %s (%s): multicast interface: %v", in.Name, in.IP, err)
		}
		logf("discover: asking for %s on %s (%s)", Service, in.Name, in.IP)
		// A responder that answers to the group instead of to us is heard here. Best effort: the port may be taken.
		if ifi, err := net.InterfaceByName(in.Name); err == nil {
			if mc, err := net.ListenMulticastUDP("udp4", ifi, mdnsGroup); err == nil {
				wg.Add(1)
				go listen(mc, in)
			} else {
				logf("discover: %s: not listening on the mDNS group (%v); unicast answers still arrive", in.Name, err)
			}
		}
		wg.Add(2)
		go func(in Iface) { // ask
			defer wg.Done()
			q := Query(uint16(time.Now().UnixNano()))
			for _, at := range resend {
				select {
				case <-ctx.Done():
					return
				case <-time.After(time.Until(started.Add(at))):
				}
				if _, err := conn.WriteToUDP(q, mdnsGroup); err != nil {
					logf("discover: %s: send: %v", in.Name, err)
					return
				}
			}
		}(in)
		go listen(conn, in)
	}
	go func() {
		select {
		case <-first:
			select {
			case <-time.After(settle):
			case <-ctx.Done():
			}
			cancel()
		case <-ctx.Done():
		}
	}()
	wg.Wait()
	out := make([]Board, 0, len(found))
	byID := map[string]bool{}
	mu.Lock()
	keys := make([]string, 0, len(found))
	for k := range found {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		b := found[k]
		if b.ID != "" && byID[b.ID] { // the same board heard on two interfaces
			continue
		}
		byID[b.ID] = true
		out = append(out, b)
	}
	mu.Unlock()
	logf("discover: %d board(s) by mDNS in %v", len(out), time.Since(started).Round(time.Millisecond))
	return out
}

// Probe is the way without multicast (a router that keeps it from crossing between Wi-Fi and cable, a firewall):
// it tries TCP port 7202 on every address of the local /24 networks and returns those that accept, as host:port.
// Only private networks are tried. Who answers there is proven later, by the session's keys.
func Probe(ctx context.Context, logf func(string, ...any)) []string {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	started := time.Now()
	var targets []string
	seen := map[string]bool{}
	for _, in := range Interfaces() {
		if !in.IP.IsPrivate() {
			continue
		}
		base := in.IP.Mask(net.CIDRMask(24, 32))
		if seen[base.String()] {
			continue
		}
		seen[base.String()] = true
		logf("discover: trying port %d on %s/24 (%s)", Port, base, in.Name)
		for h := 1; h < 255; h++ {
			ip := net.IPv4(base[0], base[1], base[2], byte(h))
			if !ip.Equal(in.IP) {
				targets = append(targets, net.JoinHostPort(ip.String(), strconv.Itoa(Port)))
			}
		}
	}
	var (
		mu  sync.Mutex
		out []string
		wg  sync.WaitGroup
		sem = make(chan struct{}, 64)
	)
	for _, t := range targets {
		wg.Add(1)
		sem <- struct{}{}
		go func(t string) {
			defer wg.Done()
			defer func() { <-sem }()
			d := net.Dialer{Timeout: 400 * time.Millisecond}
			c, err := d.DialContext(ctx, "tcp4", t)
			if err != nil {
				return
			}
			c.Close()
			mu.Lock()
			out = append(out, t)
			mu.Unlock()
		}(t)
	}
	wg.Wait()
	sort.Strings(out)
	logf("discover: %d address(es) accept port %d (%v): %v", len(out), Port, time.Since(started).Round(time.Millisecond), out)
	return out
}
