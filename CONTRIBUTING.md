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

## Licenses and origin

- Tentacle is derived from [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry
  Yuan (MIT). `LICENSE` and the original section of `NOTICE` stay unchanged; contributions are MIT.
- Code under `hardware/hid_bridge/src/airplay/` is GPL-3.0 (ported from UxPlay); contributions there are GPL-3.0
  too, and the file headers stay.
