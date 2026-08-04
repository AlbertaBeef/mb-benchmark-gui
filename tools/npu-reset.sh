#!/usr/bin/env bash
# Status and reset for every NPU on this host, in one place.
#
#   npu-reset.sh                    # status of all four cards (no root needed)
#   sudo npu-reset.sh all           # reset every card
#   sudo npu-reset.sh memryx        # reset one (hailo|deepx|memryx|axelera)
#   sudo npu-reset.sh all --force   # ...even if an app still holds a device
#   sudo npu-reset.sh memryx --hard # ...and reload the driver too (see warning)
#   sudo npu-reset.sh memryx --hard --rescan   # ...then re-enumerate on PCI
#
# Status is the default on purpose: a reset tears down driver modules, which is
# not something to trigger by mistyping.
#
# WHAT A RESET CAN AND CANNOT FIX
#   memryx   — restarts mxa-manager, which clears the stale session behind the
#              usual symptom: any app hangs *before* main() in `memx_fops_open`
#              (one thread, 0% CPU, only fds 0/1/2 open), because libmemx opens
#              /dev/memx0 from an ELF constructor. A DRIVER reload is NOT done by
#              default — see the note above reset_memryx(); it has left this card
#              in D3cold with no device node at all. Use --hard knowingly.
#   deepx    — driver + dxrtd restart.
#   hailo    — driver reload. Note `DVM_ALREADY_IN_USE` is NOT a driver problem:
#              two processes are measuring power at once. Close one instead.
#   axelera  — reloads the driver, but a Metis whose bulk-DMA path has wedged
#              (`Failed to write ELF to device memory`, `DMA WR CH0 status -110`)
#              is NOT recoverable this way. Verified: only a full power-off is.
#              This script says so rather than pretending it worked.
set -uo pipefail

TARGET="${1:-status}"
FORCE=0; HARD=0; RESCAN=0
for a in "$@"; do
    case "$a" in
        --force)  FORCE=1 ;;
        --hard)   HARD=1 ;;
        --rescan) RESCAN=1 ;;
    esac
done

# Status checks, the ok/bad/warn helpers and the module/PCI utilities all live in
# npu-status.sh — sourced rather than copied so the two scripts cannot drift.
HERE=$(dirname "$(readlink -f "$0")")
if [ -r "$HERE/npu-status.sh" ]; then
    # shellcheck source=npu-status.sh
    . "$HERE/npu-status.sh"
else
    echo "cannot find npu-status.sh next to $0" >&2
    exit 1
fi

need_root() {
    [ "$(id -u)" -eq 0 ] && return 0
    echo "This needs root: sudo $0 $*" >&2
    exit 1
}

# Refuse to pull a driver out from under a running app unless told to.
clients() { pgrep -x mb-benchmark 2>/dev/null; pgrep -x mb-powermon 2>/dev/null; }
check_clients() {
    local c; c=$(clients | tr '\n' ' ')
    [ -z "$c" ] && return 0
    if [ "$FORCE" -eq 1 ]; then
        warn "killing clients: $c"
        pkill -x mb-benchmark 2>/dev/null; pkill -x mb-powermon 2>/dev/null
        sleep 2
    else
        bad "an app still holds a device (pids: $c)"
        info "close it, or re-run with --force to have this script kill it"
        exit 1
    fi
}

reload() {  # reload <module> [<module-to-unload-first> ...]
    local mod="$1"; shift
    local m
    for m in "$@"; do modprobe -r "$m" 2>/dev/null; done
    if ! modprobe -r "$mod" 2>/dev/null; then
        bad "could not unload $mod (refcount $(mod_users "$mod"))"
        return 1
    fi
    modprobe "$mod" || { bad "could not load $mod"; return 1; }
    for m in "$@"; do modprobe "$m" 2>/dev/null; done
    return 0
}

# ----------------------------------------------------------------- reset ----
reset_hailo() {
    head_ "Hailo-8 — reset"
    reload hailo_pci && ok "hailo_pci reloaded"
    sleep 1; [ -e /dev/hailo0 ] && ok "/dev/hailo0 back" || bad "/dev/hailo0 missing"
}

reset_deepx() {
    head_ "DeepX M1 — reset"
    systemctl stop dxrt.service 2>/dev/null && info "stopped dxrt.service"
    # dxrt_driver depends on dx_dma, so it has to come out first and go back last.
    modprobe -r dxrt_driver 2>/dev/null
    modprobe -r dx_dma 2>/dev/null || bad "could not unload dx_dma"
    modprobe dx_dma && modprobe dxrt_driver && ok "drivers reloaded"
    systemctl start dxrt.service 2>/dev/null && info "started dxrt.service"
    sleep 1; [ -e /dev/dxrt0 ] && ok "/dev/dxrt0 back" || bad "/dev/dxrt0 missing"
}

# MemryX recovery is deliberately staged, because the big hammer can make things
# worse. Measured 2026-08-04 on this host: `modprobe -r memx_cascade_plus_pcie`
# let the card fall to D3cold, and it would not come back —
#   memx_pcie_ai_chip 0000:47:00.0: Unable to change power state from D3cold to
#   D0, device inaccessible
#   memryx: memx_firmware_init: get hardware info from fw failed
#   probe with driver memx_pcie_ai_chip failed with error -1
# leaving the PCI device enumerated but unbound and NO /dev/memx0 at all, which
# is worse than the wedged-but-present state it started in. So the default is a
# daemon restart only; the module reload is opt-in via --hard.
reset_memryx() {
    head_ "MemryX MX3 — reset"
    systemctl stop mxa-manager 2>/dev/null && info "stopped mxa-manager"
    pkill -f "memryx-env/bin/python3" 2>/dev/null && info "killed orphaned helpers"
    sleep 1
    systemctl start mxa-manager 2>/dev/null && info "restarted mxa-manager"
    sleep 2
    if [ -e /dev/memx0 ]; then
        ok "/dev/memx0 present"
    else
        bad "/dev/memx0 missing"
    fi
    if [ "$HARD" -eq 0 ]; then
        info "this restarts the daemon only — it clears a stale session, which is"
        info "the usual cause of a pre-main() hang in memx_fops_open."
        info "If the chip itself is unresponsive (sensors reading 65262000),"
        info "escalate with:  sudo $0 memryx --hard"
        return 0
    fi

    warn "--hard: reloading the driver. On this host that has left the card in"
    warn "D3cold and unrecoverable without a power cycle. Continuing anyway."
    systemctl stop mxa-manager 2>/dev/null
    sleep 1
    reload memx_cascade_plus_pcie && ok "driver reloaded"
    systemctl start mxa-manager 2>/dev/null
    sleep 3
    if [ -e /dev/memx0 ]; then
        ok "/dev/memx0 back"
    else
        bad "/dev/memx0 missing — the card did not re-probe"
        info "check: journalctl -k | grep -i 'D3cold\|probe with driver'"
        info "try re-enumerating the PCI device before resorting to a power cycle:"
        local bdf; bdf=$(bdf_by_vendor 0x1fe9)
        info "  echo 1 > /sys/bus/pci/devices/${bdf:-<bdf>}/remove"
        info "  echo 1 > /sys/bus/pci/rescan"
        if [ -n "$bdf" ] && [ "$RESCAN" -eq 1 ]; then
            warn "--rescan given: re-enumerating $bdf"
            echo 1 > "/sys/bus/pci/devices/$bdf/remove" 2>/dev/null
            sleep 2
            echo 1 > /sys/bus/pci/rescan 2>/dev/null
            sleep 3
            [ -e /dev/memx0 ] && ok "/dev/memx0 back after rescan" \
                              || bad "still missing — power cycle required"
        fi
    fi
}


reset_axelera() {
    head_ "Axelera Metis — reset"
    local node axbin fw_before
    node=$(ls -d /dev/metis-* 2>/dev/null | head -1)
    axbin=$(ls -d /opt/axelera/runtime-*/bin 2>/dev/null | sort -V | tail -1)
    if [ -n "$node" ] && [ -n "$axbin" ]; then
        fw_before=$("$axbin/axcmd" --device "$(basename "$node")" --fwver 2>&1 | tr -d '\n')
    fi
    reload metis && ok "metis reloaded"
    sleep 2
    node=$(ls -d /dev/metis-* 2>/dev/null | head -1)
    [ -n "$node" ] && ok "$node back" || bad "no /dev/metis-* after reload"
    # Be honest about the limit of this operation.
    if grep -qi "bl1\|mismatch" <<<"${fw_before:-}"; then
        warn "the card was in bootloader before this reset, and a reload does not"
        info "clear a wedged Metis — if models still fail to load, POWER OFF."
    fi
}

# ------------------------------------------------------------------ main ----
case "$TARGET" in
    status|--status|"")
        npu_status_main all
        echo "To reset:  sudo $0 {all|hailo|deepx|memryx|axelera} [--force]"
        ;;
    all)      need_root "$@"; check_clients
              reset_hailo; reset_deepx; reset_memryx; reset_axelera
              echo; echo "=== status after reset ==="
              npu_status_main all ;;
    hailo)    need_root "$@"; check_clients; reset_hailo ;;
    deepx)    need_root "$@"; check_clients; reset_deepx ;;
    memryx)   need_root "$@"; check_clients; reset_memryx ;;
    axelera)  need_root "$@"; check_clients; reset_axelera ;;
    -h|--help|help)
        sed -n '2,30p' "$0" | sed 's/^# \?//' ;;
    *)
        echo "unknown target: $TARGET" >&2
        echo "usage: $0 [status|all|hailo|deepx|memryx|axelera] [--force]" >&2
        exit 2 ;;
esac
