// Build switches. flash.sh / flash.ps1 force-include this file into every C/C++ file (`-include`), so every source
// file sees the same values whether or not a switch was given on the command line.
#pragma once

// 1 (default) makes a short press of the BOOT button the stop toggle and the LED the stop/heartbeat light
// (button.h); 0 (flash.sh --no-safety) builds without. The button's other jobs stay either way: confirming a
// request, and forgetting the Wi-Fi when held 10 s.
#ifndef SAFETY_CONTROLS
#define SAFETY_CONTROLS 1
#endif

// Build switch for the bring-up: 1 (flash.sh --debug / flash.ps1 -Debug) adds the serial debug interface (dbg.h)
// and the older serial echo (DEBUG_SERIAL). Never hand out such a build: `dbg btn` presses BOOT from the serial port.
#ifndef TENTACLE_DEBUG
#define TENTACLE_DEBUG 0
#endif
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL TENTACLE_DEBUG
#endif
