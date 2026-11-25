// Package flash installs the Tentacle firmware on a board over USB: it finds the board, puts it into the ROM
// bootloader, checks the chip, writes the image with internal/esprom, restarts the board and reads its setup
// Wi-Fi. cmd/tentacle-flash drives it from the command line and from a local web page.
package flash

import "fmt"

// Msg is a message for people, in English and German, with what to do about it. As an error, it reads English.
type Msg struct {
	Code string
	Args []any
	Err  error // the cause, for the log
}

func (m *Msg) Error() string { return m.Text("en") }
func (m *Msg) Unwrap() error { return m.Err }

// Text is the message in a language ("de" or anything else for English).
func (m *Msg) Text(lang string) string {
	t, ok := messages[m.Code]
	if !ok {
		if m.Err != nil {
			return m.Err.Error()
		}
		return m.Code
	}
	f := t[0]
	if lang == "de" {
		f = t[1]
	}
	return fmt.Sprintf(f, m.Args...)
}

func msg(code string, err error, args ...any) *Msg { return &Msg{Code: code, Args: args, Err: err} }

// messages: code -> {English, German}.
var messages = map[string][2]string{
	"no_board": {
		"No board found. Plug the Tentacle board (Seeed XIAO ESP32-S3 or Waveshare ESP32-S3-ePaper-1.54) into this computer with a USB-C data cable (a charge-only cable does not work), then try again.",
		"Kein Board gefunden. Steck das Tentacle-Board (Seeed XIAO ESP32-S3 oder Waveshare ESP32-S3-ePaper-1.54) mit einem USB-C-Datenkabel an diesen Computer (ein reines Ladekabel geht nicht) und versuch es dann noch einmal.",
	},
	"uart_board": {
		"Found %s, a USB serial adapter, but no ESP32-S3 with its own USB port. Tentacle needs an ESP32-S3 board such as the Seeed XIAO ESP32-S3 or the Waveshare ESP32-S3-ePaper-1.54; classic ESP32 boards cannot act as mouse and keyboard.",
		"Gefunden: %s, ein USB-Seriell-Wandler, aber kein ESP32-S3 mit eigenem USB-Anschluss. Tentacle braucht ein ESP32-S3-Board wie den Seeed XIAO ESP32-S3 oder das Waveshare ESP32-S3-ePaper-1.54; klassische ESP32-Boards können nicht Maus und Tastatur sein.",
	},
	"several": {
		"Several boards are connected (%s). Unplug the others, or choose the port under Advanced (--port).",
		"Mehrere Boards sind angeschlossen (%s). Steck die anderen ab oder wähl den Anschluss unter Erweitert (--port).",
	},
	"port_open": {
		"Port %s cannot be opened (%v). Close other programs that use it (Arduino IDE, a serial monitor, another tentacle-flash), then try again. On Linux, \"permission denied\" means your user may not use serial ports yet: sudo usermod -aG dialout $USER (Arch: uucp), then log out and in again.",
		"Der Anschluss %s lässt sich nicht öffnen (%v). Schließ andere Programme, die ihn benutzen (Arduino IDE, seriellen Monitor, ein zweites tentacle-flash), und versuch es noch einmal. Unter Linux heißt „permission denied“, dass dein Benutzer noch keine seriellen Anschlüsse benutzen darf: sudo usermod -aG dialout $USER (Arch: uucp), dann ab- und wieder anmelden.",
	},
	"no_download": {
		"The board on %s did not switch into download mode. Put it there by hand: hold the BOOT button (B), press and release RESET (R), let go of BOOT, then try again. If that does not work: unplug it, hold BOOT while you plug it back in.",
		"Das Board an %s ist nicht in den Download-Modus gewechselt. Schalte ihn von Hand ein: BOOT-Taste (B) halten, RESET (R) kurz drücken, BOOT loslassen, dann noch einmal versuchen. Wenn das nicht hilft: abstecken, BOOT halten und dabei wieder einstecken.",
	},
	"lost": {
		"The connection to the board broke off (%v). Plug it in again, directly rather than through a hub, then try again.",
		"Die Verbindung zum Board ist abgerissen (%v). Steck es wieder ein, am besten direkt statt über einen Hub, und versuch es noch einmal.",
	},
	"wrong_chip": {
		"Wrong chip: this board has an %s. Tentacle runs only on the ESP32-S3 (Seeed XIAO ESP32-S3, Waveshare ESP32-S3-ePaper-1.54).",
		"Falscher Chip: Auf diesem Board sitzt ein %s. Tentacle läuft nur auf dem ESP32-S3 (Seeed XIAO ESP32-S3, Waveshare ESP32-S3-ePaper-1.54).",
	},
	"secure": {
		"Secure boot or flash encryption is switched on in this chip's eFuses. Such a chip takes only images signed with its own key; Tentacle cannot be installed on it.",
		"In den eFuses dieses Chips ist Secure Boot oder Flash-Verschlüsselung eingeschaltet. So ein Chip nimmt nur Images mit seinem eigenen Schlüssel; Tentacle lässt sich darauf nicht installieren.",
	},
	"flash_small": {
		"The board has %s of flash; Tentacle needs %s. Use the Seeed XIAO ESP32-S3 or the Waveshare ESP32-S3-ePaper-1.54 (8 MB flash each).",
		"Das Board hat %s Flash; Tentacle braucht %s. Nimm den Seeed XIAO ESP32-S3 oder das Waveshare ESP32-S3-ePaper-1.54 (je 8 MB Flash).",
	},
	"write": {
		"Writing failed (%v). Plug the board in again and try again; if it keeps failing, try another USB cable or port.",
		"Das Schreiben ist fehlgeschlagen (%v). Steck das Board neu ein und versuch es noch einmal; wenn es weiter scheitert, ein anderes USB-Kabel oder einen anderen Anschluss probieren.",
	},
	"verify": {
		"The check after writing failed at 0x%x: the flash differs from the image. Try again with another USB cable or port; if it keeps failing, tick \"Erase whole flash\" under Advanced (--erase).",
		"Die Prüfung nach dem Schreiben ist bei 0x%x gescheitert: Der Flash weicht vom Image ab. Versuch es mit einem anderen USB-Kabel oder Anschluss noch einmal; scheitert es weiter, unter Erweitert „Ganzen Flash löschen“ ankreuzen (--erase).",
	},
	"no_image": {
		"This tentacle-flash has no firmware built in. Choose a .bin file (Advanced, or --image FILE).",
		"In dieses tentacle-flash ist keine Firmware eingebaut. Wähl eine .bin-Datei (Erweitert, oder --image DATEI).",
	},
	"read_image": {
		"Cannot read the firmware file %s (%s). Check the path and that the file is there.",
		"Die Firmware-Datei %s lässt sich nicht lesen (%s). Pfad prüfen und ob die Datei da ist.",
	},
	"bad_image": {
		"%s is not a Tentacle firmware image for this board: %s.",
		"%s ist kein Tentacle-Firmware-Image für dieses Board: %s.",
	},
	"bad_sig": {
		"The signature of %s does not match: the file was changed or does not come from Tentacle. Use the original file.",
		"Die Signatur von %s passt nicht: Die Datei wurde verändert oder stammt nicht von Tentacle. Nimm die Originaldatei.",
	},
	"busy": {
		"An installation is already running.",
		"Es läuft schon eine Installation.",
	},
	// warnings
	"no_psram": {
		"No embedded PSRAM listed in the chip's eFuses. Tentacle needs 8 MB PSRAM (the XIAO ESP32-S3 and the Waveshare ESP32-S3-ePaper-1.54 have it); on another board it may not start.",
		"In den eFuses des Chips ist kein eingebautes PSRAM eingetragen. Tentacle braucht 8 MB PSRAM (XIAO ESP32-S3 und Waveshare ESP32-S3-ePaper-1.54 haben es); auf einem anderen Board startet es womöglich nicht.",
	},
	"flash_unknown": {
		"The flash size could not be read; writing as if it were %s.",
		"Die Flash-Größe ließ sich nicht lesen; es wird geschrieben, als wären es %s.",
	},
	"unsigned": {
		"%s is not signed. Install only firmware you built yourself or got from a source you trust.",
		"%s ist nicht signiert. Installier nur Firmware, die du selbst gebaut hast oder aus einer Quelle hast, der du vertraust.",
	},
	"app_only": {
		"%s holds only the app. It goes to 0x10000, and the board must already have Tentacle's bootloader and partition table; for a new board use the .merged.bin.",
		"%s enthält nur die App. Sie kommt nach 0x10000, und das Board muss schon Bootloader und Partitionstabelle von Tentacle haben; für ein neues Board die .merged.bin nehmen.",
	},
	"no_setup": {
		"The board did not answer with its setup Wi-Fi. If it has an OLED, the name and password are shown there. Otherwise press RESET (R) or replug it and run tentacle-flash --info.",
		"Das Board hat sein Einrichtungs-WLAN nicht genannt. Mit OLED stehen Name und Passwort dort. Sonst RESET (R) drücken oder neu einstecken und tentacle-flash --info aufrufen.",
	},
}
