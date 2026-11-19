#!/usr/bin/env bash
# Build and flash the mb-probe-ra6m5 firmware onto a SparkFun Thing Plus RA6M5.
#
#   tools/flash-ra6m5.sh [--build-only] [sketch-dir]
#
# Hands-off: if the board is running a sketch (1b4f:0037) it is kicked into the
# DFU bootloader (1b4f:0036) with the 1200-baud touch, which the core
# implements in SerialUSB.cpp but SparkFun's boards.txt does not enable. If no
# sketch answers (first flash, crashed firmware), double-tap RST — the blue
# STAT LED fades slowly in the bootloader — and this waits up to 60 s for it.
#
# Needs: arduino-cli (~/.local/bin), the official arduino:renesas_portenta core
# for its tools, SparkFun's ported core under ~/Arduino/hardware/sparkfun/, and
# tools/99-thingplus-ra6m5.rules installed so dfu-util may open the bootloader.
set -euo pipefail

FQBN=sparkfun:renesas_portenta:thingplus_ra6m5
here=$(cd "$(dirname "$0")/.." && pwd)
build_only=0
[[ ${1:-} == --build-only ]] && { build_only=1; shift; }
sketch=${1:-$here/firmware/mb-probe-ra6m5}
export PATH=$HOME/.local/bin:$PATH

arduino-cli compile --fqbn "$FQBN" "$sketch"
(( build_only )) && exit 0

in_dfu() { lsusb -d 1b4f:0036 >/dev/null 2>&1; }

if ! in_dfu; then
    port=$(ls /dev/serial/by-id/*Thing_Plus_RA6M5* 2>/dev/null | head -1 || true)
    if [[ -n $port ]]; then
        echo "1200-baud touch on $port"
        stty -F "$port" 1200 hupcl
        # Opening and closing at 1200 baud with HUPCL drops DTR on close.
        : < "$port" || true
    else
        echo "No running sketch found — double-tap RST on the board."
    fi
fi

for _ in $(seq 1 120); do in_dfu && break; sleep 0.5; done
in_dfu || { echo "Bootloader (1b4f:0036) never appeared." >&2; exit 1; }

arduino-cli upload --fqbn "$FQBN" "$sketch" 2>&1 | tr '\r' '\n' | grep -v '^Download'
