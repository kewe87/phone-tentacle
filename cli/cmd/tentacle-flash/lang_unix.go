//go:build !windows

package main

import (
	"os"
	"strings"
)

// systemLang is "de" if the locale (LC_ALL, LC_MESSAGES, LANG) is German.
func systemLang() string {
	for _, k := range []string{"LC_ALL", "LC_MESSAGES", "LANG"} {
		if v := os.Getenv(k); v != "" {
			if strings.HasPrefix(strings.ToLower(v), "de") {
				return "de"
			}
			return "en"
		}
	}
	return "en"
}

func enableVT() {}
