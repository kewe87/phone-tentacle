#pragma once
// The firmware's version, the same scheme as the repo's release tags (v0.1.0). Bump it for every firmware
// release. The board reports it as `fw` in its status and in its mDNS TXT record.
#define FW_VERSION "0.2.3"
// The same in a form `tentacle update` finds in an image file (ota.cpp keeps it in the firmware).
#define FW_MARKER "tentacle-fw:" FW_VERSION ";"

// The name the board shows in Screen Mirroring and to the CLI, each with " 1A2B" after it. A build may set its
// own: flash.sh --name NAME (one word). The setup hotspot is always Tentacle-1A2B.
#define BOARD_NAME_STR2(x) #x
#define BOARD_NAME_STR(x) BOARD_NAME_STR2(x)
#ifdef BOARD_NAME
#define BOARD_NAME_TEXT BOARD_NAME_STR(BOARD_NAME)
#else
#define BOARD_NAME_TEXT "Tentacle"
#endif
