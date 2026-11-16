package pointer

import (
	"strings"
	"unicode"

	"github.com/mozillazg/go-pinyin"
	"golang.org/x/text/unicode/norm"
)

var pinyinArgs = pinyin.NewArgs() // toneless, first reading of a heteronym

func isHan(r rune) bool { return r >= 0x4e00 && r <= 0x9fff || r >= 0x3400 && r <= 0x4dbf }

// HasHan: text has Chinese characters (typed as pinyin).
func HasHan(text string) bool { return strings.ContainsFunc(text, isHan) }

// Layouts are the phone keyboard layouts the board types with (`kbd layout`, firmware src/hid/kbd_map.c). Each
// types printable ASCII; layoutExtra is what it types beyond that. Keep both in step.
var Layouts = []string{"us", "de"}

var layoutExtra = map[string]string{
	"us": "",
	"de": "äöüÄÖÜß§°€´" + "áéíóúàèìòùâêîôûñãõ" + "ÁÉÍÓÚÀÈÌÒÙÂÊÎÔÛÑÃÕ",
}

// KnownLayout: a layout the board knows, "" when not (also "en-US", "de-DE", any case).
func KnownLayout(code string) string {
	switch c := strings.ToLower(code); c {
	case "us", "en", "en-us", "en_us":
		return "us"
	case "de", "de-de", "de_de":
		return "de"
	}
	return ""
}

// CanType: the board types r as itself on the layout ("" or unknown: US, as a board from before layouts).
func CanType(layout string, r rune) bool {
	if r >= 0x20 && r <= 0x7e {
		return true
	}
	return r > 0x7e && strings.ContainsRune(layoutExtra[layout], r)
}

// Typeable turns text into what the board can type on a US layout (see TypeableOn).
func Typeable(text string) string { return TypeableOn("us", text) }

// TypeableOn turns text into what the board can type on the layout: "拿铁 3杯" → "natie 3bei". Han characters
// become joined toneless pinyin (ü as v, as the iOS pinyin keyboard takes it); characters the layout has no keys
// for lose their accents (é → e on US, kept on German), the rest is dropped. NotTyped says which.
func TypeableOn(layout, text string) string {
	all := true
	for _, r := range text {
		if !CanType(layout, r) {
			all = false
			break
		}
	}
	if all {
		return text
	}
	var out strings.Builder
	for _, r := range text {
		if CanType(layout, r) {
			out.WriteRune(r)
			continue
		}
		if isHan(r) {
			if p := pinyin.SinglePinyin(r, pinyinArgs); len(p) > 0 {
				out.WriteString(p[0])
			}
			continue
		}
		for _, d := range norm.NFD.String(string(r)) {
			if d >= 0x20 && d <= 0x7e && !unicode.Is(unicode.Mn, d) {
				out.WriteRune(d)
			}
		}
	}
	return strings.Trim(out.String(), " ")
}

// NotTyped lists, once each, the characters of text (Han aside: pinyin) that the layout cannot type as themselves:
// TypeableOn replaces or drops them.
func NotTyped(layout, text string) []string {
	var out []string
	seen := map[rune]bool{}
	for _, r := range text {
		if CanType(layout, r) || isHan(r) || seen[r] {
			continue
		}
		seen[r] = true
		out = append(out, string(r))
	}
	return out
}
