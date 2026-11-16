package daemon

import (
	"image"
	"math"
)

// The phone screen in the screenshot, for the absolute pointer: `abs` takes fractions of the phone screen, and the
// screenshot is the AirPlay frame. iOS sends the frame in the phone's own aspect (718x1560 for a 1179x2556 iPhone
// whatever the board asks for, 480x1040), so normally all of it is screen. Should a frame ever come with black bars
// around the picture (letterbox / pillarbox), they are not screen: a tap at a pixel then lands at its fraction of
// the area inside them.

// phoneAspects are width / height of phone and tablet screens (portrait).
var phoneAspects = []float64{9 / 19.5, 9.0 / 16, 3.0 / 4, 1640.0 / 2360, 1488.0 / 2266}

// blackEdges measures how many rows / columns at each edge (left, top, right, bottom) are black, each at most lim.
// Kept as the smallest over all frames of a size: black content (dark mode) comes and goes, real bars stay.
func blackEdges(img image.Image, lim [4]int) [4]int {
	b := img.Bounds()
	black := func(x0, y0, x1, y1 int) bool { // every other pixel of the line [x0,x1) x [y0,y1)
		for y := y0; y < y1; y += 2 {
			for x := x0; x < x1; x += 2 {
				if !dark(img, x, y) {
					return false
				}
			}
		}
		return true
	}
	out := lim
	for i := 0; i < 4; i++ {
		n := 0
		for n < lim[i] {
			var ok bool
			switch i {
			case 0:
				ok = black(b.Min.X+n, b.Min.Y, b.Min.X+n+1, b.Max.Y)
			case 1:
				ok = black(b.Min.X, b.Min.Y+n, b.Max.X, b.Min.Y+n+1)
			case 2:
				ok = black(b.Max.X-1-n, b.Min.Y, b.Max.X-n, b.Max.Y)
			case 3:
				ok = black(b.Min.X, b.Max.Y-1-n, b.Max.X, b.Max.Y-n)
			}
			if !ok {
				break
			}
			n++
		}
		out[i] = n
	}
	return out
}

func dark(img image.Image, x, y int) bool {
	const max = 24 // of 255: video black is 16 in YUV and decodes to about 0
	if r, ok := img.(*image.RGBA); ok {
		i := r.PixOffset(x, y)
		return r.Pix[i] <= max && r.Pix[i+1] <= max && r.Pix[i+2] <= max
	}
	cr, cg, cb, _ := img.At(x, y).RGBA()
	return cr>>8 <= max && cg>>8 <= max && cb>>8 <= max
}

// screenRect is the phone screen within a w x h frame whose black edges are bars (left, top, right, bottom). A pair
// of opposite bars counts when both are there and about equal (AirPlay centres the picture), and only when the
// picture inside them has a phone's aspect; in a frame that has one itself only bars of 3% or more: black at the
// top and bottom of a dark app is not a bar.
func screenRect(w, h int, bars [4]int) image.Rectangle {
	full := image.Rect(0, 0, w, h)
	if w <= 0 || h <= 0 {
		return full
	}
	r := full
	pair := func(a, b, size int) bool {
		return a >= 2 && b >= 2 && iabs(a-b) <= max(2, size/200) && a+b < size/2
	}
	if pair(bars[0], bars[2], w) {
		r.Min.X, r.Max.X = bars[0], w-bars[2]
	}
	if pair(bars[1], bars[3], h) {
		r.Min.Y, r.Max.Y = bars[1], h-bars[3]
	}
	// The receiver's own size (480x1040) has a phone's aspect too: then the bars must be wide to count.
	wide := r.Min.X >= w*3/100 || r.Min.Y >= h*3/100
	if r != full && aspectMiss(r) < 0.008 && (aspectMiss(full) >= 0.008 || wide) {
		return r
	}
	return full
}

// aspectMiss is how far r's aspect is from the nearest phone's, relative.
func aspectMiss(r image.Rectangle) float64 {
	aspect := float64(min(r.Dx(), r.Dy())) / float64(max(r.Dx(), r.Dy()))
	miss := math.Inf(1)
	for _, a := range phoneAspects {
		miss = math.Min(miss, math.Abs(aspect-a)/a)
	}
	return miss
}

func iabs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

// screenArea is the phone screen in screenshot pixels: the latest frame without its bars. Without a frame there
// is no telling which size the caller's pixels are of (errNoFrame).
func (c *Conn) screenArea() (image.Rectangle, error) {
	c.mu.Lock()
	f, bars, size := c.frame, c.bars, c.barsSize
	c.mu.Unlock()
	if f == nil {
		return image.Rectangle{}, errNoFrame
	}
	w, h := f.Bounds().Dx(), f.Bounds().Dy()
	if size != (image.Point{w, h}) {
		bars = [4]int{}
	}
	return screenRect(w, h, bars), nil
}
