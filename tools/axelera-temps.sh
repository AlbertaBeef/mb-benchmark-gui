#!/usr/bin/env bash
# Make Axelera Metis core temperatures readable.
#
# Two independent gates have to be cleared, and BOTH are lost on every reboot
# because the card's runtime firmware is RAM-only — nothing loads it at boot:
#
#   1. App firmware. Idle after a reboot the Metis sits in bootloader firmware
#      and axcmd/triton_trace refuse with
#          Version mismatch! Actual="v1.3.2+bl1" Expected="v1.7.0"
#      Running any inference loads it; so does `axcmd --fwload` (RAM, *not*
#      --flashload, which would write flash).
#   2. Collector log level. The firmware only emits `core_temps=[...]` at the
#      `inf` level; the default is `err`, so the line never appears.
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
for _ in $(seq 1 30); do
    node=$(ls -d /dev/metis-* 2>/dev/null | head -1)
    if [ -n "$node" ]; then DEV=$(basename "$node"); break; fi
    sleep 1
done
[ -n "$DEV" ] || { log "no /dev/metis-* after 30s — card absent?"; exit 0; }
log "device $DEV"

# --- gate 1: app firmware ---
# A bootloader-only card answers --fwver with a version-mismatch complaint
# rather than a plain version string.
# Capture first, then match. Piping into `grep -q` under `set -o pipefail` is a
# trap: grep exits at the first match, the producer takes SIGPIPE, and the
# pipeline reports failure — so the match would be read as "no match" and the
# firmware would silently never load.
fwver=$("$AXBIN/axcmd" --device "$DEV" --fwver 2>&1)
if grep -qi "mismatch\|bl1" <<<"$fwver"; then
    if [ -n "${FW:-}" ]; then
        log "bootloader firmware detected — loading $FW into RAM"
        "$AXBIN/axcmd" --device "$DEV" --fwload "$FW" >/dev/null 2>&1
        sleep 2
    else
        log "bootloader firmware, but no start_axelera_runtime.elf found"
    fi
fi
log "firmware: $("$AXBIN/axcmd" --device "$DEV" --fwver 2>&1 | tr -d '\n')"

# --- gate 2: collector verbosity ---
# NB this is a *global* device log level. Leaving it at inf makes the card's log
# ring busier, which is why the GUI's probe never sets it itself — it stays
# passive and reports "collector idle" instead. Setting it here is the explicit
# opt-in.
"$AXBIN/triton_trace" --device "$DEV" --slog-level inf:collector >/dev/null 2>&1
sleep 2

peek=$("$AXBIN/triton_trace" --device "$DEV" --slog --peek 2>&1)
if grep -q "core_temps" <<<"$peek"; then
    log "core_temps flowing"
else
    log "collector enabled but no core_temps yet (it appears within a second or two)"
fi
