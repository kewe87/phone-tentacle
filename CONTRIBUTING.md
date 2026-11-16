# Contributing

Issues and pull requests are welcome. Keep changes small and focused, and match the surrounding code.

Start from the latest `main` and open pull requests against `main`. Before you open one, update your branch from
the latest `main` (fetch and rebase, or merge it in).

Before sending a change, run the checks for what you touched:

```bash
# firmware (arduino-cli with the esp32 core 3.3.x; host tests need mbedtls 3 and libsodium: apt libmbedtls-dev libsodium-dev / brew mbedtls@3 libsodium)
hardware/flash.sh --build-only && make -C hardware/tests
# simulated board and its protocol test (needs ffmpeg and uv)
make -C hardware/tests/sim cli_sim screen.h264 && uv run hardware/tests/sim/test_cli_sim.py
# CLI
cd cli && go build ./... && go vet ./... && go test ./...
```

A change to the pairing or session crypto has to change the firmware (`hardware/hid_bridge/src/cli/`), the CLI
(`cli/internal/proto/`), `docs/cli-protocol.md` and the test vectors together
(`cd cli/testdata && uv run gen_vectors.py`, then the values in `hardware/tests/test_main.c`).

Never commit secrets: `hardware/hid_bridge/secrets.h` is git-ignored on purpose.

## Releasing

Releases are built by `.github/workflows/release.yml`; nobody uploads files by hand.

1. Bump `FW_VERSION` in `hardware/hid_bridge/version.h` (`X.Y.Z`) and add the version's section to `CHANGELOG.md`.
   Merge that to `main`.
2. Optional dry run: Actions → Release → "Run workflow". It builds and signs everything (unsigned, with a warning,
   if the secret is missing) and keeps the files as the workflow artifact `release-X.Y.Z`; it creates no release.
3. Tag the commit on `main` and push the tag: `git tag vX.Y.Z && git push origin vX.Y.Z`. The tag must be exactly
   `v` + `FW_VERSION`, or the workflow stops.
4. The workflow creates a **draft** release: `tentacle-fw-X.Y.Z.bin` + `.sig` (network update),
   `tentacle-fw-X.Y.Z.merged.bin` + `.sig` (whole flash image for USB), `tentacle-flash_*` and `tentacle_*` per
   system (zip for Windows, tar.gz otherwise) and `SHA256SUMS`. Check it, write the notes from the changelog,
   publish it.

The release firmware is built without any `-D` switch (only `-include build_cfg.h`); debug builds
(`TENTACLE_DEBUG=1`) are never released.

**Signing key.** The repository secret `TENTACLE_SIGNING_KEY` holds the private release key: the content of the
`tentacle-release.key` file that `tentacle keygen DIR` writes (one line, 64 hex digits, the Ed25519 seed). Set it
from the key file without printing it, for example with the GitHub CLI reading the file on standard input:
`gh secret set TENTACLE_SIGNING_KEY -R kewe87/phone-tentacle < /path/to/tentacle-release.key` (or paste the line
under Settings → Secrets and variables → Actions). The workflow writes it to a temporary 0600 file only for
`tentacle sign` and deletes it afterwards. The key's public half must be the one in
`hardware/hid_bridge/ota_pubkey.h` and `fwsign.ReleaseKey` (`cli/internal/fwsign`): the workflow checks both
signatures against `ota_pubkey.h` and refuses a tag signed with another key. Never commit the key file or paste
it into an issue, a log or a chat.

## Licenses and origin

- Tentacle is derived from [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry
  Yuan (MIT). `LICENSE` and the original section of `NOTICE` stay unchanged; contributions are MIT.
- Code under `hardware/hid_bridge/src/airplay/` is GPL-3.0 (ported from UxPlay); contributions there are GPL-3.0
  too, and the file headers stay.
