// Build switches. flash.sh and the release build force-include this file into every C/C++ file (`-include`), so
// every source file sees the same values whether or not a switch was given on the command line.
//
// One release image serves every board (0.4.0): what used to be a switch is a setting on the board now (NVS, see
// src/setup/board_opts.h; `tentacle device set`, the web interface's settings): the status OLED (found on I2C by
// itself), the absolute pointer, the battery pin and its divider, the OLED's language, the name. The release image
// is built without any -D flag; TENTACLE_DEBUG=1 makes the separate debug image, never handed out.
#pragma once

// 1 (default) makes a short press of the BOOT button the stop toggle and the LED the stop/heartbeat light
// (button.h); 0 (flash.sh --no-safety) builds without. Stays a build switch on purpose: a setting could be turned
// off over the network, the toggle is the person at the board's. The button's other jobs stay either way:
// confirming a request, and forgetting the Wi-Fi when held 10 s.
#ifndef SAFETY_CONTROLS
#define SAFETY_CONTROLS 1
#endif

// Build switch for the bring-up: 1 (flash.sh --debug) adds the serial debug interface (dbg.h) and the older serial
// echo (DEBUG_SERIAL). Never hand out such a build: `dbg btn` presses BOOT from the serial port.
#ifndef TENTACLE_DEBUG
#define TENTACLE_DEBUG 0
#endif
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL TENTACLE_DEBUG
#endif
