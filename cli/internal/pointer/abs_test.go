package pointer

import (
	"context"
	"fmt"
	"testing"
)

// An absolute board: no calibration, no homing; one `abs` per point, the button only after it.
func TestAbsPointerTapsAndSwipesWithoutCalibration(t *testing.T) {
	d := &fakeDevice{}
	p := newPointer(t, d, nil)
	p.Abs = true
	ctx := context.Background()
	if !p.Calibrated() || p.Calibrate(ctx) != nil || len(d.sent) != 0 {
		t.Fatalf("abs pointer: calibrated %v, sent %v", p.Calibrated(), d.sent)
	}
	if err := p.Tap(ctx, 0.5, 0.25, true, 2); err != nil { // --aim: nothing to aim at, no wiggle
		t.Fatal(err)
	}
	if err := p.Swipe(ctx, 0.5, 0.8, 0.5, 0.2, true); err != nil {
		t.Fatal(err)
	}
	if err := p.HomeSwipe(ctx); err != nil {
		t.Fatal(err)
	}
	want := []string{"release", "abs 16384 8192", "click", "click",
		"abs 16384 26214", "press", "abs 16384 6553", "release",
		"abs 16384 32603", "press", fmt.Sprintf("abs 16384 4915 %d", FlickStep), "release"}
	if fmt.Sprint(d.sent) != fmt.Sprint(want) {
		t.Fatalf("sent %q\nwant %q", d.sent, want)
	}
	if AbsUnit(-0.1) != 0 || AbsUnit(1.2) != AbsFull {
		t.Fatal("abs units not clamped to the screen")
	}
}
