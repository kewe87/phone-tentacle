#!/usr/bin/env bash
# Build and flash the board (needs arduino-cli and the esp32 core 3.3.x).
# Usage: hardware/flash.sh [port] [--debug] [--sta] [--build-only]
#   --debug       bring-up build (TENTACLE_DEBUG): time-stamped state log and `dbg` commands on USB serial, next to
#                 the command and AirPlay echo. Never hand out such a build (`dbg btn` presses BOOT from serial)
#   --sta         test build: join the Wi-Fi in hid_bridge/secrets.h instead of opening a hotspot
#   --build-only  compile only (prints flash/RAM usage)
#   --info        no build: ask a running board for its setup Wi-Fi (name + password) over USB serial
#   --name NAME   the board's name in Screen Mirroring and for the CLI (one word; default Tentacle), shown with the
#                 board's suffix: "Tentacle 1A2B". The setup Wi-Fi is always Tentacle-1A2B
# Safety is on by default: BOOT button = stop (suppresses all HID output) + status LED,
# and only one station may join the setup Wi-Fi. See docs/hardening.md. To opt out:
#   --no-safety     build without the stop toggle / status light (BOOT still confirms requests, and held 10 s
#                   forgets the Wi-Fi; the LED only blinks while a confirmation waits)
#   --multi-client  let up to 4 stations join the setup Wi-Fi
#   (--safety / --harden are still accepted; they are the default now)
# One image serves every board: the status OLED (found on I2C by itself), the absolute or relative pointer, the
# battery pin and the OLED's language are settings on the board (`tentacle device set`, or the web interface's
# settings), no longer build options. --abs, --display and --battery-pin are accepted and ignored.
# The board is driven by the tentacle CLI over Wi-Fi only (TCP 7202): the firmware has no Bluetooth and needs the
# esp32 core and the U8g2 library (`arduino-cli lib install U8g2`). The build switches live in
# hid_bridge/build_cfg.h, force-included into every C/C++ file below.
# A running bridge is a TinyUSB device, so it is first kicked into the ROM
# bootloader with a 1200-baud touch; a blank board is flashed directly.
# After flashing, the board is asked for its setup Wi-Fi (name + password; also on its OLED, if it has one):
# join it, choose your Wi-Fi on the page that opens, press BOOT on the board. BOOT held 10 s starts over.
set -euo pipefail
cd "$(dirname "$0")"
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB"
ports() { for p in /dev/cu.usbmodem* /dev/ttyACM*; do [[ -e $p ]] && echo "$p"; done; return 0; }
PORT="" DEFS="" BUILD_ONLY=0 INFO=0 NAME_NEXT=0 BAT_NEXT=0
for a in "$@"; do
  if (( BAT_NEXT )); then
    [[ $a =~ ^[0-9]+$ ]] || { echo "--battery-pin takes a GPIO number"; exit 1; }
    echo "--battery-pin is a setting now: after flashing, tentacle device set battery $a"; BAT_NEXT=0; continue
  fi
  if (( NAME_NEXT )); then
    [[ $a =~ ^[A-Za-z0-9]+$ ]] || { echo "--name takes one word of letters and digits"; exit 1; }
    DEFS+=" -DBOARD_NAME=$a"; NAME_NEXT=0; continue
  fi
  case $a in
    --name) NAME_NEXT=1 ;;
    --debug) DEFS+=" -DTENTACLE_DEBUG=1" ;;  # the one debug switch; DEBUG_SERIAL follows it (build_cfg.h)
    --sta) [[ -f hid_bridge/secrets.h ]] || { echo "--sta needs hid_bridge/secrets.h (see secrets.example.h)"; exit 1; }
           DEFS+=" -DSTA_FROM_SECRETS=1" ;;
    --build-only) BUILD_ONLY=1 ;;
    --info) INFO=1 ;;
    --abs) echo "--abs is ignored: the absolute pointer is the default (tentacle device set abs on|off)" ;;
    --safety|--harden) ;;  # default now
    --no-safety) DEFS+=" -DSAFETY_CONTROLS=0" ;;
    --multi-client) DEFS+=" -DAP_MAX_CLIENTS=4" ;;
    --display) echo "--display is ignored: the board finds its OLED by itself (tentacle device set display ...)" ;;
    --battery-pin) BAT_NEXT=1 ;;
    --small-gop) DEFS+=" -DGOP_CAP_KB=256" ;;  # test build: the GOP fills (freezes) within minutes
    *) PORT=$a ;;
  esac
done

# setup_line PORT: the board's answer to `0 setup` on USB serial (netcfg.h: wifi-setup {...}); fails after ~3 s.
setup_line() {
  local p=$1 line end
  [[ -e $p ]] || return 1
  stty -f "$p" 115200 raw -echo 2>/dev/null || stty -F "$p" 115200 raw -echo 2>/dev/null || return 1
  { exec 3<>"$p"; } 2>/dev/null || return 1
  printf '0 setup\n' >&3 2>/dev/null || { exec 3>&-; return 1; }
  end=$((SECONDS + 3))
  while (( SECONDS < end )); do
    if IFS= read -r -t 1 line <&3; then
      line=${line%$'\r'}
      if [[ $line == "wifi-setup "* ]]; then exec 3>&-; echo "$line"; return 0; fi
    fi
  done
  exec 3>&-
  return 1
}

# show_qr ROWS: prints the setup Wi-Fi's QR code (the `qr` field: one hex string per row, first module in the top
# bit) with half blocks, dark modules on a white background and a 4-module quiet zone, so a phone camera can scan it
# from a dark or light terminal.
show_qr() {
  local IFS=, rows=() bits=() r h i x y n line t b
  local up=$'\342\226\200' down=$'\342\226\204' full=$'\342\226\210' on=$'\033[30;107m' off=$'\033[0m'
  read -r -a rows <<<"$1"
  n=${#rows[@]}
  (( n >= 21 )) || return 0
  for r in "${rows[@]}"; do
    line=""
    for (( i = 0; i < ${#r}; i++ )); do
      h=$((16#${r:i:1}))
      line+=$(( h >> 3 & 1 ))$(( h >> 2 & 1 ))$(( h >> 1 & 1 ))$(( h & 1 ))
    done
    bits+=("$line")
  done
  for (( y = -4; y < n + 4; y += 2 )); do
    line=""
    for (( x = -4; x < n + 4; x++ )); do
      t=0 b=0
      if (( x >= 0 && x < n )); then
        (( y >= 0 && y < n )) && t=${bits[y]:x:1}
        (( y + 1 >= 0 && y + 1 < n )) && b=${bits[y + 1]:x:1}
      fi
      if (( t && b )); then line+=$full; elif (( t )); then line+=$up; elif (( b )); then line+=$down; else line+=" "; fi
    done
    printf '  %s%s%s\n' "$on" "$line" "$off"
  done
}

# show_setup SECONDS PORT...: asks until one of the ports answers, then prints how to set the Wi-Fi up. A board that
# says nothing (still in the bootloader, no data cable) only costs the timeout.
show_setup() {
  local wait=$1 line="" p end field
  shift
  echo "Asking the board for its setup Wi-Fi (up to $wait s)..."
  end=$((SECONDS + wait))
  while (( SECONDS < end )) && [[ -z $line ]]; do
    for p in "$@" $(ports); do
      line=$(setup_line "$p") && break
      line=""
    done
    [[ -n $line ]] || sleep 1
  done
  if [[ -z $line ]]; then
    echo "No answer. If the board is still in download mode, press RESET (or replug it), then: hardware/flash.sh --info"
    return 0
  fi
  field() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" <<<"$line"; }
  if [[ $line == *'"setup":1'* ]]; then
    echo
    echo "  Setup Wi-Fi : $(field ssid)"
    echo "  Password    : $(field pass)"
    echo
    if [[ -n $(field qr) ]]; then
      show_qr "$(field qr)"
      echo
      echo "Scan this code with the phone's camera to join without typing the password."
    fi
    echo "Join that Wi-Fi with a phone or laptop. The setup page opens by itself (or open $(field url))."
    echo "Choose your home Wi-Fi, enter its password, then press the BOOT button on the board within 30 s."
  else
    echo "The board already has a Wi-Fi stored: '$(field ssid)' (address $(field ip))."
    echo "To set it up again: hold the BOOT button for 10 s; the setup Wi-Fi comes back."
  fi
}

if (( INFO )); then
  show_setup 8 $PORT
  exit 0
fi
[[ $PWD != *" "* ]] || { echo "the -include path below cannot contain spaces: $PWD"; exit 1; }
DEFS+=" -include $PWD/hid_bridge/build_cfg.h"
FLAGS=(--build-property "compiler.cpp.extra_flags=$DEFS" --build-property "compiler.c.extra_flags=$DEFS")

arduino-cli compile -b "$FQBN" "${FLAGS[@]}" --output-dir build hid_bridge
(( BUILD_ONLY )) && exit 0
BEFORE=$(ports)
PORT=${PORT:-$(head -n 1 <<<"$BEFORE")}
[[ -n $PORT ]] || { echo "no board found (looked for /dev/cu.usbmodem* and /dev/ttyACM*)"; exit 1; }
stty -f "$PORT" 1200 2>/dev/null || stty -F "$PORT" 1200 2>/dev/null || true
# The ROM bootloader may come back under a new name. Follow the port that appeared after the
# reset rather than the first one listed, so other connected boards are never picked up.
new_ports() { comm -13 <(sort <<<"$BEFORE") <(ports | sort); }
for _ in $(seq 20); do sleep 0.5; [[ -n $(new_ports) ]] && break; done
sleep 1  # let the ROM bootloader's serial come up
NEW=$(new_ports)
case $(grep -c . <<<"$NEW" || true) in
  0) [[ -e $PORT ]] || { echo "$PORT is gone after the reset and no new port appeared"; exit 1; } ;;
  1) PORT=$NEW ;;
  *) echo "several ports appeared after the reset, not guessing:" $NEW; exit 1 ;;
esac
arduino-cli upload -b "$FQBN" -p "$PORT" --input-dir build hid_bridge
show_setup 20 "$PORT"
