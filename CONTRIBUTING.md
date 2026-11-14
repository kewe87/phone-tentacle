# Contributing

Issues and pull requests are welcome. Keep changes small and focused, and match the surrounding code.

Start from the latest `main` of this fork and open pull requests against `main`. Before you open a
pull request, update your branch from the latest `main` (fetch and rebase, or merge in the latest `main`).

Before sending a change, run the checks for what you touched:

```bash
# firmware (host tests need mbedtls 3: apt libmbedtls-dev / brew mbedtls@3)
hardware/flash.sh --build-only && make -C hardware/tests
# CLI
cd cli && go build ./... && go vet ./... && go test ./...
```

Never commit secrets: `hardware/hid_bridge/secrets.h` is git-ignored on purpose.

Code under `hardware/hid_bridge/src/airplay/` is GPL-3.0 (ported from UxPlay); contributions there are GPL-3.0 too.
