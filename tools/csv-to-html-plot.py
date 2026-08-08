#!/usr/bin/env python3
"""
csv-to-html-plot.py — offline HTML plots for mb-benchmark CSV logs.

A local fork of ../mb-powermon/csv-to-html-plot.py. That one still works on our
files, but it only knows mb-powermon's vocabulary, so on a mb-benchmark log it
silently drops the four INA228 rails, every clock, and all six benchmark series
— a 40-warning run that plots 2 power series where there should be 6.

What this fork adds:

  * the metrics mb-powermon never had — `_INA228` (per-card shunt power),
    `_CLK` / `_C<n>` (core clocks);
  * the benchmark half of the schema — `<vendor>_fps`, `_fps_per_w`,
    `_mj_per_frame` — as three more charts;
  * the run's own context (`bench_state`, `bench_model`, `<vendor>_cfg`,
    `host`) as a summary table, and `message` as a log. `bench_target_fps` is
    read and deliberately not shown — the runs here are all max-speed, so a
    column repeating "max" carried nothing;
  * **the app's accelerator colours**, so a card is the same colour here as it
    is on screen in mb-benchmark and mb-powermon-gui.

Six charts, in the GUI's own order: Power, Temperature, Frequency, Frame Rate,
Efficiency, Energy.

Usage:
    python3 tools/csv-to-html-plot.py -i logs/mb-benchmark-….csv [-o out.html]

Identifying which card a BDF is
-------------------------------
The benchmark columns are vendor-named (`hailo_fps`), so those colour exactly.
The telemetry columns are not: `Logger` writes `<bdf>_<LABEL>` with the device
name stripped — deliberately, so the column reads `0000:47:00.0_T0` and not
`0000:47:00.0_MemryX_T0` — which keeps mb-powermon compatibility but throws away
the one field that said which card it was. And **BDFs are not stable across
reboots** on the four-card host, so they cannot be hardcoded: on 2026-08-08
DeepX was at `47` and MemryX at `c1`, the exact reverse of what CLAUDE.md's
host notes record.

Three sources, tried in order:

1. `--map BDF=vendor` — explicit, always wins.
2. **The log's own startup note**, which is what makes this reliable. The first
   `message` cell lists every probe with its sensor counts, in `discover()`
   order — "Hailo: 2 temp + 1 power sensor(s); MemryX: 4 temp + 1 power
   sensor(s); DeepX: 3 temp sensor(s); …" — and the temperature columns appear
   in that same order. Zipping the two names each BDF exactly, and the sensor
   counts are checked as a guard against the two ever drifting apart.
3. Label-vocabulary inference, if the note is missing or does not line up:

       TS<n>              → Hailo    (only HailoProbe emits TS*)
       SYS / AI<n>        → Axelera  (only AxeleraProbe emits SYS/AI*)
       T<n> with POW      → MemryX   (the mxa helper reports get_power)
       T<n> without POW   → DeepX    (dxrt-cli gives temperatures only)

   Note the direction of that last pair: **MemryX** is the one with a power
   reading. Getting it backwards silently swaps two cards' colours, which is
   worse than not colouring them, so anything unidentified falls back to the
   neutral palette instead of guessing.
"""

import argparse
import csv
import json
import re
import sys
from datetime import datetime
from pathlib import Path


# ---------------------------------------------------------------------------
# Colour palette — util.h's accent:: namespace, verbatim.
# ---------------------------------------------------------------------------
#
# "Colour means one card, everywhere" is an invariant of the app: these values
# are shared by mb-benchmark and mb-powermon-gui so a card looks the same in
# both. Keep them in step with util.h's device_accent(); do not invent RGB here.

ACCEL_COLORS = {
    "hailo":    "#D9694F",   # coral
    "memryx":   "#8AA67E",   # sage
    "deepx":    "#4E7CA1",   # slate blue
    "axelera":  "#E0A24B",   # amber
    "qualcomm": "#7E6699",   # plum
}

# Accel enum order — fixes series order everywhere in the app, so the legends
# here line up with the GUI's.
ACCEL_ORDER = ["hailo", "memryx", "deepx", "axelera", "qualcomm"]

# Anything not identified as a card (an unmapped INA228 bridge, the board
# ambient probe) falls back here, exactly as device_accent() returns false and
# lets the cycling palette take over.
FALLBACK_PALETTE = ["#534AB7", "#3478C2", "#A53860",
                    "#5C8001", "#8E44AD", "#D4A017"]

LOG_OVERLAY_COLOR = "#BA7517"


def _clamp(v):
    return max(0, min(255, int(round(v))))


def shade(hex_color, factor):
    """Lighten (factor > 1) or darken (factor < 1) a #RRGGBB colour.

    A card with several sensors — DeepX T0..T3, Axelera SYS+AI0..AI3 — must stay
    visibly *one card* while still separating its sensors. Varying lightness
    around the card's accent does that; giving each sensor an unrelated hue (as
    the upstream fallback does, hashing on device+kind) would break the
    one-card-one-colour rule the app depends on.
    """
    h = hex_color.lstrip("#")
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    if factor >= 1.0:
        t = min(1.0, factor - 1.0)
        r, g, b = (c + (255 - c) * t for c in (r, g, b))
    else:
        r, g, b = (c * factor for c in (r, g, b))
    return "#%02X%02X%02X" % (_clamp(r), _clamp(g), _clamp(b))


# Spread for multi-sensor probes: base first, then alternating lighter/darker so
# adjacent sensors never land on neighbouring shades.
_SHADE_STEPS = [1.0, 1.30, 0.72, 1.55, 0.88, 1.15, 0.60]


# ---------------------------------------------------------------------------
# CSV parsing
# ---------------------------------------------------------------------------

# Telemetry columns. The first five kinds come from mb-powermon; INA228, CLK and
# C<n> are ours and are exactly what the upstream plotter drops.
_METRIC_RE = re.compile(
    r"^(.+?)_(POW|TEMP|TS\d+|T\d+|SYS|AI\d+|PCIE\d+|TOTAL|INA228|CLK|C\d+)$"
)

# Benchmark columns: <vendor>_<metric>, vendor being accel_vendor().
_BENCH_RE = re.compile(
    r"^(" + "|".join(ACCEL_ORDER) + r")_(fps|fps_per_w|mj_per_frame|cfg)$"
)

# Context columns — not plotted, but not unrecognized either. Skipping these
# quietly is the difference between a clean run and 40 lines of stderr.
_CONTEXT_COLS = {"host", "bench_state", "bench_model", "bench_target_fps",
                 "message"}


def _classify_metric(met):
    """Return 'power', 'temp', 'freq', or None to skip."""
    if met in ("pow", "total", "ina228"):
        return "power"
    if re.match(r"^pcie\d+$", met):
        return "power"
    if met in ("temp", "sys"):
        return "temp"
    if re.match(r"^(ts|t|ai)\d+$", met):
        return "temp"
    if met == "clk" or re.match(r"^c\d+$", met):
        return "freq"
    return None


def parse_header(header):
    """Split the header into telemetry, benchmark and context columns.

    Returns (telemetry, bench, context) where
      telemetry = [(device, metric, idx, raw_name), ...]
      bench     = [(vendor, metric, idx, raw_name), ...]
      context   = {name: idx}
    """
    telemetry, bench, context = [], [], {}
    for i, col in enumerate(header):
        if col == "time" or not col.strip():
            if col == "time":
                context["time"] = i
            continue
        if col in _CONTEXT_COLS:
            context[col] = i
            continue
        m = _BENCH_RE.match(col)
        if m:
            bench.append((m.group(1), m.group(2), i, col))
            continue
        m = _METRIC_RE.match(col)
        if m:
            telemetry.append((m.group(1), m.group(2).lower(), i, col))
            continue
        if col.startswith("adafruit"):
            telemetry.append((col, "pow", i, col))
            continue
        print(f"warning: unrecognized column '{col}', skipping", file=sys.stderr)
    return telemetry, bench, context


# "MemryX: 4 temp + 1 power sensor(s)" / "DeepX: 3 temp sensor(s)". The probes
# that found nothing say "not present / no data" and are skipped; the folded
# shunts appear as "INA228 - <card>" and have no temperature columns of their own.
# The "@ <bdf>" half is what makes a log self-describing; logs written before
# that was added have the name only, and fall back to positional matching.
# The separator is colon-**space**, not just a colon: a BDF is itself full of
# colons ("0000:47:00.0"), so a `[^:]+` capture stops at the first one and
# silently yields "0000" — which then matches no device and looks like the note
# simply had no BDFs.
_NOTE_ENTRY_RE = re.compile(
    r"^(?P<name>[^:@]+?)(?:\s*@\s*(?P<bdf>\S+?))?\s*:\s+"
    r"(?:(?P<temp>\d+)\s+temp)?"
    r"(?:\s*\+\s*(?P<power>\d+)\s+power)?"
)


def _vendor_of(name):
    """Probe display name → accel_vendor(), or None if it is not a card."""
    low = name.strip().lower()
    if low in ACCEL_COLORS:
        return low
    # The Qualcomm NSP names itself after the device tree ("IQ9075"); the
    # separate "<board> Board" ambient probe has a space and is not a card.
    if " " not in name and re.match(r"^(IQ|QCS|QRB|SA)\d{4}", name):
        return "qualcomm"
    return None


def devices_from_note(rows, context):
    """Parse the log's startup note.

    Returns (explicit, ordered) where `explicit` is {bdf: vendor} — exact, and
    all that is needed — and `ordered` is [(vendor, n_temp, n_power)] for logs
    written before the note carried BDFs, to be matched positionally.
    """
    idx = context.get("message")
    if idx is None:
        return {}, []
    note = ""
    for r in rows:
        if idx < len(r) and r[idx]:
            note = r[idx]
            break
    if not note:
        return {}, []

    explicit, out = {}, []
    for part in note.split(";"):
        part = part.strip()
        if not part or "not present" in part or part.startswith("INA228 -"):
            continue
        m = _NOTE_ENTRY_RE.match(part)
        if not m:
            continue
        vendor = _vendor_of(m.group("name"))
        if vendor is None:
            continue
        if m.group("bdf"):
            explicit[m.group("bdf").strip()] = vendor
        n_temp = int(m.group("temp") or 0)
        if n_temp == 0:          # no temp columns ⇒ nothing to line up against
            continue
        out.append((vendor, n_temp, int(m.group("power") or 0)))
    return explicit, out


def identify_devices(telemetry, explicit, note_map, note_devices):
    """Map each telemetry device (a BDF) to an accelerator, or None.

    See the module docstring: --map wins, then the startup note (positional, and
    count-checked), then label-vocabulary inference.
    """
    kinds = {}
    order = []
    for dev, met, _, _ in telemetry:
        if dev not in kinds:
            kinds[dev] = set()
            order.append(dev)
        kinds[dev].add(met)

    # Devices that own temperature columns, in header order — the same order
    # discover() ran in, which is the order the note lists them.
    temp_order = [d for d in order
                  if any(_classify_metric(k) == "temp" for k in kinds[d])]

    from_note = {}
    if note_map and all(d in note_map or d in explicit for d in temp_order):
        note_devices = []              # exact mapping available; no need to guess
    if note_devices and len(note_devices) == len(temp_order):
        ok = True
        for dev, (vendor, n_temp, n_power) in zip(temp_order, note_devices):
            have_t = sum(1 for k in kinds[dev] if _classify_metric(k) == "temp")
            if have_t != n_temp:
                ok = False
                break
        if ok:
            from_note = {d: v for d, (v, _, _) in zip(temp_order, note_devices)}
        else:
            print("warning: startup note does not match the temperature columns; "
                  "falling back to label inference", file=sys.stderr)
    elif note_devices:
        print(f"warning: startup note lists {len(note_devices)} device(s) but "
              f"{len(temp_order)} have temperature columns; falling back to "
              f"label inference", file=sys.stderr)

    ident = {}
    for dev in order:
        ks = kinds[dev]
        if dev in explicit:
            ident[dev] = explicit[dev]
        elif dev in note_map:
            ident[dev] = note_map[dev]     # the log named this address outright
        elif dev in from_note:
            ident[dev] = from_note[dev]
        elif _vendor_of(dev):
            ident[dev] = _vendor_of(dev)
        elif any(re.match(r"^ts\d+$", k) for k in ks):
            ident[dev] = "hailo"
        elif "sys" in ks or any(re.match(r"^ai\d+$", k) for k in ks):
            ident[dev] = "axelera"
        elif any(re.match(r"^t\d+$", k) for k in ks):
            # MemryX reports power (mxa get_power); DeepX does not.
            ident[dev] = "memryx" if "pow" in ks else "deepx"
        else:
            ident[dev] = None
    return ident


def read_csv(path):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [r for r in reader if r and r[0]]
    return header, rows


def parse_timestamp(s):
    return datetime.fromisoformat(s.rstrip(","))


def safe_float(s):
    if s is None or s == "":
        return None
    try:
        return float(s)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Min-max downsampling (unchanged from upstream)
# ---------------------------------------------------------------------------

def auto_bucket_size(n_rows, target=600):
    if n_rows <= target:
        return 1
    return max(1, n_rows * 2 // target)


def minmax_downsample(rows, col_idx, bucket_size):
    """Bucket downsample preserving both peaks and troughs."""
    if bucket_size <= 1:
        out = []
        for r in rows:
            v = safe_float(r[col_idx]) if col_idx < len(r) else None
            out.append((r[0], v))
        return out

    out = []
    for i in range(0, len(rows), bucket_size):
        bucket = rows[i:i + bucket_size]
        candidates = []
        for j, r in enumerate(bucket):
            if col_idx >= len(r):
                continue
            v = safe_float(r[col_idx])
            if v is not None:
                candidates.append((j, r[0], v))
        if not candidates:
            continue
        if len(candidates) == 1:
            out.append((candidates[0][1], candidates[0][2]))
            continue
        lo = min(candidates, key=lambda c: c[2])
        hi = max(candidates, key=lambda c: c[2])
        if lo[0] == hi[0]:
            out.append((lo[1], lo[2]))
        else:
            chrono = sorted([lo, hi], key=lambda c: c[0])
            out.append((chrono[0][1], chrono[0][2]))
            out.append((chrono[1][1], chrono[1][2]))
    return out


def build_series(rows, col_idx, t0, bucket_size):
    samples = minmax_downsample(rows, col_idx, bucket_size)
    series = []
    for t_str, v in samples:
        try:
            t_sec = (parse_timestamp(t_str) - t0).total_seconds()
        except ValueError:
            continue
        series.append({"x": round(t_sec, 3), "y": v})
    return series


# ---------------------------------------------------------------------------
# Run context: what was benchmarked, when, and what went wrong
# ---------------------------------------------------------------------------

# A benchmark has to have run this long before a "sustained" figure means
# anything — it is the steady-state number, after the part has heated and any
# thermal throttling has settled.
SUSTAINED_AFTER_S = 20 * 60
SUSTAINED_WINDOW_S = 60.0
# The opening window, before the part has had time to heat.
COLD_WINDOW_S = 30.0


def _run_stats(samples, min_span_s, window_s, cold_window_s):
    """(average, cold, sustained) fps for one card over one run.

    The average is taken over the card's **active span** — its first non-zero
    sample to its last — not over the whole run window. Two things sit inside a
    run that are not the card working: the model load (`bench_state` is
    `starting`, fps 0) and the trailing rows after Stop, where the engine zeroes
    the rates but keeps the series. Both are real 0.0 readings rather than
    blanks, so averaging the run window would drag every figure down by however
    long the model took to load. Zeros *inside* the span are kept — a card that
    stalls mid-run should show it.

    `cold` is the mean over the first `cold_window_s` of the span — the card
    before it has heated. `sustained` is the mean over the last `window_s`, and
    is None unless the span reached `min_span_s`; the pair read together is what
    shows thermal throttling. Both start from the same active span as the
    average, so neither includes the model load.
    """
    pts = [(t, v) for t, v in samples if v is not None]
    nz = [i for i, (_, v) in enumerate(pts) if v > 0]
    if not nz:
        return None, None, None
    active = pts[nz[0]:nz[-1] + 1]
    avg = sum(v for _, v in active) / len(active)

    head = [v for t, v in active if t <= active[0][0] + cold_window_s]
    cold = sum(head) / len(head) if head else None

    span = active[-1][0] - active[0][0]
    if span < min_span_s:
        return avg, cold, None
    cutoff = active[-1][0] - window_s
    tail = [v for t, v in active if t >= cutoff]
    return avg, cold, (sum(tail) / len(tail) if tail else None)


def extract_runs(rows, context, bench, t0,
                 sustained_after=SUSTAINED_AFTER_S,
                 sustained_window=SUSTAINED_WINDOW_S,
                 cold_window=COLD_WINDOW_S):
    """Recover benchmark runs from the bench_state column.

    `bench_state` walks idle → starting → running → stopping → stopped, and a
    run is a maximal stretch of `starting`/`running`/`stopping`.

    **`stopped` closes a run, it does not belong to one.** The state is never
    reset to `idle` afterwards — it stays `stopped` for the rest of the process
    — so counting it as active makes every run appear to last until the log
    ends (33 min instead of 22 on the first file this was tried against) and
    puts the trailing zeroed rows inside the run.
    """
    ACTIVE = ("starting", "running", "stopping")
    st_idx = context.get("bench_state")
    if st_idx is None:
        return []
    model_idx = context.get("bench_model")
    cfg_idx = {v: i for v, met, i, _ in bench if met == "cfg"}
    fps_idx = {v: i for v, met, i, _ in bench if met == "fps"}

    def cell(r, i):
        return r[i] if i is not None and i < len(r) else ""

    runs, cur = [], None
    for r in rows:
        state = cell(r, st_idx)
        try:
            t = (parse_timestamp(r[0]) - t0).total_seconds()
        except (ValueError, IndexError):
            continue
        active = state in ACTIVE
        if active and cur is None:
            cur = {"t_start": t, "t_end": t, "model": cell(r, model_idx),
                   "cfg": {}, "samples": {},
                   "avg": {}, "cold": {}, "sustained": {}}
        if active:
            cur["t_end"] = t
            if not cur["model"]:
                cur["model"] = cell(r, model_idx)
            for v, i in cfg_idx.items():
                c = cell(r, i)
                if c:
                    cur["cfg"][v] = c
            for v, i in fps_idx.items():
                cur["samples"].setdefault(v, []).append((t, safe_float(cell(r, i))))
        elif cur is not None:
            runs.append(cur)
            cur = None
    if cur is not None:
        runs.append(cur)

    for run in runs:
        for v, samples in run["samples"].items():
            avg, cold, sust = _run_stats(samples, sustained_after,
                                         sustained_window, cold_window)
            if avg is not None:
                run["avg"][v] = avg
            if cold is not None:
                run["cold"][v] = cold
            if sust is not None:
                run["sustained"][v] = sust
        del run["samples"]
    return runs


def extract_messages(rows, context, t0):
    """Every non-empty `message` cell, with its elapsed time.

    The logger already de-duplicates: notes are written on transition, never
    repeated every tick, so this is a short list even for a long run.
    """
    idx = context.get("message")
    if idx is None:
        return []
    out = []
    for r in rows:
        if idx >= len(r) or not r[idx]:
            continue
        try:
            t = (parse_timestamp(r[0]) - t0).total_seconds()
        except (ValueError, IndexError):
            continue
        out.append({"t": t, "clock": r[0], "text": r[idx]})
    return out


# ---------------------------------------------------------------------------
# Active-phase detection and hailortcli log overlays (unchanged from upstream)
# ---------------------------------------------------------------------------

def detect_active_phases(rows, col_idx, t0, threshold=1.0):
    phases = []
    in_phase = False
    start_t = None
    last_t = None
    for r in rows:
        try:
            t = (parse_timestamp(r[0]) - t0).total_seconds()
        except (ValueError, IndexError):
            continue
        v = safe_float(r[col_idx]) if col_idx < len(r) else None
        active = v is not None and v > threshold
        if active and not in_phase:
            start_t = t
            in_phase = True
        elif not active and in_phase:
            phases.append((start_t, last_t))
            in_phase = False
        if active:
            last_t = t
    if in_phase and last_t is not None:
        phases.append((start_t, last_t))
    return phases


_LOG_SUMMARY_RE = re.compile(
    r"Device\s+(\S+?):\s*\n"
    r"\s*Power in streaming mode\s*\(average\)\s*=\s*([\d.]+)\s*W\s*\n"
    r"\s*\(max\)\s*=\s*([\d.]+)\s*W",
    re.MULTILINE,
)


def parse_log(path):
    summaries = {}
    if not path:
        return summaries
    text = Path(path).read_text()
    for m in _LOG_SUMMARY_RE.finditer(text):
        device, avg, mx = m.group(1), float(m.group(2)), float(m.group(3))
        summaries.setdefault(device, []).append({"avg": avg, "max": mx})
    return summaries


def _phase_mean_power(rows, col_idx, t0, t_start, t_end):
    total, n = 0.0, 0
    for r in rows:
        if not r:
            continue
        try:
            t = (parse_timestamp(r[0]) - t0).total_seconds()
        except (ValueError, IndexError):
            continue
        if t < t_start or t > t_end:
            continue
        v = safe_float(r[col_idx]) if col_idx < len(r) else None
        if v is not None:
            total += v
            n += 1
    return total / n if n else None


def _pick_streaming_phase(rows, col, t0, cluster, log_entry, subphase_dur):
    n = len(cluster)
    if n == 3:
        return cluster[1]
    if n == 2:
        a, b = cluster
        ma = _phase_mean_power(rows, col, t0, a[0], a[1])
        mb = _phase_mean_power(rows, col, t0, b[0], b[1])
        if ma is not None and mb is not None and min(ma, mb) > 0:
            if max(ma, mb) / min(ma, mb) > 2.0:
                merged = a if ma > mb else b
                m_dur = merged[1] - merged[0]
                return (merged[0] + m_dur / 2.0, merged[1])
        return b
    if n == 1:
        a = cluster[0]
        dur = a[1] - a[0]
        if dur > 1.5 * subphase_dur:
            return (a[0] + dur / 2.0, a[1])
        return a
    target = log_entry["avg"]
    best, best_err = None, float("inf")
    for sub_ts, sub_te in cluster:
        m = _phase_mean_power(rows, col, t0, sub_ts, sub_te)
        if m is None:
            continue
        e = abs(m - target)
        if e < best_err:
            best_err, best = e, (sub_ts, sub_te)
    return best if best is not None else cluster[len(cluster) // 2]


def assign_log_overlays(log_summaries, rows, telemetry, t0,
                        cluster_gap=10.0, subphase_dur=15.0, threshold=None):
    overlays = []
    power_cols = {dev: idx for dev, met, idx, _ in telemetry if met == "pow"}
    for device, entries in log_summaries.items():
        if device not in power_cols:
            print(f"warning: log mentions {device} but no _POW column found in CSV",
                  file=sys.stderr)
            continue
        col = power_cols[device]
        phases = (detect_active_phases(rows, col, t0) if threshold is None
                  else detect_active_phases(rows, col, t0, threshold=threshold))
        if not phases:
            print(f"warning: {device} has {len(entries)} log entr(y/ies) "
                  f"but no active phases detected; skipping", file=sys.stderr)
            continue
        clusters = [[phases[0]]]
        for ts, te in phases[1:]:
            if ts - clusters[-1][-1][1] < cluster_gap:
                clusters[-1].append((ts, te))
            else:
                clusters.append([(ts, te)])
        for entry, cluster in zip(entries, clusters):
            ts, te = _pick_streaming_phase(rows, col, t0, cluster, entry, subphase_dur)
            overlays.append({"device": device, "t_start": ts, "t_end": te,
                             "avg": entry["avg"], "max": entry["max"]})
    return overlays


# ---------------------------------------------------------------------------
# Dataset construction
# ---------------------------------------------------------------------------

def is_pci_device(name):
    return bool(re.match(r"^[0-9a-f]{4}:[0-9a-f]{2}", name))


def assign_colors(telemetry, ident):
    """Colour every telemetry column, keeping one card to one hue.

    Sensors within a card are separated by lightness (see shade()), assigned per
    (device, chart) so DeepX's four temperatures spread out without its clock
    series being pushed off the base colour on a chart of its own.
    """
    groups = {}
    for dev, met, idx, raw in telemetry:
        kind = _classify_metric(met)
        if kind is None:
            continue
        groups.setdefault((dev, kind), []).append(met)

    fallback_at = {}
    colors = {}
    for (dev, kind), mets in groups.items():
        accel = ident.get(dev)
        if accel and accel in ACCEL_COLORS:
            base = ACCEL_COLORS[accel]
        else:
            # Unidentified: an unmapped INA228 bridge or the board ambient probe.
            # device_accent() returns false for these too and the cycling palette
            # takes over — same behaviour, same reason.
            if dev not in fallback_at:
                fallback_at[dev] = len(fallback_at)
            base = FALLBACK_PALETTE[fallback_at[dev] % len(FALLBACK_PALETTE)]
        for n, met in enumerate(sorted(mets)):
            colors[(dev, met)] = shade(base, _SHADE_STEPS[n % len(_SHADE_STEPS)])
    return colors


def build_datasets(rows, telemetry, bench, ident, overlays, t0, bucket_size):
    power, temp, freq = [], [], []
    fps, eff, energy = [], [], []
    idle = set()

    def style(label, series, color, dash, tension):
        return {"label": label, "data": series,
                "borderColor": color, "backgroundColor": color,
                "borderDash": dash, "borderWidth": 1.5,
                "pointRadius": 0, "pointHoverRadius": 4,
                "tension": tension, "spanGaps": True}

    colors = assign_colors(telemetry, ident)

    for dev, met, idx, raw in telemetry:
        kind = _classify_metric(met)
        if kind is None:
            continue
        series = build_series(rows, idx, t0, bucket_size)
        color = colors[(dev, met)]
        accel = ident.get(dev)
        # Label with the card when we know it — "Hailo 0000:01:00.0_POW" reads
        # far better than a bare BDF, and it is the whole point of identifying.
        label = f"{accel.capitalize()} · {raw}" if accel else raw
        if kind == "power":
            # Distinguish the external shunt from an on-die reading: the INA228
            # is the whole-card figure the efficiency numbers are built on.
            dash = [] if met == "ina228" else [6, 4] if is_pci_device(dev) else [2, 3]
            power.append(style(label, series, color, dash, 0.1))
        elif kind == "temp":
            dash = [6, 4] if met in ("ts1", "sys") else []
            temp.append(style(label, series, color, dash, 0.2))
        elif kind == "freq":
            freq.append(style(label, series, color, [], 0.1))

    # Benchmark series. These need no inference — the column names the vendor.
    bench_by = {(v, met): (i, raw) for v, met, i, raw in bench}
    for vendor in ACCEL_ORDER:
        color = ACCEL_COLORS[vendor]
        for met, bucket in (("fps", fps), ("fps_per_w", eff),
                            ("mj_per_frame", energy)):
            got = bench_by.get((vendor, met))
            if not got:
                continue
            idx, raw = got
            series = build_series(rows, idx, t0, bucket_size)
            # A card that never ran is a flat line on zero carrying nothing.
            # Columns exist for every card the binary knows about — including
            # those absent from the build, which stay empty by design so two
            # logs stay diffable — and the engine folds onto fixed slots, so an
            # idle card logs a real 0.0 rather than a blank. Both look the same
            # here, and both are dropped. Reported, never silent.
            if not any(p["y"] for p in series):
                idle.add(vendor)
                continue
            bucket.append(style(vendor.capitalize(), series, color, [], 0.1))

    for ov in overlays:
        power.append({
            "label": f"{ov['device']} log avg = {ov['avg']:.3f} W",
            "data": [{"x": ov["t_start"], "y": ov["avg"]},
                     {"x": ov["t_end"], "y": ov["avg"]}],
            "borderColor": LOG_OVERLAY_COLOR, "backgroundColor": LOG_OVERLAY_COLOR,
            "borderWidth": 3, "pointRadius": 3, "pointStyle": "rectRot",
            "tension": 0, "showLine": True})
        if abs(ov["max"] - ov["avg"]) > 1e-4:
            power.append({
                "label": f"{ov['device']} log max = {ov['max']:.3f} W",
                "data": [{"x": ov["t_start"], "y": ov["max"]},
                         {"x": ov["t_end"], "y": ov["max"]}],
                "borderColor": LOG_OVERLAY_COLOR, "borderDash": [4, 3],
                "borderWidth": 2, "pointRadius": 0,
                "tension": 0, "showLine": True})

    return power, temp, freq, fps, eff, energy, idle


# ---------------------------------------------------------------------------
# HTML output
# ---------------------------------------------------------------------------

def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def legend_item_html(ds):
    color = ds["borderColor"]
    if ds.get("borderDash"):
        swatch = (f'<span style="display:inline-block;width:18px;height:0;'
                  f'border-top:2px dashed {color};"></span>')
    else:
        swatch = (f'<span style="display:inline-block;width:18px;height:2px;'
                  f'background:{color};"></span>')
    return ('<span style="display:flex;align-items:center;gap:6px;">'
            f'{swatch}{esc(ds["label"])}</span>')


def legend_html(label, datasets):
    if not datasets:
        return ""
    items = "".join(legend_item_html(ds) for ds in datasets)
    return ('<div style="display:flex;flex-wrap:wrap;gap:14px;'
            'margin:0.5rem 0;font-size:12px;color:#555;">'
            f'<strong style="color:#222;">{esc(label)}:</strong>{items}</div>')


def runs_html(runs, sustained_after):
    """Cold / average / sustained per card, read left to right as the run ages."""
    if not runs:
        return ""
    mins = sustained_after / 60.0
    out = ['<h2>Benchmark runs</h2>',
           '<table><tr><th>#</th><th>model</th><th>window</th>'
           '<th>card</th><th>configuration</th>'
           '<th class="num">cold fps</th>'
           '<th class="num">average fps</th>'
           '<th class="num">sustained fps</th></tr>']
    any_sustained = any(r["sustained"] for r in runs)
    for n, r in enumerate(runs, 1):
        cards = [v for v in ACCEL_ORDER if v in r["cfg"]] or \
                [v for v in ACCEL_ORDER if v in r["avg"]]
        cards = cards or [None]
        span = f'{r["t_start"]:.0f}–{r["t_end"]:.0f} s'
        dur = f'{(r["t_end"] - r["t_start"]) / 60.0:.1f} min'
        rowspan = len(cards)
        for j, v in enumerate(cards):
            lead = ""
            if j == 0:
                lead = (f'<td rowspan="{rowspan}">{n}</td>'
                        f'<td rowspan="{rowspan}">{esc(r["model"])}</td>'
                        f'<td rowspan="{rowspan}">{span}<br>'
                        f'<span class="dim">{dur}</span></td>')
            if v is None:
                out.append(f'<tr>{lead}<td colspan="5" class="dim">'
                           '(no card reported)</td></tr>')
                continue
            sw = (f'<span style="display:inline-block;width:10px;height:10px;'
                  f'border-radius:2px;background:{ACCEL_COLORS[v]};'
                  f'margin-right:6px;"></span>')
            def cell_num(x):
                return (f'<td class="num">{x:.1f}</td>' if x is not None
                        else '<td class="num dim">—</td>')
            out.append(f'<tr>{lead}<td>{sw}{v.capitalize()}</td>'
                       f'<td class="mono">{esc(r["cfg"].get(v, ""))}</td>'
                       f'{cell_num(r["cold"].get(v))}'
                       f'{cell_num(r["avg"].get(v))}'
                       f'{cell_num(r["sustained"].get(v))}</tr>')
    out.append("</table>")

    # Say what the two columns mean, and — when sustained is blank everywhere —
    # why, so an empty column never reads as "the card produced nothing".
    note = (f'<p class="dim">All three are taken over each card\'s active span '
            f'(first to last non-zero sample), so the model load and the rows '
            f'after Stop are excluded. <b>Cold</b> is the first '
            f'{COLD_WINDOW_S:.0f} s, before the part has heated; '
            f'<b>average</b> is the whole span; <b>sustained</b> is the final '
            f'{SUSTAINED_WINDOW_S:.0f} s, shown only for runs of at least '
            f'{mins:.0f} min. Cold against sustained is the throttling.')
    if not any_sustained:
        note += (' No run here reached that, so the column is empty — lower it '
                 'with <span class="mono">--sustained-after</span>.')
    out.append(note + "</p>")
    return "".join(out)


def messages_html(messages):
    if not messages:
        return ""
    out = ['<h2>Messages</h2>',
           '<p class="dim">Logged on transition, not repeated per tick.</p>',
           '<table><tr><th>t</th><th>clock</th><th>message</th></tr>']
    for m in messages:
        out.append(f'<tr><td class="num">{m["t"]:.0f} s</td>'
                   f'<td class="mono dim">{esc(m["clock"])}</td>'
                   f'<td>{esc(m["text"])}</td></tr>')
    out.append("</table>")
    return "".join(out)


HTML_HEAD = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>__TITLE__</title>
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
         sans-serif; max-width: 1100px; margin: 2rem auto; padding: 0 1rem;
         color: #222; }
  h1 { font-size: 1.4rem; margin-bottom: 0.25rem; }
  h2 { font-size: 1rem; color: #555; margin: 1.5rem 0 0.25rem; font-weight: normal; }
  .meta { color: #666; font-size: 0.85rem; margin-bottom: 1.5rem; }
  .chart { position: relative; width: 100%; height: 280px; margin-bottom: 12px; }
  table { border-collapse: collapse; font-size: 12px; width: 100%;
          margin: 0.5rem 0 1rem; }
  th, td { border-bottom: 1px solid #e5e5e5; padding: 4px 8px; text-align: left;
           vertical-align: top; }
  th { color: #555; font-weight: 600; border-bottom: 1px solid #bbb; }
  .mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  .num { text-align: right; font-variant-numeric: tabular-nums; }
  .dim { color: #888; }
</style>
</head>
<body>
<h1>__TITLE__</h1>
<div class="meta">__META__</div>
"""

CHART_JS = """
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<script>
const DATA = __DATA_JSON__;

function makeChart(canvasId, datasets, yLabel, xMax, unit, zeroBased) {
  const el = document.getElementById(canvasId);
  if (!el) return;
  new Chart(el, {
    type: 'line',
    data: { datasets: datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: 'nearest', axis: 'x', intersect: false },
      plugins: {
        legend: { display: false },
        tooltip: {
          callbacks: {
            title: (items) => 't = ' + items[0].parsed.x.toFixed(1) + ' s',
            label: (item) => item.dataset.label + ': ' +
              (item.parsed.y == null ? 'n/a' :
               item.parsed.y.toFixed(3) + ' ' + unit)
          }
        }
      },
      scales: {
        x: { type: 'linear',
             title: { display: true, text: 'elapsed time (s)' },
             min: 0, max: xMax },
        y: { title: { display: true, text: yLabel },
             beginAtZero: zeroBased }
      }
    }
  });
}

for (const c of DATA.charts)
  makeChart(c.canvas, c.datasets, c.y_label, DATA.x_max, c.unit, c.zero_based);
</script>
</body>
</html>
"""


def build_html(title, meta, charts, runs, messages, x_max, sustained_after):
    head = HTML_HEAD.replace("__TITLE__", esc(title)).replace("__META__", meta)

    body = runs_html(runs, sustained_after)
    for c in charts:
        if not c["datasets"]:
            continue
        body += f'<h2>{esc(c["title"])}</h2>'
        body += legend_html(c["title"], c["datasets"])
        body += f'<div class="chart"><canvas id="{c["canvas"]}"></canvas></div>'
    body += messages_html(messages)

    data = {"x_max": x_max,
            "charts": [c for c in charts if c["datasets"]]}
    return head + body + CHART_JS.replace("__DATA_JSON__", json.dumps(data))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_map(values):
    out = {}
    for v in values or []:
        if "=" not in v:
            print(f"error: --map expects BDF=vendor, got '{v}'", file=sys.stderr)
            sys.exit(2)
        bdf, vendor = v.split("=", 1)
        vendor = vendor.strip().lower()
        if vendor not in ACCEL_COLORS:
            print(f"error: --map unknown vendor '{vendor}' "
                  f"(expected one of {', '.join(ACCEL_ORDER)})", file=sys.stderr)
            sys.exit(2)
        out[bdf.strip()] = vendor
    return out


def main():
    p = argparse.ArgumentParser(
        description="Generate offline HTML plots from mb-benchmark CSV logs.")
    p.add_argument("-i", "--input", required=True, help="input CSV file")
    p.add_argument("-l", "--log",
                   help="optional hailortcli log file (overlays avg/max markers)")
    p.add_argument("-o", "--output", help="output HTML file (default: <input>.html)")
    p.add_argument("-d", "--downsample", type=int,
                   help="bucket size for min-max downsampling (default: auto, 1 = disable)")
    p.add_argument("-t", "--title", help="chart title (default: input filename stem)")
    p.add_argument("--map", action="append", metavar="BDF=VENDOR",
                   help="state which accelerator a BDF is, e.g. "
                        "--map 0000:47:00.0=memryx. Repeatable. Overrides the "
                        "label-vocabulary inference; use it when DeepX and "
                        "MemryX are the wrong way round.")
    p.add_argument("--sustained-after", type=float, default=20.0, metavar="MIN",
                   help="minimum run length before a sustained figure is shown "
                        "(default: 20). A sustained number is only meaningful "
                        "once the part has heated and any throttling settled.")
    p.add_argument("--sustained-window", type=float, default=60.0, metavar="SEC",
                   help="length of the trailing window the sustained figure "
                        "averages over (default: 60)")
    p.add_argument("--cold-window", type=float, default=30.0, metavar="SEC",
                   help="length of the opening window the cold figure averages "
                        "over (default: 30)")
    p.add_argument("--invocation-gap", type=float, default=10.0, metavar="SEC",
                   help="seconds between active power phases that marks a new "
                        "hailortcli invocation (default: 10.0)")
    p.add_argument("--phase-threshold", type=float, default=None, metavar="W",
                   help="power (W) above which a device counts as active "
                        "(default: 1.0)")
    p.add_argument("--subphase-duration", type=float, default=15.0, metavar="SEC",
                   help="expected hailortcli sub-phase duration (default: 15.0)")
    args = p.parse_args()

    output = args.output or str(Path(args.input).with_suffix(".html"))
    title = args.title or Path(args.input).stem

    header, rows = read_csv(args.input)
    if not rows:
        print(f"error: no data rows in {args.input}", file=sys.stderr)
        sys.exit(1)

    telemetry, bench, context = parse_header(header)
    note_map, note_devices = devices_from_note(rows, context)
    ident = identify_devices(telemetry, parse_map(args.map),
                             note_map, note_devices)

    bucket_size = (args.downsample if args.downsample is not None
                   else auto_bucket_size(len(rows)))

    t0 = parse_timestamp(rows[0][0])
    x_max = round((parse_timestamp(rows[-1][0]) - t0).total_seconds() + 1, 1)

    log_summaries = parse_log(args.log) if args.log else {}
    overlays = assign_log_overlays(log_summaries, rows, telemetry, t0,
                                   cluster_gap=args.invocation_gap,
                                   subphase_dur=args.subphase_duration,
                                   threshold=args.phase_threshold)

    power, temp, freq, fps, eff, energy, idle = build_datasets(
        rows, telemetry, bench, ident, overlays, t0, bucket_size)

    runs = extract_runs(rows, context, bench, t0,
                        sustained_after=args.sustained_after * 60.0,
                        sustained_window=args.sustained_window,
                        cold_window=args.cold_window)
    messages = extract_messages(rows, context, t0)

    # Same order as the GUI's graph column: telemetry leads because it is live
    # with no run in progress.
    charts = [
        {"title": "Power", "canvas": "powChart", "datasets": power,
         "y_label": "power (W)", "unit": "W", "zero_based": True},
        {"title": "Temperature", "canvas": "tempChart", "datasets": temp,
         "y_label": "temperature (°C)", "unit": "°C", "zero_based": False},
        {"title": "Frequency", "canvas": "freqChart", "datasets": freq,
         "y_label": "clock (MHz)", "unit": "MHz", "zero_based": True},
        {"title": "Frame Rate", "canvas": "fpsChart", "datasets": fps,
         "y_label": "frame rate (fps)", "unit": "fps", "zero_based": True},
        {"title": "Efficiency", "canvas": "effChart", "datasets": eff,
         "y_label": "efficiency (fps/W)", "unit": "fps/W", "zero_based": True},
        {"title": "Energy", "canvas": "energyChart", "datasets": energy,
         "y_label": "energy (mJ/frame)", "unit": "mJ/frame", "zero_based": True},
    ]

    host_idx = context.get("host")
    host = rows[0][host_idx] if host_idx is not None and host_idx < len(rows[0]) else ""
    meta = (f"Source: {esc(Path(args.input).name)}"
            + (f" · Host: {esc(host)}" if host else "")
            + f" · Started: {esc(t0.strftime('%Y-%m-%d %H:%M:%S'))}"
            + f" · Generated: {datetime.now().strftime('%Y-%m-%d %H:%M')}")

    Path(output).write_text(
        build_html(title, meta, charts, runs, messages, x_max,
                   args.sustained_after * 60.0))

    named = sum(1 for d in ident.values() if d)
    print(f"wrote {output}")
    print(f"  source: {args.input} ({len(rows)} rows, downsample={bucket_size})")
    print(f"  devices: {named}/{len(ident)} identified — " +
          ", ".join(f"{d}={v or '?'}" for d, v in sorted(ident.items())))
    n_power = len([d for d in power if "log" not in d["label"]])
    print(f"  series: {n_power} power, {len(temp)} temperature, {len(freq)} frequency, "
          f"{len(fps)} fps, {len(eff)} efficiency, {len(energy)} energy")
    print(f"  runs: {len(runs)}, messages: {len(messages)}, "
          f"log overlays: {len(overlays)}")
    if idle:
        print("  dropped (never ran, flat zero): " +
              ", ".join(v for v in ACCEL_ORDER if v in idle))


if __name__ == "__main__":
    main()
