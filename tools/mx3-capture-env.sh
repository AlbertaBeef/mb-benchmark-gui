#!/usr/bin/env bash
# Capture the MemryX MX3 host configuration, for comparing one machine against
# another. Read-only: sysfs, dpkg and lspci only, no device access, no root
# required (anything needing root degrades to a note rather than failing).
#
# Written for docs/memryx-mx3-wedge.md, which compares the AMD Strix Halo host
# that produced the published 200 000-frame runs against the EPYC host where
# the same workload now wedges the card.
#
# Usage:  ./tools/mx3-capture-env.sh > mx3-env-<hostname>.txt
set -u

line() { printf '\n== %s ==\n' "$1"; }
val()  { printf '  %-22s %s\n' "$1" "${2:-<none>}"; }

printf 'MX3 host capture — %s — %s\n' "$(hostname)" "$(date -Is)"

line "host"
val "kernel"        "$(uname -r)"
val "arch"          "$(uname -m)"
val "cpu"           "$(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"
val "distro"        "$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"

line "memryx packages (apt)"
if command -v dpkg >/dev/null 2>&1; then
    dpkg -l 2>/dev/null | awk '/^ii/ && ($2 ~ /memx|mxa/) {printf "  %-28s %s\n", $2, $3}'
    [ -z "$(dpkg -l 2>/dev/null | awk '/^ii/ && ($2 ~ /memx|mxa/)')" ] && echo "  <none installed via apt>"
else
    echo "  <no dpkg>"
fi

line "memryx install history (dpkg log)"
zgrep -h -iE 'memx|mxa' /var/log/dpkg.log* 2>/dev/null \
    | grep -iE ' (install|upgrade|remove|purge) ' | sort | sed 's/^/  /' \
    || echo "  <no dpkg log access>"
printf '  log covers: %s .. %s\n' \
    "$(zgrep -h '' /var/log/dpkg.log* 2>/dev/null | sort | head -1 | cut -c1-10)" \
    "$(zgrep -h '' /var/log/dpkg.log* 2>/dev/null | sort | tail -1 | cut -c1-10)"

line "python memryx (venv)"
for v in "${MB_MEMRYX_PYTHON:-}" "$HOME/mb-edgeai/memryx-env/bin/python" \
         "$HOME/memryx-env/bin/python"; do
    [ -n "$v" ] && [ -x "$v" ] || continue
    val "interpreter" "$v"
    "$v" - <<'PY' 2>/dev/null || echo "  <memryx not importable>"
import memryx, os
print("  %-22s %s" % ("memryx", getattr(memryx, "__version__", "?")))
print("  %-22s %s" % ("path", os.path.dirname(memryx.__file__)))
PY
    break
done

line "kernel module"
val "module version" "$(cat /sys/module/memx_cascade_plus_pcie/version 2>/dev/null)"
val "loaded"         "$(grep -c '^memx_cascade_plus_pcie ' /proc/modules 2>/dev/null)"
if command -v dkms >/dev/null 2>&1; then
    dkms status 2>/dev/null | grep -i memx | sed 's/^/  /' || echo "  <no memx dkms>"
fi

line "pcie"
found=0
for d in /sys/bus/pci/devices/*/; do
    drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
    [ "$drv" = "memx_pcie_ai_chip" ] || continue
    found=1
    bdf=$(basename "$d")
    val "bdf"            "$bdf"
    val "link speed"     "$(cat "$d/current_link_speed" 2>/dev/null)"
    val "link width"     "x$(cat "$d/current_link_width" 2>/dev/null)"
    val "max width"      "x$(cat "$d/max_link_width" 2>/dev/null)"
    val "ids"            "$(lspci -s "$bdf" -nn 2>/dev/null | sed 's/^[^ ]* //')"
done
[ "$found" = 0 ] && echo "  <no memx_pcie_ai_chip bound>"
echo "  -- other accelerators, for link-width comparison --"
for d in /sys/bus/pci/devices/*/; do
    drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
    case "$drv" in hailo|dx_dma_pcie|axl)
        printf '  %-18s %-14s %s x%s\n' "$drv" "$(basename "$d")" \
            "$(cat "$d/current_link_speed" 2>/dev/null)" \
            "$(cat "$d/current_link_width" 2>/dev/null)";; esac
done

line "thermal (idle unless something is running)"
for h in /sys/class/hwmon/hwmon*; do
    n=$(cat "$h/name" 2>/dev/null)
    case "$n" in *memx*|*memryx*)
        for t in "$h"/temp*_input; do
            [ -r "$t" ] || continue
            # A sensor can exist and still refuse to be read (EOPNOTSUPP on a
            # wedged chip), so take the value first and only divide if it is
            # actually a number -- otherwise the arithmetic aborts the script.
            raw=$(cat "$t" 2>/dev/null) || continue
            case "$raw" in ''|*[!0-9-]*) continue;; esac
            printf '  %-22s %s C\n' "$(basename "$t")" "$(( raw / 1000 ))"
        done;;
    esac
done

line "mxa-manager"
val "active" "$(systemctl is-active mxa-manager 2>/dev/null)"
# mx_bench ships with the PYTHON sdk (the venv), not the apt C++ stack --
# so its version tracks `memryx` above, not memx-accl. acclBench is the C++ one.
val "mx_bench" "$(command -v mx_bench 2>/dev/null \
                || ls "$HOME"/mb-edgeai/memryx-env/bin/mx_bench \
                      "$HOME"/memryx-env/bin/mx_bench 2>/dev/null | head -1)"
val "acclBench" "$(command -v acclBench 2>/dev/null || ls /usr/bin/acclBench 2>/dev/null)"
