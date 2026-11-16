// flashbuild builds tentacle-flash for every system into dist/: Windows (amd64), Linux (amd64, arm64) and macOS
// (one universal file for Apple silicon and Intel). Everything is pure Go (CGO_ENABLED=0), so it runs on any
// computer with Go, without a C compiler or Xcode.
//
//	cd cli
//	go run ./tools/flashbuild --image ../build/hid_bridge.ino.merged.bin --version 0.4.0
//
// --image puts that firmware into the binaries; without it they carry no firmware and need --image or the page's
// "Own firmware" at run time. Sign the merged image first with `tentacle sign` (the signature goes into the file)
// so the installer can check it.
package main

import (
	"bytes"
	"compress/gzip"
	"debug/macho"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/kewe87/phone-tentacle/cli/internal/fwsign"
)

const releaseDir = "internal/flash/release"

type target struct{ goos, goarch string }

func main() {
	image := flag.String("image", "", "firmware image to build in (a *.merged.bin, signed with tentacle sign)")
	version := flag.String("version", "dev", "version printed by tentacle-flash --version")
	out := flag.String("out", "dist", "output directory")
	flag.Parse()
	if err := build(*image, *version, *out); err != nil {
		fmt.Fprintln(os.Stderr, "flashbuild:", err)
		os.Exit(1)
	}
}

func build(image, version, out string) error {
	if _, err := os.Stat("cmd/tentacle-flash"); err != nil {
		return errors.New("run this in the cli directory")
	}
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	if image != "" {
		cleanup, err := embed(image, version)
		if err != nil {
			return err
		}
		defer cleanup()
	} else if n, _ := filepath.Glob(filepath.Join(releaseDir, "*.bin*")); len(n) == 0 {
		fmt.Println("note: no --image: the binaries carry no firmware")
	}
	ldflags := "-s -w -X main.version=" + version
	targets := []target{{"windows", "amd64"}, {"linux", "amd64"}, {"linux", "arm64"}, {"darwin", "arm64"}, {"darwin", "amd64"}}
	var mac []string
	for _, t := range targets {
		name := fmt.Sprintf("tentacle-flash_%s_%s", t.goos, t.goarch)
		if t.goos == "windows" {
			name += ".exe"
		}
		path := filepath.Join(out, name)
		cmd := exec.Command("go", "build", "-trimpath", "-ldflags", ldflags, "-o", path, "./cmd/tentacle-flash")
		cmd.Env = append(os.Environ(), "CGO_ENABLED=0", "GOOS="+t.goos, "GOARCH="+t.goarch)
		cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("%s/%s: %w", t.goos, t.goarch, err)
		}
		if t.goos == "darwin" {
			mac = append(mac, path)
			continue
		}
		report(path)
	}
	uni := filepath.Join(out, "tentacle-flash_darwin_universal")
	if err := fatMachO(uni, mac); err != nil {
		return err
	}
	for _, p := range mac {
		os.Remove(p)
	}
	report(uni)
	return nil
}

// embed copies the image into internal/flash/release for go:embed and returns a function that removes it again.
func embed(image, version string) (func(), error) {
	old, _ := fs.Glob(os.DirFS(releaseDir), "*.bin*")
	if len(old) > 0 {
		return nil, fmt.Errorf("%s already holds %s; remove it first", releaseDir, old[0])
	}
	data, err := os.ReadFile(image)
	if err != nil {
		return nil, err
	}
	base := "tentacle-" + version + ".merged.bin"
	if !strings.Contains(filepath.Base(image), "merged") {
		base = "tentacle-" + version + ".bin"
	}
	dst := filepath.Join(releaseDir, base)
	// gzip: the merged image is 8 MB, nearly all of it empty flash
	var gz bytes.Buffer
	zw, _ := gzip.NewWriterLevel(&gz, gzip.BestCompression)
	zw.Write(data)
	if err := zw.Close(); err != nil {
		return nil, err
	}
	files := []string{dst + ".gz"}
	if err := os.WriteFile(dst+".gz", gz.Bytes(), 0o644); err != nil {
		return nil, err
	}
	if _, _, signed := fwsign.Split(data); signed {
		// the signature is in the image itself (tentacle sign)
	} else if sig, err := os.ReadFile(image + fwsign.SigExt); err == nil { // an older, separately signed image
		if err := os.WriteFile(dst+fwsign.SigExt, sig, 0o644); err != nil {
			os.Remove(dst + ".gz")
			return nil, err
		}
		files = append(files, dst+fwsign.SigExt)
	} else {
		fmt.Printf("warning: %s is not signed: the built-in firmware is unsigned (sign it with `tentacle sign`)\n", image)
	}
	return func() {
		for _, f := range files {
			os.Remove(f)
		}
	}, nil
}

func report(path string) {
	if st, err := os.Stat(path); err == nil {
		fmt.Printf("%-48s %5.1f MB\n", path, float64(st.Size())/(1<<20))
	}
}

// fatMachO joins thin Mach-O files into one universal ("fat") file, as lipo -create does.
func fatMachO(out string, thin []string) error {
	const align = 14 // 16 KiB, as Apple silicon pages
	type slice struct {
		data         []byte
		cpu, sub     uint32
		offset, size uint32
	}
	var slices []slice
	off := uint32(1 << align)
	for _, p := range thin {
		data, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		f, err := macho.NewFile(bytes.NewReader(data))
		if err != nil {
			return fmt.Errorf("%s: %w", p, err)
		}
		slices = append(slices, slice{data: data, cpu: uint32(f.Cpu), sub: f.SubCpu, offset: off, size: uint32(len(data))})
		off += uint32(len(data))
		off = (off + 1<<align - 1) &^ (1<<align - 1)
	}
	var b bytes.Buffer
	be := binary.BigEndian
	b.Write(be.AppendUint32(nil, macho.MagicFat))
	b.Write(be.AppendUint32(nil, uint32(len(slices))))
	for _, s := range slices {
		for _, v := range []uint32{s.cpu, s.sub, s.offset, s.size, align} {
			b.Write(be.AppendUint32(nil, v))
		}
	}
	for _, s := range slices {
		b.Write(make([]byte, int(s.offset)-b.Len()))
		b.Write(s.data)
	}
	if err := os.WriteFile(out, b.Bytes(), 0o755); err != nil {
		return err
	}
	// check that Go reads it back as a universal file with both slices
	ff, err := macho.OpenFat(out)
	if err != nil {
		return fmt.Errorf("universal file: %w", err)
	}
	defer ff.Close()
	if len(ff.Arches) != len(thin) {
		return fmt.Errorf("universal file has %d slices, want %d", len(ff.Arches), len(thin))
	}
	return nil
}
