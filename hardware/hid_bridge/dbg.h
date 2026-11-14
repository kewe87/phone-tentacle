// Bring-up debugging over USB serial, only in a TENTACLE_DEBUG build (flash.sh --debug / flash.ps1 -Debug).
// To be removed again after the first hardware tests: the list of places is in PROGRESS.md, "Phase DBG".
//   DBG("fmt", ...)   one line "[sec.ms] D text" on USB serial (not in the RAM log)
//   dbg_begin()       boot report: reset reason, the last crash
//   dbg_poll()        loop(): logs every state change, and heap / PSRAM / stack low-water marks every 10 s
//   dbg_command()     a serial line "dbg state|nvs|wifi [scan]|btn short|btn long|btn double|reboot|forget"
// A debug build must never be handed out: `dbg btn` presses the BOOT button from the serial port.
#pragma once

#ifndef TENTACLE_DEBUG
#define TENTACLE_DEBUG 0
#endif

#if TENTACLE_DEBUG
void dbg_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dbg_begin(const char *reset, const char *crash);
void dbg_poll();
bool dbg_command(const char *line);  // true: it was a dbg line (answered on serial)
#define DBG(...) dbg_log(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif
