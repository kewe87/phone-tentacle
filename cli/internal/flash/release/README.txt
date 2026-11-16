tentacle-flash builds the firmware in this folder into itself (go:embed).

Put one image here before building, signed (tentacle sign --key KEY FILE puts the signature into the file):

    tentacle-<version>.merged.bin

`go run ./tools/flashbuild --image PATH/hid_bridge.ino.merged.bin` copies it in, builds all platforms into
dist/ and takes them out again. Images in this folder are git-ignored; without one, tentacle-flash asks for a
file (--image FILE, or "Advanced" on its page).
