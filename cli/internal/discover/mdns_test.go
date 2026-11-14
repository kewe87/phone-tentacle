package discover

import (
	"bytes"
	"encoding/binary"
	"net"
	"testing"
)

// msg builds DNS messages for the tests, with name compression like a real responder's.
type msg struct {
	b     []byte
	names map[string]int // name → offset, for pointers
}

func newMsg(flags uint16) *msg {
	m := &msg{b: make([]byte, 12), names: map[string]int{}}
	binary.BigEndian.PutUint16(m.b[2:], flags)
	return m
}

// name appends a name, pointing at an earlier copy of it (or of its tail) where there is one.
func (m *msg) name(labels ...string) {
	for i := range labels {
		key := ""
		for _, l := range labels[i:] {
			key += l + "."
		}
		if off, ok := m.names[key]; ok {
			m.b = binary.BigEndian.AppendUint16(m.b, 0xC000|uint16(off))
			return
		}
		m.names[key] = len(m.b)
		m.b = append(m.b, byte(len(labels[i])))
		m.b = append(m.b, labels[i]...)
	}
	m.b = append(m.b, 0)
}

// rr appends a record header and its data (written by fill), in section sec (6 answers, 10 additional).
func (m *msg) rr(sec int, typ uint16, owner []string, fill func()) {
	m.name(owner...)
	m.b = binary.BigEndian.AppendUint16(m.b, typ)
	m.b = binary.BigEndian.AppendUint16(m.b, 0x8001) // IN, cache flush
	m.b = binary.BigEndian.AppendUint32(m.b, 120)
	lenAt := len(m.b)
	m.b = append(m.b, 0, 0)
	fill()
	binary.BigEndian.PutUint16(m.b[lenAt:], uint16(len(m.b)-lenAt-2))
	binary.BigEndian.PutUint16(m.b[sec:], binary.BigEndian.Uint16(m.b[sec:])+1)
}

var (
	svc  = []string{"_esp32agent", "_tcp", "local"}
	inst = []string{"ESP32 Agent 6AD5", "_esp32agent", "_tcp", "local"}
	host = []string{"iphone-agent", "local"}
)

// answer is what the board sends: PTR in the answers; SRV, TXT and A in the additionals.
func answer(txt ...string) *msg {
	m := newMsg(0x8400)
	m.rr(6, typePTR, svc, func() { m.name(inst...) })
	m.rr(10, typeSRV, inst, func() {
		m.b = append(m.b, 0, 0, 0, 0)
		m.b = binary.BigEndian.AppendUint16(m.b, 7202)
		m.name(host...)
	})
	m.rr(10, typeTXT, inst, func() {
		for _, t := range txt {
			m.b = append(m.b, byte(len(t)))
			m.b = append(m.b, t...)
		}
	})
	m.rr(10, typeA, host, func() { m.b = append(m.b, 192, 168, 178, 61) })
	return m
}

func TestQuery(t *testing.T) {
	q := Query(0x1234)
	want := []byte{0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
		11, '_', 'e', 's', 'p', '3', '2', 'a', 'g', 'e', 'n', 't', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
		0, 12, 0x80, 0x01} // PTR, IN + unicast response
	if !bytes.Equal(q, want) {
		t.Fatalf("query\n got %x\nwant %x", q, want)
	}
	if Parse(q, net.IPv4(1, 2, 3, 4)) != nil {
		t.Fatal("a question parsed as an answer")
	}
}

func TestParseAnswer(t *testing.T) {
	m := answer("id=246F28AA6AD5", "name=ESP32 Agent 6AD5", "fw=0.2.5", "paired=1", "v=1")
	from := net.IPv4(192, 168, 178, 61).To4()
	bs := Parse(m.b, from)
	if len(bs) != 1 {
		t.Fatalf("boards: %+v", bs)
	}
	b := bs[0]
	if b.Instance != "ESP32 Agent 6AD5" || b.ID != "246f28aa6ad5" || b.Name != "ESP32 Agent 6AD5" || b.FW != "0.2.5" ||
		b.Paired != 1 || b.Port != 7202 || !b.IP.Equal(from) || b.Addr() != "192.168.178.61:7202" {
		t.Fatalf("board: %+v", b)
	}
	// Without the sender's address (a relayed answer) the A record gives it.
	if bs = Parse(m.b, nil); len(bs) != 1 || bs[0].Addr() != "192.168.178.61:7202" {
		t.Fatalf("address from the A record: %+v", bs)
	}
}

func TestParseWithoutTXTOrWithAnotherPort(t *testing.T) {
	m := newMsg(0x8400)
	m.rr(6, typePTR, svc, func() { m.name(inst...) })
	m.rr(6, typeSRV, inst, func() {
		m.b = append(m.b, 0, 0, 0, 0)
		m.b = binary.BigEndian.AppendUint16(m.b, 7999)
		m.name(host...)
	})
	bs := Parse(m.b, net.IPv4(10, 0, 0, 7))
	if len(bs) != 1 || bs[0].Port != 7999 || bs[0].ID != "" || bs[0].Paired != -1 || bs[0].Name != "ESP32 Agent 6AD5" {
		t.Fatalf("boards: %+v", bs)
	}
}

func TestParseIgnoresOtherServicesAndGarbage(t *testing.T) {
	// AirPlay's records from the same board are not ours.
	m := newMsg(0x8400)
	m.rr(6, typePTR, []string{"_airplay", "_tcp", "local"}, func() { m.name("ESP32 Agent", "_airplay", "_tcp", "local") })
	m.rr(10, typeTXT, []string{"ESP32 Agent", "_airplay", "_tcp", "local"}, func() { m.b = append(m.b, 4, 'i', 'd', '=', 'x') })
	if bs := Parse(m.b, net.IPv4(10, 0, 0, 7)); len(bs) != 0 {
		t.Fatalf("an AirPlay answer gave %+v", bs)
	}
	// Every truncation of a good answer parses without a panic, and never invents a board id.
	good := answer("id=246f28aa6ad5", "paired=0").b
	for n := 0; n < len(good); n++ {
		for _, b := range Parse(good[:n], net.IPv4(10, 0, 0, 7)) {
			if b.ID != "" && b.ID != "246f28aa6ad5" {
				t.Fatalf("cut at %d: %+v", n, b)
			}
		}
	}
	// A compression pointer that points at itself ends, it does not loop.
	loop := append(newMsg(0x8400).b, 0xC0, 12, 0, 12, 0x80, 1, 0, 0, 0, 0, 0, 0)
	binary.BigEndian.PutUint16(loop[6:], 1)
	if bs := Parse(loop, net.IPv4(10, 0, 0, 7)); len(bs) != 0 {
		t.Fatalf("a looping name gave %+v", bs)
	}
	// Bytes flipped anywhere: no panic.
	for i := range good {
		bad := bytes.Clone(good)
		bad[i] ^= 0xFF
		Parse(bad, net.IPv4(10, 0, 0, 7))
	}
}

func TestTwoBoardsInOneAnswer(t *testing.T) {
	m := newMsg(0x8400)
	other := []string{"ESP32 Agent 1A2B", "_esp32agent", "_tcp", "local"}
	m.rr(6, typePTR, svc, func() { m.name(inst...) })
	m.rr(6, typePTR, svc, func() { m.name(other...) })
	m.rr(10, typeTXT, inst, func() {
		m.b = append(m.b, 15, 'i', 'd', '=', '2', '4', '6', 'f', '2', '8', 'a', 'a', '6', 'a', 'd', '5')
	})
	m.rr(10, typeTXT, other, func() {
		m.b = append(m.b, 15, 'i', 'd', '=', '2', '4', '6', 'f', '2', '8', 'a', 'a', '1', 'a', '2', 'b')
	})
	bs := Parse(m.b, net.IPv4(10, 0, 0, 7))
	if len(bs) != 2 || bs[0].ID != "246f28aa6ad5" || bs[1].ID != "246f28aa1a2b" || bs[1].Instance != "ESP32 Agent 1A2B" {
		t.Fatalf("boards: %+v", bs)
	}
}
