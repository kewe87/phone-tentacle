# Security

Tentacle drives a real phone, so bugs can have real effects. Please report security issues privately through
GitHub's "Report a vulnerability" (Security tab of `kewe87/phone-tentacle`) instead of a public issue. Include
steps to reproduce and the commit you tested.

In scope:

- the board's network services: the CLI link (TCP 7202: pairing, sessions, sealed frames), the AirPlay receiver
  (TCP 7000 and its data ports), mDNS, and the setup hotspot with its page (port 80, only while the board is being
  set up);
- anything that lets someone pair, change the board's Wi-Fi or send input **without a person pressing the BOOT
  button** where the design asks for one (the pairing window, the confirmation press, the setup page);
- anything that gets past the stop toggle;
- the `tentacle` CLI and its MCP server on the computer (keys in the config directory, the daemon's local socket).

How it is meant to hold, and what is left open: [docs/hardening.md](docs/hardening.md).

Tentacle is derived from ESP32 iPhone Agent by Henry Yuan. Issues in this repository's code belong here, not with
the original project.
