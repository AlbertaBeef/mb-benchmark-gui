#!/usr/bin/env bash
# Make Axelera Metis core temperatures readable.
#
# Two independent gates have to be cleared, and BOTH are lost on every reboot
# because the card's runtime firmware is RAM-only — nothing loads it at boot:
#
#   1. App firmware. Idle after a reboot the Metis sits in bootloader firmware
#      and reports something like `v1.3.2+bl1`; axcmd/triton_trace then refuse
#      with a version mismatch. **This script does NOT load it** — see the long
#      note at the gate-1 check. Running any model loads it safely through the
#      runtime; loading it here with `axcmd --fwload` breaks inference until a
#      power cycle. So: run a model first, then run this.
#   2. Collector log level. The firmware only emits `core_temps=[...]` at the
#      `inf` level; the default is `err`, so the line never appears. This one is
#      passive and safe to set at any time, and is all this script now does.
#
# Exit status: 0 either way — collector set, or card still in bootloader with
# nothing to do until something runs a model. Neither is a failure.
#
# Idempotent: safe to run repeatedly, and a no-op once both gates are clear.
set -uo pipefail

log() { echo "axelera-temps: $*"; }

# --- locate the runtime and the firmware image (newest install wins) ---
AXBIN=$(ls -d /opt/axelera/runtime-*/bin 2>/dev/null | sort -V | tail -1)
FW=$(ls /opt/axelera/device-*/omega/bin/start_axelera_runtime.elf 2>/dev/null | sort -V | tail -1)
[ -n "${AXBIN:-}" ] || { log "no /opt/axelera/runtime-*/bin — nothing to do"; exit 0; }

# --- wait for the device node; at boot the PCIe setup may not have run yet ---
DEV=""
NODE=""
for _ in $(seq 1 30); do
    NODE=$(ls -d /dev/metis-* 2>/dev/null | head -1)
    if [ -n "$NODE" ]; then DEV=$(basename "$NODE"); break; fi
    sleep 1
done
[ -n "$DEV" ] || { log "no /dev/metis-* after 30s — card absent?"; exit 0; }
log "device $DEV"

# --- gate 1: app firmware — REPORTED, NEVER LOADED HERE ---
# This script used to run `axcmd --fwload` when it found a card in bootloader.
# Do not put that back. Measured repeatedly on 2026-08-07: firmware uploaded that
# way leaves the card in a state where inference fails on the SECOND frame with
#   [ERROR][waitForQueueBinaryCompletion]: Wait kernel failed with return code -1433
#   [ERROR][axeCommandQueueExecuteCommandLists]: Failed with error code: 1879048193
# (0x70000001 = ZE_RESULT_ERROR_DEVICE_LOST), with no DMA errors and no link drop
# — the card looks perfectly healthy and every model still refuses to run. Only a
# power cycle clears it; a driver reload does not, because card RAM survives it.
#
# The runtime's own load path is fine: axr_device_connect() loads the firmware on
# a cold card, and every benchmark that let it do so has worked. The evidence is
# one-sided — every boot where this script uploaded, inference failed; the one
# boot where the runtime loaded it, four consecutive runs passed.
#
# So the correct order is: run a model FIRST (which loads the firmware), then run
# this script for temperatures. On a headless boot with nothing to run a model,
# temperatures simply are not available yet, and saying so is better than
# uploading firmware that breaks the card for inference.
#
# Capture first, then match. Piping into `grep -q` under `set -o pipefail` is a
# trap: grep exits at the first match, the producer takes SIGPIPE, and the
# pipeline reports failure — so the match would read as "no match".
fwver=$("$AXBIN/axcmd" --device "$DEV" --fwver 2>&1)
if grep -qi "mismatch\|bl1" <<<"$fwver"; then
    log "card is in BOOTLOADER — not loading firmware from here, on purpose."
    log "  Run any model first (the runtime loads the firmware safely), then"
    log "  re-run this script for temperatures."
    log "  Uploading with 'axcmd --fwload' leaves inference broken until a power"
    log "  cycle — see the comment above this check, and Known issues in README."
    log "firmware: $(tr -d '\n' <<<"$fwver")"
    # Exit 0, not an error code: "no firmware yet" is a legitimate state, not a
    # failure. The boot service is Type=oneshot, so a non-zero exit would leave
    # the unit permanently red on every cold boot — which is exactly the state
    # this script can no longer do anything about.
    exit 0
fi
log "firmware: $("$AXBIN/axcmd" --device "$DEV" --fwver 2>&1 | tr -d '\n')"

# --- gate 2: collector verbosity ---
# NB this is a *global* device log level, and it is volatile: it resets on a
# reboot AND on a driver reload (modprobe -r/-i metis), so a mid-session
# `modprobe` cycle silently takes temperatures away again. Re-run this script.
# The GUI's probe deliberately never sets it — it stays passive and reports
# "collector idle" instead of changing device state behind the user's back.
# Setting it here is the explicit opt-in.
"$AXBIN/triton_trace" --device "$DEV" --slog-level inf:collector >/dev/null 2>&1
sleep 2

peek=$("$AXBIN/triton_trace" --device "$DEV" --slog --peek 2>&1)
if grep -q "core_temps" <<<"$peek"; then
    log "core_temps flowing"
else
    log "collector enabled but no core_temps yet (it appears within a second or two)"
fi

# Temperatures flowing says NOTHING about whether inference works — they are
# separate paths, and this script has reported success on a card that could not
# run a single model. If you need to know the card is healthy, run a benchmark.
log "note: this checks temperatures only; it does not verify that inference runs"
