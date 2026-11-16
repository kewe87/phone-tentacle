package main

import (
	"strings"

	"golang.org/x/sys/windows"
)

// systemLang is "de" if Windows' display language is German.
func systemLang() string {
	ls, err := windows.GetUserPreferredUILanguages(windows.MUI_LANGUAGE_NAME)
	if err == nil && len(ls) > 0 && strings.HasPrefix(strings.ToLower(ls[0]), "de") {
		return "de"
	}
	return "en"
}

// enableVT lets the console show the colours of the QR code (Windows 10 and later).
func enableVT() {
	h := windows.Handle(windows.Stdout)
	var mode uint32
	if windows.GetConsoleMode(h, &mode) == nil {
		_ = windows.SetConsoleMode(h, mode|windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING)
	}
}
