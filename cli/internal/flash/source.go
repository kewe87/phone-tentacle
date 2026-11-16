package flash

import (
	"bytes"
	"compress/gzip"
	"context"
	"embed"
	"io"
	"io/fs"
	"path"
	"sort"
	"strings"
)

// Source provides a firmware image. tentacle-flash uses the first source that has one: a file given with --image
// or picked on the page, else the image built into the program (Embedded).
//
// Further sources plug in here. One that fetches the newest signed release from GitHub would implement Source
// with Fetch downloading the release's .merged.bin, signature inside (ParseImage then checks the signature as
// for any other source) and be put in front of Embedded in cmd/tentacle-flash. The repository is private for now,
// so there is no such source yet.
type Source interface {
	// Label names the source for people ("built in", a file name).
	Label() string
	// Fetch returns the image's name, its bytes (the signature usually inside, fwsign.Embed) and a separate
	// signature (nil if there is none or it is inside the bytes). It returns
	// ErrNoImage if the source has nothing to offer.
	Fetch(ctx context.Context) (name string, data, sig []byte, err error)
}

// ErrNoImage: a source has no image (the program was built without one).
var ErrNoImage = msg("no_image", nil)

// Busy: an installation or chip check is already running.
var Busy = msg("busy", nil)

// Open fetches an image from a source and checks it.
func Open(ctx context.Context, s Source) (*Image, error) {
	name, data, sig, err := s.Fetch(ctx)
	if err != nil {
		return nil, err
	}
	return ParseImage(name, data, sig)
}

//go:embed release
var releaseFS embed.FS

// Embedded is the image built into the program from internal/flash/release (see README.txt there).
type Embedded struct{}

func (Embedded) Label() string { return "built in" }

func (Embedded) Fetch(context.Context) (string, []byte, []byte, error) {
	return embeddedFrom(releaseFS, "release")
}

// embeddedFrom picks the last NAME.bin or NAME.bin.gz in dir (gzip keeps the 8 MB merged image, mostly empty
// flash, small in the program). The signature is inside the image; a legacy NAME.bin.sig is taken too.
func embeddedFrom(fsys fs.FS, dir string) (string, []byte, []byte, error) {
	entries, err := fs.ReadDir(fsys, dir)
	if err != nil {
		return "", nil, nil, ErrNoImage
	}
	var files []string
	for _, e := range entries {
		if n := e.Name(); !e.IsDir() && (strings.HasSuffix(n, ".bin") || strings.HasSuffix(n, ".bin.gz")) {
			files = append(files, n)
		}
	}
	if len(files) == 0 {
		return "", nil, nil, ErrNoImage
	}
	sort.Strings(files)
	file := files[len(files)-1]
	data, err := fs.ReadFile(fsys, path.Join(dir, file))
	if err != nil {
		return "", nil, nil, err
	}
	name := strings.TrimSuffix(file, ".gz")
	if name != file {
		zr, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			return "", nil, nil, msg("bad_image", err, name)
		}
		if data, err = io.ReadAll(io.LimitReader(zr, 64<<20)); err != nil {
			return "", nil, nil, msg("bad_image", err, name)
		}
	}
	sig, err := fs.ReadFile(fsys, path.Join(dir, name+".sig"))
	if err != nil {
		sig = nil
	}
	return name, data, sig, nil
}

// File is an image file on disk, signed inside (or, from older releases, with FILE.sig next to it).
type File struct{ Path string }

func (f File) Label() string { return f.Path }

func (f File) Fetch(context.Context) (string, []byte, []byte, error) {
	im, err := readFile(f.Path)
	return im.name, im.data, im.sig, err
}

// Bytes is an image held in memory (one uploaded on the page).
type Bytes struct {
	Name      string
	Data, Sig []byte
}

func (b Bytes) Label() string { return b.Name }

func (b Bytes) Fetch(context.Context) (string, []byte, []byte, error) {
	return b.Name, b.Data, b.Sig, nil
}
