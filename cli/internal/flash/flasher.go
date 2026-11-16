package flash

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
)

// Stage is a step of the installation, for the progress display.
type Stage string

const (
	StageConnect Stage = "connect" // finding the board, download mode
	StageCheck   Stage = "check"   // chip, flash, PSRAM
	StageErase   Stage = "erase"   // --erase: the whole flash
	StageWrite   Stage = "write"
	StageVerify  Stage = "verify"
	StageRestart Stage = "restart"
	StageSetup   Stage = "setup" // asking the firmware for its setup Wi-Fi
)

// Flasher installs firmware on a board.
type Flasher struct {
	Sys System
	// Log gets English lines for the log (commands, ports, sizes).
	Log func(string)
	// Progress gets the stage and, while writing, bytes done of total.
	Progress func(s Stage, done, total int)
	// Wait is how long a port may take to come back after a restart (default 10 s).
	Wait time.Duration
}

func (f *Flasher) logf(format string, args ...any) {
	if f.Log != nil {
		f.Log(fmt.Sprintf(format, args...))
	}
}

func (f *Flasher) stage(s Stage, done, total int) {
	if f.Progress != nil {
		f.Progress(s, done, total)
	}
}

func (f *Flasher) wait() time.Duration {
	if f.Wait > 0 {
		return f.Wait
	}
	return 10 * time.Second
}

// Board is a board in the ROM bootloader.
type Board struct {
	f      *Flasher
	Port   string
	conn   Conn
	L      *esprom.Loader
	Chip   *esprom.ChipInfo
	Notes  []*Msg // warnings from the checks
	flash  uint32 // flash size the writes assume
	closed bool
}

// Close closes the port without a reset.
func (b *Board) Close() {
	if !b.closed {
		b.closed = true
		_ = b.conn.Close()
	}
}

// Pick chooses the port to use: the one given, else the only candidate.
func (f *Flasher) Pick(port string) (PortInfo, []PortInfo, error) {
	ps, err := f.Sys.Ports()
	if err != nil {
		return PortInfo{}, nil, msg("lost", err, err)
	}
	if port != "" {
		for _, p := range ps {
			if p.Name == port {
				return p, ps, nil
			}
		}
		return PortInfo{Name: port}, ps, nil
	}
	var cands, uarts []PortInfo
	for _, p := range ps {
		switch {
		case p.Candidate():
			cands = append(cands, p)
		case p.Kind == KindUART:
			uarts = append(uarts, p)
		}
	}
	switch {
	case len(cands) == 1:
		return cands[0], ps, nil
	case len(cands) > 1:
		var names []string
		for _, c := range cands {
			names = append(names, c.Name)
		}
		return PortInfo{}, ps, msg("several", nil, strings.Join(names, ", "))
	case len(uarts) > 0:
		return PortInfo{}, ps, msg("uart_board", nil, describe(uarts[0]))
	}
	return PortInfo{}, ps, msg("no_board", nil)
}

// Connect finds the board (or takes port), brings it into the ROM bootloader and syncs. A running firmware is
// restarted into the bootloader, which then shows up as another port; Connect follows it.
func (f *Flasher) Connect(ctx context.Context, port string) (*Board, error) {
	f.stage(StageConnect, 0, 0)
	p, list, err := f.Pick(port)
	if err != nil {
		return nil, err
	}
	before := map[string]bool{}
	for _, q := range list {
		before[q.Name] = true
	}
	f.logf("board on %s (%s %s:%s %q)", p.Name, p.Kind, p.VID, p.PID, p.Product)
	if p.Kind != KindFirmware {
		b, err := f.tryROM(p.Name, true)
		if b != nil || err != nil {
			return b, err
		}
		f.logf("%s: no bootloader answer, restarting it with a 1200 baud touch", p.Name)
	} else {
		f.logf("%s: running firmware, restarting it into the bootloader (1200 baud touch)", p.Name)
	}
	if c, err := f.Sys.Open(p.Name, 1200); err != nil {
		return nil, msg("port_open", err, p.Name, err)
	} else {
		_ = c.Close()
	}
	name, err := f.follow(ctx, before, p.Name)
	if err != nil {
		return nil, err
	}
	f.logf("bootloader on %s", name)
	// the ROM is in download mode after the touch; try without a reset first
	for _, reset := range []bool{false, true} {
		b, err := f.tryROM(name, reset)
		if b != nil || err != nil {
			return b, err
		}
	}
	return nil, msg("no_download", nil, name)
}

// tryROM opens a port and syncs with the ROM, after a USB-JTAG reset into download mode if reset is set. It
// returns nil, nil when nothing answered.
func (f *Flasher) tryROM(name string, reset bool) (*Board, error) {
	var c Conn
	var err error
	for i := range 5 { // the port can take a moment to open after it appeared
		if c, err = f.Sys.Open(name, 115200); err == nil {
			break
		}
		if i < 4 {
			time.Sleep(300 * time.Millisecond)
		}
	}
	if err != nil {
		return nil, msg("port_open", err, name, err)
	}
	if reset {
		esprom.EnterDownloadMode(c)
		time.Sleep(50 * time.Millisecond)
	}
	l := esprom.New(c)
	l.Trace = f.Log
	if err := l.Sync(10); err != nil {
		_ = c.Close()
		return nil, nil
	}
	return &Board{f: f, Port: name, conn: c, L: l}, nil
}

// follow waits for the port the board comes back on after a restart: a new one, or the old name once it has gone
// and come back.
func (f *Flasher) follow(ctx context.Context, before map[string]bool, old string) (string, error) {
	gone := false
	deadline := time.Now().Add(f.wait())
	for time.Now().Before(deadline) {
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
		ps, err := f.Sys.Ports()
		if err != nil {
			continue
		}
		present := false
		for _, p := range ps {
			if p.Name == old {
				present = true
			}
			if p.Candidate() && !before[p.Name] {
				time.Sleep(500 * time.Millisecond) // let the new port settle
				return p.Name, nil
			}
		}
		if !present {
			gone = true
		} else if gone {
			time.Sleep(500 * time.Millisecond)
			return old, nil
		}
	}
	ps, _ := f.Sys.Ports()
	for _, p := range ps {
		if p.Name == old {
			return old, nil
		}
	}
	return "", msg("lost", nil, "the board did not come back after the restart")
}

// Check reads the chip and checks it against the image: an ESP32-S3, no secure boot, enough flash; missing PSRAM
// is only a warning. It sets the flash size for the writes.
func (b *Board) Check(im *Image) error {
	b.f.stage(StageCheck, 0, 0)
	c, err := b.L.ReadChipInfo()
	if err != nil {
		return msg("lost", err, err)
	}
	b.Chip = c
	b.f.logf("chip %s %s, MAC %s, PSRAM %q, flash id %06x (%s), USB-JTAG %v", c.Name, c.Revision, c.MAC, c.PSRAM,
		c.FlashID, c.FlashSizeText(), c.USBJTAG)
	if c.ChipID != esprom.ChipESP32S3 {
		return msg("wrong_chip", nil, c.Name)
	}
	if c.SecureBoot || c.FlashEncrypt {
		return msg("secure", nil)
	}
	if c.USBJTAG {
		if err := b.L.DisableWatchdogs(); err != nil {
			b.f.logf("watchdogs: %v", err)
		}
	}
	need := uint32(MinFlash)
	if im != nil {
		need = max(need, im.FlashNeed)
	}
	b.flash = c.FlashSize
	if c.FlashSize == 0 {
		b.flash = need
		b.Notes = append(b.Notes, msg("flash_unknown", nil, esprom.SizeText(need)))
	} else if c.FlashSize < need {
		return msg("flash_small", nil, esprom.SizeText(c.FlashSize), esprom.SizeText(need))
	}
	if c.PSRAM == "" {
		b.Notes = append(b.Notes, msg("no_psram", nil))
	}
	if err := b.L.SPISetParams(b.flash); err != nil {
		return msg("lost", err, err)
	}
	return nil
}

// Write erases the whole flash if erase is set, writes the image's regions and checks each with MD5.
func (b *Board) Write(ctx context.Context, im *Image, erase bool) error {
	if erase {
		b.f.stage(StageErase, 0, int(b.flash))
		b.f.logf("erasing the whole flash (%s)", esprom.SizeText(b.flash))
		if err := b.L.EraseRange(0, b.flash); err != nil {
			return msg("write", err, err)
		}
		b.f.stage(StageErase, int(b.flash), int(b.flash))
	}
	total, done := im.Bytes(), 0
	for _, r := range im.Regions {
		if err := ctx.Err(); err != nil {
			return err
		}
		b.f.logf("writing %s: %d bytes at 0x%06x", r.Name, len(r.Data), r.Offset)
		base := done
		err := b.L.WriteCompressed(r.Offset, r.Data, func(d, _ int) { b.f.stage(StageWrite, base+min(d, len(r.Data)), total) })
		if err != nil {
			return msg("write", err, err)
		}
		done += len(r.Data)
	}
	for i, r := range im.Regions {
		b.f.stage(StageVerify, i, len(im.Regions))
		if err := b.L.Verify(r.Offset, r.Data); err != nil {
			var ve *esprom.VerifyError
			if errors.As(err, &ve) {
				return msg("verify", err, r.Offset)
			}
			return msg("write", err, err)
		}
	}
	b.f.stage(StageVerify, len(im.Regions), len(im.Regions))
	if err := b.L.Finish(); err != nil {
		b.f.logf("finish: %v", err)
	}
	return nil
}

// Restart leaves the bootloader: it clears the force-download bit and resets the chip into its firmware, then
// closes the port.
func (b *Board) Restart() {
	b.f.stage(StageRestart, 0, 0)
	if b.Chip != nil && b.Chip.ChipID == esprom.ChipESP32S3 { // the register is the S3's
		if err := b.L.ClearForceDownload(); err != nil {
			b.f.logf("clear force download: %v", err)
		}
	}
	esprom.HardReset(b.conn)
	b.Close()
	b.f.logf("restarted the board")
}

// Options are the choices for Install.
type Options struct {
	Port  string // "" = find it
	Image *Image
	Erase bool
	// SetupWait is how long to ask the firmware for its setup Wi-Fi afterwards (0 = 20 s, <0 = not at all).
	SetupWait time.Duration
}

// Result is what an installation found out.
type Result struct {
	Port  string
	Chip  *esprom.ChipInfo
	Notes []*Msg
	Setup *SetupInfo // nil if the board did not say
}

// Install does it all: connect, check, (erase,) write, verify, restart, ask for the setup Wi-Fi.
func (f *Flasher) Install(ctx context.Context, o Options) (*Result, error) {
	if o.Image == nil {
		return nil, ErrNoImage
	}
	b, err := f.Connect(ctx, o.Port)
	if err != nil {
		return nil, err
	}
	defer b.Close()
	if err := b.Check(o.Image); err != nil {
		b.Restart() // back into whatever firmware it has
		return nil, err
	}
	if err := b.Write(ctx, o.Image, o.Erase); err != nil {
		return nil, err
	}
	b.Restart()
	res := &Result{Port: b.Port, Chip: b.Chip, Notes: b.Notes}
	if o.SetupWait >= 0 {
		w := o.SetupWait
		if w == 0 {
			w = 20 * time.Second
		}
		f.stage(StageSetup, 0, 0)
		res.Setup, _ = f.AskSetup(ctx, "", w)
	}
	return res, nil
}

// ChipInfo connects, reads the chip and restarts the board into its firmware.
func (f *Flasher) ChipInfo(ctx context.Context, port string) (*esprom.ChipInfo, []*Msg, error) {
	b, err := f.Connect(ctx, port)
	if err != nil {
		return nil, nil, err
	}
	err = b.Check(nil)
	chip, notes := b.Chip, b.Notes
	b.Restart()
	if chip != nil && err != nil {
		var m *Msg
		if errors.As(err, &m) && m.Code != "lost" {
			notes = append(notes, m) // a wrong chip is the answer here, not a failure
			err = nil
		}
	}
	return chip, notes, err
}
