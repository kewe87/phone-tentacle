package main

import (
	"bytes"
	"context"
	"strings"
	"testing"

	"github.com/kewe87/phone-tentacle/cli/internal/esprom"
	"github.com/kewe87/phone-tentacle/cli/internal/flash"
)

func TestFlags(t *testing.T) {
	var out, errOut bytes.Buffer
	if run([]string{"--version"}, &out, &errOut) != 0 || !strings.Contains(out.String(), "tentacle-flash dev") {
		t.Errorf("--version: %q", out.String())
	}
	if run([]string{"stray"}, &out, &errOut) != 2 {
		t.Error("a stray argument is accepted")
	}
	out.Reset()
	if run([]string{"--yes", "--lang", "de", "--image", "does-not-exist.bin"}, &out, &errOut) != 1 ||
		!strings.Contains(errOut.String(), "lässt sich nicht lesen") {
		t.Errorf("missing image: %q", errOut.String())
	}
}

func TestInstallOutput(t *testing.T) {
	rom := esprom.NewFakeROM()
	rom.Download = false
	sys := flash.NewFakeSystem(rom, `wifi-setup {"setup":1,"ssid":"Tentacle-1A2B","pass":"abcdefghijkl","url":"http://192.168.4.1/","qr":"`+
		strings.Repeat("FFFFFF8,", 20)+`FFFFFF8"}`)
	img := bytes.Repeat([]byte{0xFF}, 0x20000)
	copy(img, []byte{0xE9, 6, 2, 0x3F})
	img[12], img[13] = 9, 0
	copy(img[0x100:], "tentacle-fw:0.4.0;")
	var out, errOut bytes.Buffer
	c := &cli{out: &out, err: &errOut, lang: "en"}
	if code := c.install(context.Background(), sys, flash.Bytes{Name: "t.ino.bin", Data: img}, "", false); code != 0 {
		t.Fatalf("exit %d: %s", code, errOut.String())
	}
	for _, want := range []string{"Firmware: t.ino.bin (0.4.0), NOT signed", "Board: ESP32-S3", "Done", "Tentacle-1A2B", "abcdefghijkl", "█"} {
		if !strings.Contains(out.String(), want) {
			t.Errorf("output lacks %q:\n%s", want, out.String())
		}
	}
	if !strings.Contains(errOut.String(), "Warning:") {
		t.Errorf("no warning for the unsigned app-only image: %q", errOut.String())
	}
}
