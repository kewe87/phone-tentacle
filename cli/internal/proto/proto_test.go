package proto

import (
	"bytes"
	"context"
	"crypto/ecdh"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"testing"
	"time"
	"unicode/utf8"
)

type vframe struct {
	Stream string `json:"stream"`
	Type   byte   `json:"type"`
	Ctr    uint32 `json:"ctr"`
	Plain  string `json:"plain"`
	Frame  string `json:"frame"`
}

type vectors struct {
	CPriv, BPriv, CPub, BPub, Shared, ClientID, BoardID, K, PairMac, CNonce, BNonce, KC2b, KB2c, HelloMac string
	PairNonce, PairCommit, PairCode                                                                       string
	FrameC                                                                                                vframe `json:"frame_c"`
	FrameB                                                                                                vframe `json:"frame_b"`
	FrameV                                                                                                vframe `json:"frame_v"`
}

func (v *vectors) UnmarshalJSON(b []byte) error {
	var m map[string]json.RawMessage
	if err := json.Unmarshal(b, &m); err != nil {
		return err
	}
	str := func(k string) string { var s string; json.Unmarshal(m[k], &s); return s }
	v.CPriv, v.BPriv, v.CPub, v.BPub, v.Shared = str("c_priv"), str("b_priv"), str("c_pub"), str("b_pub"), str("shared")
	v.ClientID, v.BoardID, v.K, v.PairMac = str("client_id"), str("board_id"), str("K"), str("pair_mac")
	v.CNonce, v.BNonce, v.KC2b, v.KB2c, v.HelloMac = str("c_nonce"), str("b_nonce"), str("k_c2b"), str("k_b2c"), str("hello_mac")
	v.PairNonce, v.PairCommit, v.PairCode = str("pair_nonce"), str("pair_commit"), str("pair_code")
	json.Unmarshal(m["frame_c"], &v.FrameC)
	json.Unmarshal(m["frame_b"], &v.FrameB)
	json.Unmarshal(m["frame_v"], &v.FrameV)
	return nil
}

func h(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

func load(t *testing.T) vectors {
	raw, err := os.ReadFile("../../testdata/vectors.json")
	if err != nil {
		t.Fatal(err)
	}
	var v vectors
	if err := json.Unmarshal(raw, &v); err != nil {
		t.Fatal(err)
	}
	return v
}

func eq(t *testing.T, name string, got []byte, want string) {
	t.Helper()
	if hex.EncodeToString(got) != want {
		t.Errorf("%s = %x, want %s", name, got, want)
	}
}

func TestVectors(t *testing.T) {
	v := load(t)
	cPriv, _ := ecdh.X25519().NewPrivateKey(h(v.CPriv))
	bPriv, _ := ecdh.X25519().NewPrivateKey(h(v.BPriv))
	eq(t, "c_pub", cPriv.PublicKey().Bytes(), v.CPub)
	eq(t, "b_pub", bPriv.PublicKey().Bytes(), v.BPub)
	shared, _ := cPriv.ECDH(bPriv.PublicKey())
	eq(t, "shared", shared, v.Shared)
	k := PairKey(shared, h(v.CPub), h(v.BPub))
	eq(t, "K", k, v.K)
	eq(t, "pair_mac", PairMAC(k, h(v.ClientID)), v.PairMac)
	eq(t, "pair_commit", PairCommit(h(v.PairNonce)), v.PairCommit)
	if got := PairCode(k, h(v.PairNonce)); got != v.PairCode || got != "615618" {
		t.Fatalf("pair_code: got %s want %s", got, v.PairCode)
	}
	eq(t, "hello_mac", HelloMAC(k, h(v.CNonce), h(v.BNonce)), v.HelloMac)
	c2b, b2c := SessionKeys(k, h(v.CNonce), h(v.BNonce))
	eq(t, "k_c2b", c2b, v.KC2b)
	eq(t, "k_b2c", b2c, v.KB2c)
	for _, f := range []vframe{v.FrameC, v.FrameB, v.FrameV} {
		key := b2c
		if f.Stream == "C" {
			key = c2b
		}
		a, _ := NewAEAD(key)
		eq(t, "frame "+f.Stream, a.Seal(f.Stream[0], f.Type, f.Ctr, h(f.Plain)), f.Frame)
		ctr, plain, err := a.Open(f.Stream[0], h(f.Frame))
		if err != nil || ctr != f.Ctr || hex.EncodeToString(plain) != f.Plain {
			t.Errorf("open %s: ctr %d plain %x err %v", f.Stream, ctr, plain, err)
		}
		if _, _, err := a.Open('X', h(f.Frame)); err == nil {
			t.Errorf("frame %s opened with the wrong stream", f.Stream)
		}
	}
}

var (
	clientID = bytes.Repeat([]byte{7}, 16)
	boardID  = h("246f28aa6ad5")
)

func ctx(t *testing.T) context.Context {
	c, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	t.Cleanup(cancel)
	return c
}

// paired returns a session to a fake board that paired with clientID.
func paired(t *testing.T) (*FakeBoard, *Session) {
	b := NewFakeBoard(boardID)
	l := b.Connect()
	k, id, err := Pair(ctx(t), l, clientID, "test-pc", nil)
	if err != nil || !bytes.Equal(id, boardID) {
		t.Fatalf("pair: %v %x", err, id)
	}
	s, err := Hello(ctx(t), l, clientID, func(id []byte) []byte { return k })
	if err != nil {
		t.Fatal(err)
	}
	return b, s
}

func next(t *testing.T, s *Session) (Msg, bool, error) {
	t.Helper()
	select {
	case f := <-s.Recv():
		return s.Decode(f)
	case <-time.After(time.Second):
		t.Fatal("no frame")
	}
	return Msg{}, false, nil
}

func TestPairHelloAndCommand(t *testing.T) {
	b, s := paired(t)
	if err := s.Send("7 ping"); err != nil {
		t.Fatal(err)
	}
	m, ok, err := next(t, s)
	if !ok || err != nil || m.Kind != 'r' || string(m.Data) != "7 ok" {
		t.Fatalf("reply %c %q ok=%v err=%v", m.Kind, m.Data, ok, err)
	}
	b.Status(`{"ap":"streaming"}`)
	m, ok, _ = next(t, s)
	if !ok || m.Kind != 's' || string(m.Data) != `{"ap":"streaming"}` {
		t.Fatalf("status %q", m.Data)
	}
}

func TestBoardSendsNothingBeforeTheClientProvesK(t *testing.T) {
	b, s := paired(t)
	b.Status("{}")
	b.Video(1, 3, []byte{1, 2, 3, 4})
	select {
	case f := <-s.Recv():
		t.Fatalf("got %x before the first DATA frame", f)
	case <-time.After(50 * time.Millisecond):
	}
}

func TestPairNotConfirmedAndUnknownClient(t *testing.T) {
	b := NewFakeBoard(boardID)
	b.Boot = func(string, string) bool { return false } // nobody presses BOOT
	prompted := false
	_, _, err := Pair(ctx(t), b.Connect(), clientID, "test-pc", func(string, time.Duration) { prompted = true })
	var be BoardError
	if !errors.As(err, &be) || !strings.HasPrefix(string(be), "pairing not confirmed") || !prompted {
		t.Fatalf("pair without the button: %v (prompted %v)", err, prompted)
	}
	_, err = Hello(ctx(t), b.Connect(), clientID, func([]byte) []byte { return nil })
	if !errors.As(err, &be) || be != "not paired" {
		t.Fatalf("hello unknown: %v", err)
	}
}

// PAIR: c_pub[32] client_id[16] commit[32] name[0..32]; the name is optional and cut to 32 bytes at a rune boundary.
func TestPairFrame(t *testing.T) {
	cPub, commit := bytes.Repeat([]byte{1}, 32), bytes.Repeat([]byte{3}, 32)
	f := PairFrame(cPub, clientID, commit, "")
	if len(f) != 1+32+16+32 || f[0] != TPair || !bytes.Equal(f[1:33], cPub) || !bytes.Equal(f[33:49], clientID) || !bytes.Equal(f[49:81], commit) {
		t.Fatalf("frame without a name: %x", f)
	}
	f = PairFrame(cPub, clientID, commit, "Kevins-PC")
	if len(f) != 81+9 || string(f[81:]) != "Kevins-PC" {
		t.Fatalf("frame with a name: %x", f)
	}
	for _, name := range []string{strings.Repeat("x", 40), strings.Repeat("ü", 20), strings.Repeat("x", 31) + "ü", strings.Repeat("x", 30) + "日本", "PC-" + strings.Repeat("😀", 9)} {
		f = PairFrame(cPub, clientID, commit, name)
		got := string(f[81:])
		if len(got) > PairNameMax || !utf8.ValidString(got) || !strings.HasPrefix(name, got) || len(got) < PairNameMax-3 {
			t.Fatalf("name %q cut to %q (%d bytes)", name, got, len(got))
		}
	}
}

func TestPairShowsTheBoardsCode(t *testing.T) {
	b := NewFakeBoard(boardID)
	var boardName, boardCode, cliCode string
	b.Boot = func(name, code string) bool { boardName, boardCode = name, code; return true }
	l := b.Connect()
	k, id, err := Pair(ctx(t), l, clientID, "Kevins-PC", func(code string, wait time.Duration) {
		cliCode = code
		if wait != 30*time.Second {
			t.Errorf("wait %v", wait)
		}
	})
	if err != nil || !bytes.Equal(id, boardID) || len(k) != 32 {
		t.Fatalf("pair: %v", err)
	}
	if boardName != "Kevins-PC" || len(cliCode) != 6 || cliCode != boardCode {
		t.Fatalf("board shows %q %q, the CLI %q", boardName, boardCode, cliCode)
	}
	if _, err := Hello(ctx(t), l, clientID, func([]byte) []byte { return k }); err != nil {
		t.Fatalf("hello on the link that paired: %v", err)
	}
}

func TestPairBusy(t *testing.T) {
	b := NewFakeBoard(boardID)
	b.PairErr = "busy: board in use (a paired computer's session; end it, wait 10 s)"
	_, _, err := Pair(ctx(t), b.Connect(), clientID, "pc", func(string, time.Duration) { t.Error("prompted") })
	var be BoardError
	if !errors.As(err, &be) || !strings.HasPrefix(string(be), "busy: board in use") {
		t.Fatalf("pair on a busy board: %v", err)
	}
}

// tamper rewrites frames from the board, as someone in the middle would.
type tamper struct {
	Link
	out  chan []byte
	edit func(f []byte) []byte
}

func newTamper(l Link, edit func([]byte) []byte) *tamper {
	t := &tamper{Link: l, out: make(chan []byte, 16), edit: edit}
	go func() {
		for f := range l.Recv() {
			t.out <- edit(bytes.Clone(f))
		}
		close(t.out)
	}()
	return t
}

func (t *tamper) Recv() <-chan []byte { return t.out }

func TestPairRejectsAnotherKeyInPaired(t *testing.T) {
	b := NewFakeBoard(boardID)
	l := newTamper(b.Connect(), func(f []byte) []byte {
		if f[0] == TPaired {
			f[1] ^= 1 // not the key PAIR_KEY carried
		}
		return f
	})
	if _, _, err := Pair(ctx(t), l, clientID, "pc", nil); err == nil || !strings.Contains(err.Error(), "proof does not match") {
		t.Fatalf("pair with a changed PAIRED: %v", err)
	}
}

// Someone in the middle who swaps the board's key gets another K, so the two codes differ (and PAIRED's proof fails).
func TestSwappedKeyChangesTheCode(t *testing.T) {
	b := NewFakeBoard(boardID)
	var boardCode, cliCode string
	b.Boot = func(_, code string) bool { boardCode = code; return true }
	other, _ := ecdh.X25519().GenerateKey(rand.Reader)
	l := newTamper(b.Connect(), func(f []byte) []byte {
		if f[0] == TPairKey {
			copy(f[1:], other.PublicKey().Bytes())
		}
		return f
	})
	_, _, err := Pair(ctx(t), l, clientID, "pc", func(code string, _ time.Duration) { cliCode = code })
	if err == nil {
		t.Fatal("pair through a swapped key succeeded")
	}
	if cliCode == "" || cliCode == boardCode {
		t.Fatalf("codes %q (CLI) and %q (board) should differ", cliCode, boardCode)
	}
}

func TestWrongKeyIsRejected(t *testing.T) {
	b := NewFakeBoard(boardID)
	l := b.Connect()
	if _, _, err := Pair(ctx(t), l, clientID, "test-pc", nil); err != nil {
		t.Fatal(err)
	}
	_, err := Hello(ctx(t), l, clientID, func([]byte) []byte { return make([]byte, 32) })
	if err == nil {
		t.Fatal("hello with a wrong K succeeded")
	}
}

func TestReplayedFramesAreDropped(t *testing.T) {
	b, s := paired(t)
	s.Send("1 ping")
	if _, ok, _ := next(t, s); !ok {
		t.Fatal("first reply dropped")
	}
	sent := b.SentFrames()
	b.Inject(sent[len(sent)-1]) // the same reply again
	if _, ok, err := next(t, s); ok || err != nil {
		t.Fatalf("replayed reply accepted: ok=%v err=%v", ok, err)
	}
	b.Video(1, 3, []byte{0})
	if m, ok, _ := next(t, s); !ok || m.Gap {
		t.Fatal("first video frame")
	}
	sent = b.SentFrames()
	b.Inject(sent[len(sent)-1])
	if _, ok, _ := next(t, s); ok {
		t.Fatal("replayed video accepted")
	}

	// board side: a replayed command is not run twice
	before := len(b.Commands)
	s.Send("2 click")
	next(t, s)
	frame := s.c2b.Seal(StreamC, TData, s.ctrC, []byte("c2 click"))
	s.l.Write(frame)
	if len(b.Commands) != before+1 {
		t.Fatalf("commands %v", b.Commands)
	}
}

func TestControlGapEndsSessionVideoGapDoesNot(t *testing.T) {
	b, s := paired(t)
	s.Send("1 ping")
	next(t, s)
	b.Video(1, 3, []byte{0})
	next(t, s)
	b.mu.Lock()
	b.conn.ctrV++
	b.mu.Unlock()
	b.Video(3, 3, []byte{0})
	if m, ok, err := next(t, s); !ok || err != nil || !m.Gap {
		t.Fatalf("video after a gap: ok=%v err=%v gap=%v", ok, err, m.Gap)
	}
	b.SkipB()
	b.Status("{}")
	if _, _, err := next(t, s); !errors.Is(err, ErrSession) {
		t.Fatalf("control gap: %v", err)
	}
}

func TestClientCounterGapEndsBoardSession(t *testing.T) {
	_, s := paired(t)
	s.Send("1 ping")
	next(t, s)
	s.ctrC++ // one command lost
	s.Send("3 ping")
	select {
	case _, open := <-s.Recv():
		if open {
			t.Fatal("board answered after a counter gap")
		}
	case <-time.After(time.Second):
		t.Fatal("board kept the session")
	}
}
