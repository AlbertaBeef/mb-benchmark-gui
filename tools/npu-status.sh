#!/usr/bin/env bash
# Read-only health check for every edge-AI NPU on this host.
#
#   ./npu-status.sh            # all cards
#   ./npu-status.sh memryx     # one (hailo|deepx|memryx|axelera)
#
# Needs no root and touches nothing: it reads /proc, /sys, systemd state and —
# for Axelera only — asks the card its firmware version, which is a control-plane
# read. Nothing here can change device state. `npu-reset.sh` is the one that acts.
#
# It also sources cleanly: `npu-reset.sh` pulls in the status_* functions from
# here rather than keeping a second copy that could drift.
#
# Exit status: 0 if every checked card looks healthy, 1 if any check failed.
set -uo pipefail

c_ok=$'\e[32m'; c_bad=$'\e[31m'; c_warn=$'\e[33m'; c_off=$'\e[0m'
[ -t 1 ] || { c_ok=""; c_bad=""; c_warn=""; c_off=""; }

NPU_FAILURES=0
ok()   { echo "  ${c_ok}ok${c_off}      $*"; }
bad()  { echo "  ${c_bad}FAIL${c_off}    $*"; NPU_FAILURES=$((NPU_FAILURES + 1)); }
warn() { echo "  ${c_warn}warn${c_off}    $*"; }
info() { echo "          $*"; }
head_() { echo; echo "=== $1 ==="; }

# /proc/modules, not lsmod: /usr/sbin is not on a normal user's PATH, so `lsmod`
# silently fails here and every driver reads as missing.
mod_loaded() { grep -q "^$1 " /proc/modules; }
mod_users()  { awk -v m="$1" '$1==m{print $3}' /proc/modules; }

# BDF of a card by PCI vendor id — never hardcode one, they move across reboots.
bdf_by_vendor() {
    local d
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/vendor" 2>/dev/null)" = "$1" ] && { basename "$d"; return; }
    done
}

pci_driver() {  # pci_driver <bdf> -> bound driver name, or "NONE"
    local l; l=$(readlink "/sys/bus/pci/devices/$1/driver" 2>/dev/null)
    [ -n "$l" ] && basename "$l" || echo NONE
}

# ---------------------------------------------------------------------------
status_hailo() {
    head_ "Hailo-8"
    mod_loaded hailo_pci && ok "driver hailo_pci loaded" || bad "hailo_pci not loaded"
    [ -e /dev/hailo0 ] && ok "/dev/hailo0 present" || bad "/dev/hailo0 missing"
    local bdf; bdf=$(bdf_by_vendor 0x1e60)
    [ -n "$bdf" ] && info "$bdf driver=$(pci_driver "$bdf")"
    info "note: DVM_ALREADY_IN_USE is two power clients, not a wedged driver —"
    info "      close mb-powermon or mb-benchmark, don't reset anything"
}

status_deepx() {
    head_ "DeepX M1"
    mod_loaded dx_dma && ok "driver dx_dma loaded" || bad "dx_dma not loaded"
    mod_loaded dxrt_driver && ok "driver dxrt_driver loaded" || bad "dxrt_driver not loaded"
    [ -e /dev/dxrt0 ] && ok "/dev/dxrt0 present" || bad "/dev/dxrt0 missing"
    systemctl is-active --quiet dxrt.service && ok "dxrt.service active" \
                                             || bad "dxrt.service inactive"
}

status_memryx() {
    head_ "MemryX MX3"
    mod_loaded memx_cascade_plus_pcie && ok "driver memx_cascade_plus_pcie loaded" \
                                      || bad "driver not loaded"
    [ -e /dev/memx0 ] && ok "/dev/memx0 present" || bad "/dev/memx0 missing"
    systemctl is-active --quiet mxa-manager && ok "mxa-manager active" \
                                            || bad "mxa-manager inactive"

    local bdf drv; bdf=$(bdf_by_vendor 0x1fe9)
    if [ -n "$bdf" ]; then
        drv=$(pci_driver "$bdf")
        info "$bdf driver=$drv"
        # Enumerated but unbound is the D3cold state a driver reload can cause:
        # the card will not re-probe and there is no device node at all.
        if [ "$drv" = "NONE" ]; then
            bad "PCI device present but UNBOUND — the card did not re-probe"
            info "typically 'Unable to change power state from D3cold to D0'."
            info "recover with:  echo 1 > /sys/bus/pci/devices/$bdf/remove"
            info "               echo 1 > /sys/bus/pci/rescan"
            info "or power the machine off."
        fi
    fi

    # The tell for a chip that has stopped answering: every sensor reads the
    # -274 °C sentinel (65262000 millidegrees = 65536-274 as unsigned).
    local hw t sentinel=0
    for hw in /sys/class/hwmon/hwmon*; do
        [ "$(cat "$hw/name" 2>/dev/null)" = "memx0" ] || continue
        t=$(cat "$hw/temp1_input" 2>/dev/null)
        [ -n "$t" ] || continue
        info "temp1_input = $t (${t%???} °C)"
        [ "$t" -gt 150000 ] && sentinel=1
    done
    [ "$sentinel" -eq 1 ] && {
        bad "sensors report the driver's no-reading sentinel — chip is wedged"
        info "try:  sudo npu-reset.sh memryx      (restarts mxa-manager)"
    }

    local orphan; orphan=$(pgrep -f "memryx-env/bin/python3" 2>/dev/null | tr '\n' ' ')
    [ -n "$orphan" ] && warn "helper processes still running: $orphan"
    return 0
}

status_axelera() {
    head_ "Axelera Metis"
    mod_loaded metis && ok "driver metis loaded" || bad "metis not loaded"
    local bdf; bdf=$(bdf_by_vendor 0x1f9d)
    [ -n "$bdf" ] && info "$bdf driver=$(pci_driver "$bdf")"

    local node; node=$(ls -d /dev/metis-* 2>/dev/null | head -1)
    [ -n "$node" ] && ok "$node present" || { bad "no /dev/metis-*"; return 0; }

    local axbin fw
    axbin=$(ls -d /opt/axelera/runtime-*/bin 2>/dev/null | sort -V | tail -1)
    [ -z "$axbin" ] && { warn "no /opt/axelera runtime — cannot query firmware"; return 0; }
    fw=$("$axbin/axcmd" --device "$(basename "$node")" --fwver 2>&1 | tr -d '\n')
    info "$fw"

    local dma; dma=$(journalctl -b -k --no-pager 2>/dev/null |
                     grep -cE "DMA (RD|WR) CH0|Link down")
    info "DMA errors / link drops this boot: $dma"

    # Bootloader alone is normal after a cold boot. Bootloader *plus* DMA errors
    # means the bulk-DMA path is wedged and no firmware upload will land.
    if grep -qi "bl1\|mismatch" <<<"$fw"; then
        if [ "$dma" -gt 0 ]; then
            bad "card is in bootloader AND has DMA errors — WEDGED"
            info "a driver reload will NOT fix this. Power the machine OFF."
        else
            warn "card is in bootloader (normal after a cold boot)"
            info "running any model loads the firmware; for temperatures alone"
            info "use tools/axelera-temps.sh"
        fi
    else
        ok "runtime firmware loaded"
        local temps
        temps=$("$axbin/triton_trace" --device "$(basename "$node")" --slog --peek 2>&1 |
                grep -o "core_temps=\[[0-9,: ]*\]" | tail -1)
        [ -n "$temps" ] && ok "$temps" \
                        || warn "collector idle — run tools/axelera-temps.sh for temps"
    fi
    return 0
}

npu_status_main() {
    case "${1:-all}" in
        all|"")  status_hailo; status_deepx; status_memryx; status_axelera ;;
        hailo)   status_hailo ;;
        deepx)   status_deepx ;;
        memryx)  status_memryx ;;
        axelera) status_axelera ;;
        -h|--help|help)
            sed -n '2,15p' "$0" | sed 's/^# \?//'; return 0 ;;
        *) echo "unknown card: $1" >&2
           echo "usage: $0 [all|hailo|deepx|memryx|axelera]" >&2; return 2 ;;
    esac
    echo
    if [ "$NPU_FAILURES" -eq 0 ]; then
        echo "All checks passed."
    else
        echo "$NPU_FAILURES check(s) failed.  To act:  sudo npu-reset.sh {all|<card>}"
    fi
    [ "$NPU_FAILURES" -eq 0 ]
}

# Only run when executed directly; `source`ing it just defines the functions,
# which is how npu-reset.sh reuses these checks without a second copy.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    npu_status_main "${1:-all}"
fi
