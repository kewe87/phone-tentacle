#pragma once
// The firmware's version, the same scheme as the repo's release tags (v0.2.0). Bump it for every firmware
// release. The board reports it as `fw` in its status and in its mDNS TXT record.
#define FW_VERSION "0.2.5"

// The name the board shows in Screen Mirroring, and to the CLI with " 1A2B" after it. A build may set its
// own: flash.sh --name NAME (one word). The setup hotspot is always ESP32Agent-1A2B.
#define BOARD_NAME_STR2(x) #x
#define BOARD_NAME_STR(x) BOARD_NAME_STR2(x)
#ifdef BOARD_NAME
#define BOARD_NAME_TEXT BOARD_NAME_STR(BOARD_NAME)
#else
#define BOARD_NAME_TEXT "ESP32 Agent"
#endif
