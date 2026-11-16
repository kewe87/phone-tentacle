tentacle-flash builds the firmware in this folder into itself (go:embed).

Put one image here before building, with its signature next to it:

    tentacle-<version>.merged.bin
    tentacle-<version>.merged.bin.sig    (tentacle sign --key KEY FILE)

`go run ./tools/flashbuild --image PATH/hid_bridge.ino.merged.bin` copies them in, builds all platforms into
dist/ and takes them out again. Images in this folder are git-ignored; without one, tentacle-flash asks for a
file (--image FILE, or "Advanced" on its page).
