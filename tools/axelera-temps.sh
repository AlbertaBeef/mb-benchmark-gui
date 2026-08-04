#!/usr/bin/env bash
# Make Axelera Metis core temperatures readable.
#
# Two independent gates have to be cleared, and BOTH are lost on every reboot
# because the card's runtime firmware is RAM-only — nothing loads it at boot:
#
#   1. App firmware. Idle after a reboot the Metis sits in bootloader firmware
#      and reports something like `v1.3.2+bl1-stage0`; axcmd/triton_trace then
#      refuse with a version mismatch. **Running any inference loads it** — that
#      is the normal path, and on a desktop it is the preferred one. `axcmd
#      --fwload` (RAM, *not* --flashload, which would write flash) exists for
#      the headless/at-boot case where nothing is going to run a model.
#   2. Collector log level. The firmware only emits `core_temps=[...]` at the
#      `inf` level; the default is `err`, so the line never appears. This one is
#      passive and safe to set at any time.
#
# Gate 2 alone is enough whenever something has already loaded the firmware.
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

# --- gate 1: app firmware ---
# A bootloader-only card answers --fwver with a version-mismatch complaint or a
# `bl1` build string rather than a plain runtime version.
# Capture first, then match. Piping into `grep -q` under `set -o pipefail` is a
# trap: grep exits at the first match, the producer takes SIGPIPE, and the
# pipeline reports failure — so the match would be read as "no match" and the
# firmware would silently never load.
fwver=$("$AXBIN/axcmd" --device "$DEV" --fwver 2>&1)
if grep -qi "mismatch\|bl1" <<<"$fwver"; then
    # Never reload firmware underneath a live session. Every axr_device_connect()
    # already makes the runtime reload it; doing our own upload at the same time
    # means two writers to the same device. If a client is attached, leave gate 1
    # to it — it will load the firmware itself as soon as it connects.
    if command -v fuser >/dev/null 2>&1 && fuser -s "$NODE" 2>/dev/null; then
        log "card is in bootloader but a process has $DEV open — leaving the"
        log "firmware to that client (any inference loads it). Skipping --fwload."
    elif [ -z "${FW:-}" ]; then
        log "bootloader firmware, but no start_axelera_runtime.elf found"
    else
        log "bootloader firmware detected — loading $FW into RAM"
        out=$("$AXBIN/axcmd" --device "$DEV" --fwload "$FW" 2>&1)
        # Verify rather than assume: the upload is a ~3.8 MB bulk DMA write, and
        # it is exactly the thing that fails when the card has wedged.
        fwver=$("$AXBIN/axcmd" --device "$DEV" --fwver 2>&1)
        if grep -qi "mismatch\|bl1" <<<"$fwver"; then
            log "FIRMWARE UPLOAD FAILED — the card is still in bootloader."
            grep -qi "timed out\|Failed to write ELF" <<<"$out" && \
                log "  the ELF write timed out: the card's bulk-DMA path is wedged."
            log "  A driver reload does NOT clear this; only a full power-off does."
            log "  See 'Known issues' in the repo README before blaming a client."
            exit 1
        fi
    fi
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
