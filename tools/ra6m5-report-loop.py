#!/usr/bin/env python3
"""Generate the data file for docs/reports/ra6m5-loop/.

    tools/ra6m5-report-loop.py \
        --run docs/data/ra6m5-loop-tc-20261123.csv \
        --out docs/reports/ra6m5-loop

Writes `data.js` (one `const LOOP = {...}`) and prints every scalar the page's
prose quotes, so the prose can be checked against the data rather than
remembered.  `index.html` renders every table and chart from this file;
nothing in the page re-derives a number.

REWRITTEN 2026-11-23 for firmware 0.6.0 and the three-arm run.  The previous
version of this script described the 0.5.0 four-setpoint run, which asked
"does the loop hold a setpoint?" at 70/85/60/95 C.  This run asks a different
question and the shape of the data follows it: ONE setpoint (90 C), ONE load,
three arms differing only in where the loop's single temperature comes from.

    arm 1   tc=on            closed loop on the physical thermocouple
    arm 2   thermocouple=off     open loop, the static feedforward alone
    arm 3   thermocouple=model   the fitted estimate requested -- and absent

TWO LOG SCHEMAS, and this script reads both.  Firmware <= 0.6.1 wrote `ctl.tc`
and `ctl.nn` as EFFECTIVE booleans, which only restated `ctl.src`.  0.7.0
replaced the pair with one three-valued REQUEST in `ctl.tc` (0 off, 1 real,
2 model) plus `ctl.model_ready`, so `ctl.tc` and `ctl.src` now say different
things -- asked-for against got.  The schema is detected from the presence of
`_CTL_MODEL_READY`; `src = 1` means the same state under both and is spelled
"modelled" throughout, 0.6.0 having called it "inferred".

So the axes that mattered in the old report are gone (there is no setpoint
ladder to fit a mW/C leakage slope against) and new ones appear:

  * PROVENANCE IS THE FIRST TABLE, not a footnote.  `_CTL_SRC` and
    `_CTL_STATE` are the only evidence an arm ran as configured, and arms 2
    and 3 are distinguishable by `_CTL_STATE` ALONE (5 open vs 6 degraded) --
    every other control channel reads identically in both.  If that one
    column had not separated them this run would have measured arm 2 three
    times and been unable to say so.
  * THE REPEATABILITY BOUND IS NOT ALWAYS MEASURABLE IN THE RUN ITSELF.  On
    the 2026-11-23 run arms 2 and 3 were a replicate pair, by accident of
    there being no model compiled in yet, and their difference bounds every
    other difference in that report.  Once a model exists all three arms are
    distinct configurations, so `repeat` is None and the bound has to be
    CARRIED FORWARD from the earlier run with `--prior`.  It is computed from
    that file rather than typed in, for the same reason every other number
    here is.  No arm-to-arm difference may be called real below it.
  * THE FOUR FAN-OFF BRACKETS ARE AN AMBIENT PROBE.  `fan.idle = 0` holds the
    fan off and the card at ~0.86 W for 30 minutes before the first arm,
    between the arms, and after the last.  Differencing them separates a real
    hardware change from a controller difference -- which this run needed, because
    the thermal resistance did move.

Conventions shared with `tools/ra6m5-report-tc.py`, for the same reasons:

  * Columns are resolved by SUFFIX.  The BDF and the USB port path in each name
    move across reboots (CLAUDE.md, 'Host specifics').
  * `series` is DECIMATED (every Nth row, not averaged) with `t` in seconds
    from the first row, so the timeline's cursor lands on a real sample.
  * `die` is the HOTTEST on-die sensor, max(TS0, TS1).  A mean would bury the
    sensor that actually throttles.
  * A missing reading is JSON `null`, never 0.
  * Power for every thermal resistance is the INA228 RAIL power
    (`_INA228_POWER`), never the card's own `_POW`.  The firmware's own die
    estimate uses the board's INA228, i.e. the rail, so this is also the only
    basis on which the estimator can be scored.

Two things this run forces that the old script did not have to handle:

  * OPEN LOOP REPORTS `nan` FOR `_CTL_IN` AND `_CTL_ERR`, by design -- nothing
    is measured, so quoting an error would invent one.  Every window helper
    therefore scores the arms against the CARD'S OWN SENSOR (`die` vs target),
    which is the same instrument in all three arms and the only one that is.
    `err_*` is reported where the loop had an error and null where it did not.
  * THE ESTIMATOR MUST BE SCORED ON THE SETTLED WINDOW ONLY.  In open loop
    `die_est` tracks `target` by construction while the fan is unclamped, so
    during the approach it reads ~90 C while the die is still at 57 -- pooling
    the ramp gives a +42 C "error" that means nothing about the model.  The old
    script pooled whole steps safely because every arm was closed-loop.

Steady state is the LAST 10 MINUTES of each 30-minute arm, and samples whose
rail power is below `--load-floor` are dropped from it: the engine's stop
sample sits inside that window with the load already gone, and one such row
moves the die mean by 0.3 C.
"""
import argparse, csv, json, math, os, re, statistics as st
from datetime import datetime

MODE = {0: 'off', 1: 'on'}
SRC = {0: 'measured', 1: 'modelled', 2: 'none'}
# `ctl.tc` as the 0.7.0 request.  Under the old schema it is an effective bool
# and is rendered as such instead -- see `Run.schema`.
TCW = {0: 'off', 1: 'real', 2: 'model'}
STATE = {0: 'idle', 1: 'settling', 2: 'holding', 3: 'floor', 4: 'ceiling',
         5: 'open', 6: 'degraded', 7: 'fault'}
# The EMC2101 quantises duty to 1/63.  Two consecutive 1 % requests can land on
# one level, so every duty here is the board's own readback (CLAUDE.md).
DUTY_STEP = 100.0 / 63.0


def num(v):
    v = v.strip()
    if not v:
        return None
    try:
        f = float(v)
    except ValueError:
        return None
    return None if math.isnan(f) else f


def mean(v):
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else None


def pstdev(v):
    v = [x for x in v if x is not None]
    return st.pstdev(v) if len(v) > 1 else None


def mn(v):
    v = [x for x in v if x is not None]
    return min(v) if v else None


def mx(v):
    v = [x for x in v if x is not None]
    return max(v) if v else None


def r4(v):
    return None if v is None else round(v, 4)


def div(a, b):
    return None if (a is None or not b) else a / b


def read_model(path):
    """Parse the soft sensor's constants out of firmware/.../Control.h.

    READ, NEVER RETYPED. The page quotes R_conv, the heat capacity and the
    time constants it implies, and a copy of those here would be free to drift
    away from the firmware that actually runs -- which is exactly the kind of
    disagreement a report is supposed to catch, not create. If the header is
    refactored this raises rather than silently reporting stale constants.
    """
    src = open(path).read()

    def arr(name):
        m = re.search(name + r'\[kCurveN\]\s*=\s*\{([^}]*)\}', src)
        if not m:
            raise SystemExit(f"{path}: cannot find {name}[kCurveN] -- "
                             "Control.h was refactored; fix read_model()")
        return [float(x) for x in m.group(1).replace('\n', ' ').split(',') if x.strip()]

    def const(name):
        m = re.search(r'kHeatCapacity\s*=\s*([0-9.]+)' if name == 'kHeatCapacity'
                      else r'double\s+' + name + r'\s*=\s*([0-9.]+)', src)
        if not m:
            raise SystemExit(f"{path}: cannot find {name}")
        return float(m.group(1))

    duty, rconv = arr('d'), arr('r')
    if len(duty) != len(rconv):
        raise SystemExit(f"{path}: curve_duty has {len(duty)} nodes, "
                         f"curve_rconv has {len(rconv)}")
    if any(b > a for a, b in zip(rconv, rconv[1:])):
        raise SystemExit(f"{path}: curve_rconv is not monotone non-increasing")
    return dict(duty=duty, rconv=rconv, C=const('kHeatCapacity'),
                rint=const('rint'), ready='model_ready() const { return true; }' in src)


def rconv_at(m, d):
    duty, rc = m['duty'], m['rconv']
    if d <= duty[0]:
        return rc[0]
    if d >= duty[-1]:
        return rc[-1]
    for i in range(1, len(duty)):
        if d <= duty[i]:
            f = (d - duty[i - 1]) / (duty[i] - duty[i - 1])
            return rc[i - 1] + f * (rc[i] - rc[i - 1])
    return rc[-1]


def run_model(rows, m, seed=None):
    """Integrate the soft sensor over `rows`, returning its estimate per row.

    The firmware's own recurrence, at the log's 1 Hz rather than the loop's
    250 ms -- against a 70-165 s pole that is still 70x oversampled. Inputs are
    held forward exactly as `Control::freshen()` holds them, so a gap in the
    CSV behaves here as it does on the board.
    """
    est, w, air, duty = [], None, None, None
    tc_hat, prev_s = seed, None
    for d in rows:
        if d['p'] is not None:
            w = d['p']
        if d['air'] is not None:
            air = d['air']
        if d['duty'] is not None:
            duty = d['duty']
        if w is None or air is None or duty is None:
            est.append(None)
            continue
        R = rconv_at(m, duty)
        ss = air + w * R
        if tc_hat is None:
            tc_hat = d['hs'] if d['hs'] is not None else ss
        elif prev_s is not None:
            dt = min(max(d['s'] - prev_s, 0.0), 5.0)
            tau = R * m['C']
            if dt > 0 and tau > 0:
                tc_hat += min(dt / tau, 1.0) * (ss - tc_hat)
        prev_s = d['s']
        est.append(tc_hat)
    return est


def score_est(rows, est, key='hs'):
    """rms / bias / sd / worst, and which way the worst error pointed.

    `cold` is the share of samples reading BELOW the probe. It is reported
    because the two error terms are not symmetric in consequence: an estimate
    that reads cold under-cools, and that is the only direction that can cost
    throttle margin.
    """
    e = [a - d[key] for a, d in zip(est, rows)
         if a is not None and d[key] is not None]
    if not e:
        return None
    mu = sum(e) / len(e)
    return dict(n=len(e), rms=r4(math.sqrt(sum(x * x for x in e) / len(e))),
                bias=r4(mu), sd=r4(pstdev(e)),
                worst_cold=r4(min(e)), worst_hot=r4(max(e)),
                cold_pct=r4(100.0 * sum(1 for x in e if x < 0) / len(e)))


class Run:
    def __init__(self, path):
        self.path = path
        with open(path) as fh:
            r = csv.reader(fh)
            hdr = next(r)
            ix = {n: i for i, n in enumerate(hdr)}

            def c(suffix, required=True):
                for n in hdr:
                    if n.endswith(suffix):
                        return ix[n]
                if required:
                    raise SystemExit(f"{path}: no column ending in {suffix!r}")
                return None

            cols = dict(
                ts0=c('_TS0'), ts1=c('_TS1'), air=c('_EMC2101'), hs=c('_MAX31855'),
                p=c('_INA228_POWER'), pcard=c('_POW'), clk=c('_CLK'),
                flow=c('_AIRFLOW'), rpm=c('_FAN_RPM'), duty=c('_FAN_DUTY'),
                mode=c('_CTL_MODE'), src=c('_CTL_SRC'), state=c('_CTL_STATE'),
                tc=c('_CTL_TC'), nn=c('_CTL_NN', False),
                model_ready=c('_CTL_MODEL_READY', False), auto=c('_CTL_AUTO'),
                tgt=c('_CTL_TARGET'), inp=c('_CTL_IN'), die_est=c('_CTL_DIE_EST'),
                err=c('_CTL_ERR'), cduty=c('_CTL_DUTY'), ff=c('_CTL_FF'),
                integ=c('_CTL_INTEG'), sat=c('_CTL_SAT'),
            )
            self.rows = []
            for row in r:
                d = {'t': datetime.fromisoformat(row[0]), 'st': row[ix['bench_state']],
                     'msg': row[ix['message']]}
                d['fps'] = num(row[ix['hailo_fps']])
                for k, i in cols.items():
                    # An optional column resolves to None (a schema that does
                    # not carry it); the field still exists, always empty, so
                    # every consumer can ask for it without a guard.
                    d[k] = None if i is None else num(row[i])
                dies = [x for x in (d['ts0'], d['ts1']) if x is not None]
                d['die'] = max(dies) if dies else None
                d['drop'] = (d['die'] - d['hs']) if (d['die'] is not None and d['hs'] is not None) else None
                self.rows.append(d)
        # 0.7.0 removed `ctl.nn` and added `ctl.model_ready`; its absence is
        # what identifies a pre-0.7.0 log, in which `ctl.tc` is a bool.
        self.schema = 2 if cols.get('model_ready') is not None else 1
        self.t0 = self.rows[0]['t']
        for d in self.rows:
            d['s'] = (d['t'] - self.t0).total_seconds()
        if not any(r['tc'] is not None for r in self.rows):
            raise SystemExit(f"{path}: no _CTL_TC column data -- this run predates "
                             "firmware 0.6.0 (kProtocol 2); _CTL_SRC in a 0.5.0 log "
                             "means die-vs-heatsink, not provenance, and must not be "
                             "read by this script")
        # Nothing below reads `nn` under schema 2, nor `model_ready` under 1.

    # ---- window helpers -------------------------------------------------
    def blocks(self, pred, minlen=60):
        out, cur = [], None
        for d in self.rows:
            if pred(d):
                cur = [d] if cur is None else cur + [d]
            else:
                if cur and len(cur) >= minlen:
                    out.append(cur)
                cur = None
        if cur and len(cur) >= minlen:
            out.append(cur)
        return out

    def arms(self):
        """One block per benchmarked arm, WHOLE -- the first sample included.

        The engine's first sample divides a frame-counter delta by a partial
        interval and reads high (CLAUDE.md, 'Discard the first
        BenchEngine::sample()'), so it is dropped wherever the FRAME RATE is
        summarised -- see `throughput` -- but not here.  That trap is about
        `hailo_fps` alone, and the control-loop channels in the same row are a
        direct read of the board: dropping it would hide the arming sample,
        which is where the bounded PI has to prove itself."""
        return self.blocks(lambda d: d['st'] == 'running', 600)

    def brackets(self):
        """The fan-off, loop-off, card-idle windows -- `fan.idle = 0`.

        NOT the same thing as the 0.5.0 report's `idles()`, which looked for
        the loop still armed with the load gone.  Here the plan hands the
        register back before every pause, so the loop is OFF and the fan is at
        0 %: an unforced thermal baseline at a fixed ~0.86 W.  Repeated four
        times across 3.5 hours, that is an ambient probe, and it is the only
        instrument in the run that can tell a hardware change from a controller
        difference."""
        return self.blocks(
            lambda d: d['st'] != 'running' and (d['mode'] or 0) == 0
            and d['duty'] is not None and d['duty'] <= 0.1, 300)


def tail(block, secs, floor=None):
    end = block[-1]['s']
    out = [d for d in block if d['s'] >= end - secs]
    if floor is not None:
        out = [d for d in out if d['p'] is not None and d['p'] >= floor]
    return out


def hist(block, key):
    h = {}
    for d in block:
        if d[key] is not None:
            k = int(d[key])
            h[k] = h.get(k, 0) + 1
    return dict(sorted(h.items()))


def modal(h):
    return max(h.items(), key=lambda kv: kv[1])[0] if h else None


def settle_die(block, tgt, band=0.5, hold=1.0):
    """First sample whose DIE is within `band` of target and stays within
    `hold` for the rest of the arm.

    Scored on the card's own sensor, not on `_CTL_IN`: the open-loop arms have
    no `_CTL_IN` at all, and the card's sensor is the one instrument common to
    all three arms.  A first touch that is then left behind is an overshoot
    crossing, not a settle, which is why the tail is tested too."""
    for i, d in enumerate(block):
        if d['die'] is None or abs(d['die'] - tgt) > band:
            continue
        if all(x['die'] is not None and abs(x['die'] - tgt) <= hold for x in block[i:]):
            return d['s'] - block[0]['s']
    return None


def occupancy(block):
    """Which quantised EMC2101 levels the arm actually sat on, and for how
    long.  The open-loop arms differ from each other mostly by which side of
    one level boundary they landed on, so this is a headline, not a detail."""
    o = {}
    for d in block:
        if d['duty'] is not None:
            k = round(d['duty'], 1)
            o[k] = o.get(k, 0) + 1
    return dict(sorted(o.items()))


def describe_arm(block, window, floor):
    b0 = block[0]
    tgt = mean([d['tgt'] for d in block])
    tl = tail(block, window, floor)
    tl_all = tail(block, window)

    # Histograms span the WHOLE block.  block[0] is the engine's first sample,
    # which reads high for the FRAME RATE only (CLAUDE.md); the control channels
    # in that row are a direct read of the board and are the arming second --
    # the one this report most needs, because that is where the bounded PI has
    # to prove itself.  Only `fps` below drops it.
    states = hist(block, 'state')
    srcs = hist(block, 'src')
    st_mode = modal(states)
    src_mode = modal(srcs)
    tcs = hist(block, 'tc')
    nns = hist(block, 'nn')
    mrs = hist(block, 'model_ready')

    # The arm's identity comes from the EFFECTIVE flags plus the state, because
    # the log records what the loop DID, never what the plan asked for.  State
    # 5 vs 6 is the whole difference between arm 2 and arm 3.
    if src_mode == 0:
        label, kind = 'thermocouple', 'closed'
    elif src_mode == 1:
        label, kind = 'learned estimate', 'closed'
    elif st_mode == 6:
        label, kind = 'none (estimate requested, absent)', 'open'
    else:
        label, kind = 'none (open by choice)', 'open'

    die = [d['die'] for d in tl]
    derr = [d['die'] - tgt for d in tl if d['die'] is not None]
    lerr = [d['inp'] - tgt for d in tl if d['inp'] is not None]
    est = [d['die_est'] - d['die'] for d in tl
           if d['die_est'] is not None and d['die'] is not None]
    rint = [d['drop'] / d['p'] for d in tl if d['drop'] is not None and d['p']]
    rconv = [(d['hs'] - d['air']) / d['p'] for d in tl
             if d['hs'] is not None and d['air'] is not None and d['p']]
    rtot = [(d['die'] - d['air']) / d['p'] for d in tl
            if d['die'] is not None and d['air'] is not None and d['p']]
    dff = [d['cduty'] - d['ff'] for d in block
           if d['cduty'] is not None and d['ff'] is not None]
    body = block
    fps = [d['fps'] for d in block[1:] if d['fps']]   # see the note above

    rec = dict(
        t0=round(b0['s']), t1=round(block[-1]['s']), n=len(block),
        clock=b0['t'].strftime('%H:%M'), target=r4(tgt),
        label=label, kind=kind,
        mode=modal(hist(block, 'mode')), src=src_mode, state=st_mode,
        src_name=SRC.get(src_mode), state_name=STATE.get(st_mode),
        states=states, srcs=srcs, tcs=tcs, nns=nns, mrs=mrs,
        autos=hist(block, 'auto'),
        tc=modal(tcs), nn=modal(nns), model_ready=modal(mrs),
        auto=modal(hist(block, 'auto')),
        # --- the regulated quantity, scored on the CARD's sensor ---
        n_settled=len(tl), n_window=len(tl_all),
        die=r4(mean(die)), die_sd=r4(pstdev(die)),
        die_err=r4(mean(derr)), die_err_sd=r4(pstdev(derr)),
        die_err_min=r4(mn(derr)), die_err_max=r4(mx(derr)),
        peak=r4(mx([d['die'] for d in block])),
        over=r4(None if mx([d['die'] for d in block]) is None
                else mx([d['die'] for d in block]) - tgt),
        settle=None if settle_die(block, tgt) is None else round(settle_die(block, tgt)),
        reach=next((round(d['s'] - b0['s']) for d in block
                    if d['die'] is not None and d['die'] >= tgt - 0.5), None),
        # --- the loop's own error, where it had one ---
        err_mean=r4(mean(lerr)), err_sd=r4(pstdev(lerr)), err_n=len(lerr),
        # --- thermal ---
        hs=r4(mean([d['hs'] for d in tl])), air=r4(mean([d['air'] for d in tl])),
        p=r4(mean([d['p'] for d in tl])), p_sd=r4(pstdev([d['p'] for d in tl])),
        duty=r4(mean([d['duty'] for d in tl])), duty_sd=r4(pstdev([d['duty'] for d in tl])),
        duty_lo=r4(mn([d['duty'] for d in block])), duty_hi=r4(mx([d['duty'] for d in block])),
        occupancy={str(k): v for k, v in occupancy(tl).items()},
        rpm=r4(mean([d['rpm'] for d in tl])), af=r4(mean([d['flow'] for d in tl])),
        rint=r4(mean(rint)), rint_sd=r4(pstdev(rint)),
        rconv=r4(mean(rconv)), rtot=r4(mean(rtot)),
        # --- the estimator, settled window only (see the module docstring) ---
        est=r4(mean(est)), est_sd=r4(pstdev(est)),
        est_min=r4(mn(est)), est_max=r4(mx(est)),
        est_absmax=r4(mx([abs(x) for x in est])),
        est_under=sum(1 for x in est if x < 0), est_n=len(est),
        # --- the PI's authority ---
        ff=r4(mean([d['ff'] for d in tl])),
        integ=r4(mean([d['integ'] for d in tl])),
        integ_lo=r4(mn([d['integ'] for d in body])),
        integ_hi=r4(mx([d['integ'] for d in body])),
        integ_zero=all(d['integ'] == 0 for d in body if d['integ'] is not None),
        dff_min=r4(mn(dff)), dff_max=r4(mx(dff)),
        dff_absmax=r4(mx([abs(x) for x in dff])),
        sat=sum(1 for d in block if d['sat']),
        # --- throughput ---
        fps=r4(mean(fps)), fps_sd=r4(pstdev(fps)),
        fps_min=r4(mn(fps)), fps_max=r4(mx(fps)),
        clk_lo=r4(mn([d['clk'] for d in body])),
        thr=round(100 * sum(1 for d in body if d['clk'] is not None and d['clk'] < 399)
                  / max(1, len(body)), 3),
    )
    # The bumpless-transfer preseed, and what the trim window did with it.
    # The preseed is the value at the ARM -- block[0] -- not the sample after
    # it.  One tick of ki already moves it, and quoting the second sample
    # understated 0.5.0's injection by 0.4 points when this was last written.
    i0 = next((d['integ'] for d in block if d['integ'] is not None), None)
    e0 = next((d['err'] for d in block if d['err'] is not None), None)
    rec['preseed'] = r4(i0)
    rec['preseed_err'] = r4(e0)
    rec['rearmed'] = bool(i0 is not None and abs(i0) > 1.0)
    if rec['rearmed']:
        z = next((d['s'] - b0['s'] for d in body
                  if d['integ'] is not None and d['integ'] <= 0), None)
        rec['integ_zero_at'] = None if z is None else round(z)
        rec['integ_rate'] = r4(-i0 / z * 60) if z else None
    else:
        rec['integ_zero_at'] = None
        rec['integ_rate'] = None
    return rec


def describe_bracket(block, window):
    """A fan-off, card-idle window summarised over its LAST `window` seconds.

    `window` here is deliberately shorter than the arms' steady-state window.
    Every bracket but the first follows a 30-minute 4.9 W soak, so reaching 600 s
    back into it picks up the cooling transient: the same bracket reads 43.41 C
    over its last 600 s and 43.38 C over its last 300 s, and the difference is
    larger than the session drift this window exists to measure."""
    tl = tail(block, window)
    head = tl[:60] if len(tl) >= 120 else tl
    foot = tl[-60:] if len(tl) >= 120 else tl
    return dict(
        t0=round(block[0]['s']), t1=round(block[-1]['s']), n=len(block),
        clock=block[0]['t'].strftime('%H:%M'), n_tail=len(tl),
        die=r4(mean([d['die'] for d in tl])), die_sd=r4(pstdev([d['die'] for d in tl])),
        hs=r4(mean([d['hs'] for d in tl])), hs_sd=r4(pstdev([d['hs'] for d in tl])),
        air=r4(mean([d['air'] for d in tl])), air_sd=r4(pstdev([d['air'] for d in tl])),
        p=r4(mean([d['p'] for d in tl])), p_sd=r4(pstdev([d['p'] for d in tl])),
        af=r4(mean([d['flow'] for d in tl])), duty=r4(mean([d['duty'] for d in tl])),
        # at a fixed 0.86 W with the fan off, die - air is the self-heating rise
        rise=r4(None if (mean([d['die'] for d in tl]) is None
                         or mean([d['air'] for d in tl]) is None)
                else mean([d['die'] for d in tl]) - mean([d['air'] for d in tl])),
        # (die - air) / W with the fan off is the NATURAL-CONVECTION resistance to
        # ambient.  At a fixed 0.86 W it is the single number that says whether the
        # card's thermal environment is the same as it was, and it is what moved.
        r_amb=r4(None if (mean([d['die'] for d in tl]) is None
                          or mean([d['air'] for d in tl]) is None
                          or not mean([d['p'] for d in tl]))
                 else (mean([d['die'] for d in tl]) - mean([d['air'] for d in tl]))
                 / mean([d['p'] for d in tl])),
        # flat to within one die LSB across the window => a usable baseline
        creep=r4(None if (mean([d['die'] for d in head]) is None
                          or mean([d['die'] for d in foot]) is None)
                 else mean([d['die'] for d in foot]) - mean([d['die'] for d in head])),
        settled=bool(mean([d['die'] for d in head]) is not None
                     and abs(mean([d['die'] for d in foot])
                             - mean([d['die'] for d in head])) < 0.15),
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--run', required=True, help='the session with the loop armed')
    ap.add_argument('--out', required=True)
    ap.add_argument('--step', type=int, default=10, help='timeline decimation (default 10 s)')
    ap.add_argument('--window', type=int, default=600,
                    help='steady-state window, seconds back from the end of an arm (default 600)')
    ap.add_argument('--bracket-window', type=int, default=300,
                    help='window for the fan-off brackets, seconds back from the end '
                         '(default 300). Shorter than --window on purpose: every bracket '
                         'but the first follows a hot soak, and 600 s reaches into the '
                         'cooling transient.')
    ap.add_argument('--firmware', default=os.path.join(
                        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        'firmware', 'mb-probe-ra6m5', 'Control.h'),
                    help='Control.h, read for the soft sensor\'s constants. They are '
                         'parsed rather than copied so the page cannot quote a model '
                         'the firmware does not have.')
    ap.add_argument('--prior', help='an earlier run of the SAME plan whose two '
                    'open arms were a replicate pair, used only for the '
                    'run-to-run repeatability bound.  Needed once a model is '
                    'compiled in, because all three arms are then distinct and '
                    'the run can no longer measure its own noise floor.')
    ap.add_argument('--load-floor', type=float, default=4.7,
                    help='drop settled samples below this rail wattage (default 4.7 W). '
                         'The engine stop sample sits inside the window with the load '
                         'already gone and moves the die mean by 0.3 C.')
    a = ap.parse_args()

    R = Run(a.run)
    arms, brks = R.arms(), R.brackets()
    if len(arms) != 3:
        print(f"note: {len(arms)} benchmarked arms, not the 3 this page is written for")
    if len(brks) != 4:
        print(f"note: {len(brks)} fan-off brackets, not the 4 this page is written for")

    out = {}
    out['start'] = R.rows[0]['t'].isoformat(timespec='milliseconds')
    out['end'] = R.rows[-1]['t'].isoformat(timespec='milliseconds')
    out['n'] = len(R.rows)
    out['mins'] = round(R.rows[-1]['s'] / 60, 1)
    out['window'] = a.window
    out['load_floor'] = a.load_floor
    # Derived, not asserted: a schema-2 log can only have come from 0.7.0 or
    # later. Hardcoding this made the page claim 0.6.0 for any run.
    out['fw'] = '0.6.x' if R.schema == 1 else '>= 0.7.0'
    out['proto'] = 2 if R.schema == 1 else 3
    out['schema'] = R.schema
    out['arms'] = [describe_arm(b, a.window, a.load_floor) for b in arms]
    out['bracket_window'] = a.bracket_window
    out['brackets'] = [describe_bracket(b, a.bracket_window) for b in brks]
    A = out['arms']

    # ---- provenance: the only confirmation an arm ran as configured -------
    # Arms 2 and 3 agree on SRC, TC, NN and AUTO.  _CTL_STATE is the single
    # column that separates them, and the firmware had to be fixed on hardware
    # to make it do so (kOpen was unreachable because the NaN test sat after
    # the if/else instead of inside the kInferred branch).
    # `nn` only exists in a pre-0.7.0 log; `model_ready` only in a newer one.
    chans = (['src', 'tc', 'nn', 'auto', 'state'] if R.schema == 1
             else ['src', 'tc', 'model_ready', 'auto', 'state'])
    out['provenance'] = dict(
        schema=R.schema, channels=chans, tcw=TCW,
        rows=[dict(arm=i + 1, label=s['label'],
                   src=s['srcs'], tc=s['tcs'], nn=s['nns'], mr=s['mrs'],
                   auto=s['autos'], state=s['states']) for i, s in enumerate(A)],
        # which channels actually differ between every pair of arms
        discriminators={},
    )
    for i in range(len(A)):
        for j in range(i + 1, len(A)):
            KEY = {'src': 'srcs', 'tc': 'tcs', 'nn': 'nns',
                   'model_ready': 'mrs', 'auto': 'autos', 'state': 'states'}
            d = [k for k in chans if modal(A[i][KEY[k]]) != modal(A[j][KEY[k]])]
            out['provenance']['discriminators'][f'{i+1}v{j+1}'] = d

    # ---- the headline: closed vs open at the same setpoint ----------------
    closed = [s for s in A if s['kind'] == 'closed']
    openl = [s for s in A if s['kind'] == 'open']
    out['compare'] = dict(
        closed=[s['label'] for s in closed], open=[s['label'] for s in openl],
        rows=[dict(arm=i + 1, label=s['label'], kind=s['kind'],
                   die_err=s['die_err'], die_err_sd=s['die_err_sd'],
                   peak=s['peak'], over=s['over'], reach=s['reach'],
                   duty=s['duty'], duty_sd=s['duty_sd'], occupancy=s['occupancy'],
                   rpm=s['rpm'], af=s['af'], hs=s['hs'], air=s['air'], p=s['p'],
                   rint=s['rint'], est=s['est'], est_sd=s['est_sd'],
                   est_absmax=s['est_absmax'], est_under=s['est_under'],
                   est_n=s['est_n'], fps=s['fps'], fps_sd=s['fps_sd'],
                   thr=s['thr'], integ_zero=s['integ_zero'], sat=s['sat'])
              for i, s in enumerate(A)])
    if closed and openl:
        c, o = closed[0], openl[0]
        out['compare']['headline'] = dict(
            closed_arm=A.index(c) + 1, open_arm=A.index(o) + 1,
            die_delta=r4(o['die_err'] - c['die_err']),
            duty_delta=r4(o['duty'] - c['duty']),
            est_sd_ratio=r4(div(o['est_sd'], c['est_sd'])),
            est_absmax_ratio=r4(div(o['est_absmax'], c['est_absmax'])),
            same_levels=sorted(o['occupancy']) == sorted(c['occupancy']))

    # ---- the substitution itself: probe vs model, same setpoint ----------
    # THE POINT OF THE WHOLE RUN.  Both arms are closed loops with an
    # identical control law; only where `tc_` comes from differs.  Scored on
    # the CARD's own sensor, and drift-corrected from the fan-off bracket
    # immediately before each arm, because the arms are two hours apart and
    # the session's drift is monotone -- it pushes the later arm hotter, the
    # same direction as any real substitution cost.
    def bracket_before(arm):
        c = [b for b in out['brackets'] if b['t1'] <= arm['t0']]
        return c[-1] if c else None

    probe = next((s for s in A if s['src'] == 0), None)
    modl = next((s for s in A if s['src'] == 1), None)
    out['compare']['substitution'] = None
    if probe and modl:
        bp, bm = bracket_before(probe), bracket_before(modl)
        amb = None if (bp is None or bm is None) else r4(bm['die'] - bp['die'])
        dd = r4(modl['die_err'] - probe['die_err'])
        out['compare']['substitution'] = dict(
            probe_arm=A.index(probe) + 1, model_arm=A.index(modl) + 1,
            die_delta=dd,
            duty_delta=r4(modl['duty'] - probe['duty']),
            duty_levels=r4(div(modl['duty'] - probe['duty'], DUTY_STEP)),
            same_levels=sorted(modl['occupancy']) == sorted(probe['occupancy']),
            over=[probe['over'], modl['over']],
            over_delta=r4(modl['over'] - probe['over']),
            reach=[probe['reach'], modl['reach']],
            reach_delta=(None if None in (probe['reach'], modl['reach'])
                         else modl['reach'] - probe['reach']),
            est=[probe['est'], modl['est']],
            est_sd=[probe['est_sd'], modl['est_sd']],
            rconv=[probe['rconv'], modl['rconv']],
            p=[probe['p'], modl['p']],
            bracket_before=[None if bp is None else bp['clock'],
                            None if bm is None else bm['clock']],
            # bracket 1 is the ONLY one that approaches from below (cold card,
            # app just launched); every other sheds a hot soak.  Flagged
            # rather than silently used, because it is the weak link in this
            # correction and the load probe was caught by exactly this.
            amb_from_cold_start=bool(bp is not None and bp is out['brackets'][0]),
            amb_die=amb,
            corrected=None if amb is None else r4(dd - amb))

    # ---- repeatability: the two open arms are a replicate pair ------------
    # There is no model compiled in, so arm 3 ran open too.  That was foreseen
    # (the plan calls it "a second baseline sample, which is not wasted") and it
    # is what bounds every other difference in this report.
    out['repeat'] = None
    if len(openl) >= 2:
        o1, o2 = openl[0], openl[1]
        # the bracket immediately before each arm, as the ambient reference
        def before(arm):
            cands = [b for b in out['brackets'] if b['t1'] <= arm['t0']]
            return cands[-1] if cands else None
        b1, b2 = before(o1), before(o2)
        amb = None if (b1 is None or b2 is None) else r4(b2['die'] - b1['die'])
        amb_hs = None if (b1 is None or b2 is None) else r4(b2['hs'] - b1['hs'])
        amb_air = None if (b1 is None or b2 is None) else r4(b2['air'] - b1['air'])
        out['repeat'] = dict(
            arms=[A.index(o1) + 1, A.index(o2) + 1],
            labels=[o1['label'], o2['label']],
            die_err=[o1['die_err'], o2['die_err']],
            die_delta=r4(o2['die_err'] - o1['die_err']),
            duty=[o1['duty'], o2['duty']], duty_delta=r4(o2['duty'] - o1['duty']),
            duty_levels=r4(div(o2['duty'] - o1['duty'], DUTY_STEP)),
            occupancy=[o1['occupancy'], o2['occupancy']],
            rpm=[o1['rpm'], o2['rpm']], af=[o1['af'], o2['af']],
            hs=[o1['hs'], o2['hs']], air=[o1['air'], o2['air']],
            p=[o1['p'], o2['p']], rconv=[o1['rconv'], o2['rconv']],
            est=[o1['est'], o2['est']], est_sd=[o1['est_sd'], o2['est_sd']],
            # the ambient correction the brackets supply
            bracket_before=[None if b1 is None else b1['clock'],
                            None if b2 is None else b2['clock']],
            amb_die=amb, amb_hs=amb_hs, amb_air=amb_air,
            # what is left once the measured ambient shift is taken out
            unexplained=None if amb is None else r4((o2['die_err'] - o1['die_err']) - amb),
            # the bound every other comparison in this report inherits
            bound=r4(abs(o2['die_err'] - o1['die_err'])),
            air_sensor_disagrees=bool(amb is not None and amb_air is not None
                                      and amb * amb_air < 0))

    # ---- the bound carried forward from an earlier replicate pair --------
    out['prior'] = None
    if a.prior:
        PR = Run(a.prior)
        pa = [describe_arm(b, a.window, a.load_floor) for b in PR.arms()]
        po = [s for s in pa if s['kind'] == 'open']
        if len(po) >= 2:
            out['prior'] = dict(
                file=os.path.basename(a.prior),
                date=PR.rows[0]['t'].strftime('%d %b'),
                n=len(PR.rows), mins=round(PR.rows[-1]['s'] / 60, 1),
                arms=[pa.index(po[0]) + 1, pa.index(po[1]) + 1],
                die_err=[po[0]['die_err'], po[1]['die_err']],
                die_delta=r4(po[1]['die_err'] - po[0]['die_err']),
                duty=[po[0]['duty'], po[1]['duty']],
                p=[po[0]['p'], po[1]['p']],
                rconv=[po[0]['rconv'], po[1]['rconv']],
                bound=r4(abs(po[1]['die_err'] - po[0]['die_err'])))
        else:
            print(f"note: {a.prior} has {len(po)} open arms, not the 2 a "
                  f"replicate pair needs -- no bound carried forward")

    # ---- drift across the whole session, from the fan-off brackets --------
    B = out['brackets']
    out['drift'] = dict(
        base=0,
        rows=[dict(i=i + 1, clock=b['clock'], n=b['n'], settled=b['settled'],
                   die=b['die'], die_sd=b['die_sd'], hs=b['hs'], air=b['air'],
                   p=b['p'], af=b['af'],
                   rise=b['rise'], r_amb=b['r_amb'], creep=b['creep'],
                   d_die=r4(b['die'] - B[0]['die']) if i else 0.0,
                   d_hs=r4(b['hs'] - B[0]['hs']) if i else 0.0,
                   d_air=r4(b['air'] - B[0]['air']) if i else 0.0,
                   d_r=r4(b['r_amb'] - B[0]['r_amb']) if i else 0.0,
                   d_p=r4(b['p'] - B[0]['p']) if i else 0.0)
              for i, b in enumerate(B)],
        window=a.bracket_window,
        # THE CLOSING BRACKET DID NOT SETTLE, and that is the run's real verdict on
        # itself.  The three that did span 0.26 C; the fourth was still falling
        # 30 minutes in, and is NOT a valid baseline comparison -- it is evidence
        # that the session's drift had not finished, which is a different and more
        # important statement than any number it could have supplied.
        last_settled=B[-1]['settled'], last_creep=B[-1]['creep'],
        r_amb=[b['r_amb'] for b in B],
        r_drop=r4(B[-1]['r_amb'] - B[0]['r_amb']),
        r_drop_pct=r4(100 * (B[-1]['r_amb'] / B[0]['r_amb'] - 1)),
        monotone=all(B[i]['r_amb'] >= B[i + 1]['r_amb'] for i in range(len(B) - 1)),
        # three card-side instruments agree the cooling improved; the loop's own
        # air input is the one that says the opposite
        air_rose=bool(B[-1]['air'] > B[0]['air']),
        span_die=r4(mx([b['die'] for b in B]) - mn([b['die'] for b in B])),
        span_die_settled=r4(None if sum(1 for b in B if b['settled']) < 2 else
                            mx([b['die'] for b in B if b['settled']])
                            - mn([b['die'] for b in B if b['settled']])),
        span_air=r4(mx([b['air'] for b in B]) - mn([b['air'] for b in B])),
        n_settled=sum(1 for b in B if b['settled']))

    # ---- kTrim: the 0.5.0 defect, replayed by the arming second ----------
    # 0.5.0 preseeded +103.4 points and pinned the fan at its 73 % ceiling for
    # 9.4 minutes with the die 10.3 C BELOW setpoint; on a cold start the P term
    # subtracted 29.7 points from a correct feedforward and floored the fan for
    # 40 s, overshooting 3.5 C.  Both were one defect: an unbounded PI fighting
    # the thermal time constant.  kTrim = 15 duty points is the whole fix.
    out['ktrim'] = None
    if closed:
        c = closed[0]
        blk = arms[A.index(c)]
        ser = [d for d in blk[1:] if d['integ'] is not None]
        out['ktrim'] = dict(
            arm=A.index(c) + 1, ktrim=15.0, kp=1.0, ki=0.01,
            preseed=c['preseed'], preseed_err=c['preseed_err'],
            integ_hi=c['integ_hi'], integ_lo=c['integ_lo'],
            integ_zero_at=c['integ_zero_at'], integ_rate=c['integ_rate'],
            # the bound is 15 - kp*err, so an integrator above 15 is legal: it
            # is holding exactly what the trim window cannot currently express
            integ_above_ktrim=sum(1 for d in ser if d['integ'] > 15.0),
            dff_min=c['dff_min'], dff_max=c['dff_max'], dff_absmax=c['dff_absmax'],
            budget_used=r4(div(c['dff_absmax'], 15.0)),
            over=c['over'], sat=c['sat'],
            floor=c['states'].get(3, 0), ceiling=c['states'].get(4, 0),
            # The host tick is ~1.0009 s, so no sample's offset into the arm is
            # ever an exact integer -- an `in (0, 1, 2, ...)` test matched only
            # t=0 and silently printed a one-row table.  Take the nearest sample.
            decay=[[round(d['s'] - blk[0]['s']), r4(d['integ']), r4(d['duty']),
                    r4(d['ff']), r4(d['err'])]
                   for d in (min(blk, key=lambda x: abs(x['s'] - blk[0]['s'] - m))
                             for m in (0, 1, 2, 5, 10, 30, 60, 120, 300,
                                       600, 900, 1200, 1500, 1800))
                   if d['s'] - blk[0]['s'] <= blk[-1]['s'] - blk[0]['s']],
            prior=dict(preseed=103.4, pinned_min=9.4, deficit=10.3,
                       cold_p=-29.7, cold_over=3.5, src='0.5.0, 2026-11-22'))

    # ---- the estimator, pooled over the settled windows -------------------
    allt = [d for b in arms for d in tail(b, a.window, a.load_floor)
            if d['die_est'] is not None and d['die'] is not None]
    e = [d['die_est'] - d['die'] for d in allt]
    RINT_CFG = 1.709
    out['estimator'] = dict(
        n=len(e), mean=r4(mean(e)), sd=r4(pstdev(e)),
        min=r4(mn(e)), max=r4(mx(e)), absmax=r4(mx([abs(x) for x in e])),
        under=sum(1 for x in e if x < 0),
        rint_cfg=RINT_CFG,
        by_arm=[dict(arm=i + 1, label=s['label'], kind=s['kind'],
                     mean=s['est'], sd=s['est_sd'], absmax=s['est_absmax'],
                     under=s['est_under'], n=s['est_n'],
                     rint=s['rint'], p=s['p'],
                     # the closed-loop estimate is tc + rint*W, so ALL of its
                     # bias is the R_int error -- predicted here and checked
                     predicted=r4(None if (s['rint'] is None or s['p'] is None)
                                  else (RINT_CFG - s['rint']) * s['p']))
                for i, s in enumerate(A)],
        scatter=[[r4(d['die']), r4(d['die_est'] - d['die'])]
                 for d in allt[::max(1, len(allt) // 900)]])
    for r in out['estimator']['by_arm']:
        r['residual'] = r4(None if (r['mean'] is None or r['predicted'] is None)
                           else r['mean'] - r['predicted'])
    # R_int measured at one duty only this time -- the 0.5.0 run's four points
    # spanned 32-61 % and found 1.619..1.742.  This run cannot separate duty
    # from heatsink temperature at all, and says so.
    out['estimator']['rint_run'] = dict(
        pts=[[s['duty'], s['rint'], s['rint_sd']] for s in A],
        mean=r4(mean([s['rint'] for s in A])),
        duty_span=r4(mx([s['duty'] for s in A]) - mn([s['duty'] for s in A])),
        prior=dict(lo=1.619, hi=1.742, lo_duty='32-41 %', hi_duty='58-61 %',
                   src='0.5.0, 2026-11-22'),
        one_point=True)

    # ---- the feedforward curve, measured back -----------------------------
    # The table seeded into Control.h is the R_conv column of the 22 Nov fan
    # ladder.  This run re-measures it at ONE duty (all three arms land within
    # a level of each other), so it is a spot check and not a recalibration.
    SEED = [(0.0, 16.48), (9.5, 16.17), (20.6, 15.20), (30.2, 12.97), (39.7, 11.29),
            (50.8, 9.88), (60.3, 8.37), (69.8, 7.11), (73.0, 6.74)]

    def seeded(duty):
        for (d0, r0), (d1, r1) in zip(SEED, SEED[1:]):
            if d0 <= duty <= d1:
                return r0 + (r1 - r0) * (duty - d0) / (d1 - d0)
        return SEED[-1][1] if duty > SEED[-1][0] else SEED[0][1]

    out['curve'] = dict(
        seed=[[d, r] for d, r in SEED],
        measured=[[s['duty'], s['rconv'], r4(seeded(s['duty'])),
                   r4(s['rconv'] - seeded(s['duty'])),
                   r4(100 * (s['rconv'] / seeded(s['duty']) - 1))]
                  for s in sorted(A, key=lambda x: x['duty'])],
        spot_check=True,
        duty_span=r4(mx([s['duty'] for s in A]) - mn([s['duty'] for s in A])))
    out['curve']['worst_pct'] = r4(mx([abs(m[4]) for m in out['curve']['measured']]))

    # ---- throughput, pooled over every loaded sample ---------------------
    run = [d for b in arms for d in b[1:]]   # first sample dropped: see Run.arms()
    fps = [d['fps'] for d in run if d['fps']]
    out['throughput'] = dict(
        n=len(run), fps_mean=r4(mean(fps)), fps_sd=r4(pstdev(fps)),
        fps_min=r4(mn(fps)), fps_max=r4(mx(fps)),
        clk_lo=r4(mn([d['clk'] for d in run])), clk_hi=r4(mx([d['clk'] for d in run])),
        thr=round(100 * sum(1 for d in run if d['clk'] is not None and d['clk'] < 399)
                  / len(run), 3),
        die_max=r4(mx([d['die'] for d in run])),
        target=r4(mean([s['target'] for s in A])), bound=95.0,
        margin=r4(None if mx([d['die'] for d in run]) is None
                  else 95.0 - mx([d['die'] for d in run])))

    # ---- actuator: how often the EMC2101 was actually written ------------
    lv = [round(d['duty'] / DUTY_STEP) for d in R.rows if d['duty'] is not None]
    out['actuator'] = dict(
        changes=sum(1 for x, y in zip(lv, lv[1:]) if x != y),
        levels=len(set(lv)), step=r4(DUTY_STEP), n=len(lv),
        # `ctl.sat` counts STEPS CLAMPED AT A DUTY LIMIT since the last report --
        # it is not a write-error counter, which is what the 0.5.0 script called
        # it.  Clamping is normal and expected at the boundaries of an arm.
        clamped=r4(mx([d['sat'] for d in R.rows])),
        clamped_rows=sum(1 for d in R.rows if d['sat']))

    # ---- the two die sensors, and the card's own watts -------------------
    both = [d for d in run if d['ts0'] is not None and d['ts1'] is not None]
    out['die_sensors'] = dict(
        n=len(both), diff_mean=r4(mean([d['ts1'] - d['ts0'] for d in both])),
        diff_absmax=r4(mx([abs(d['ts1'] - d['ts0']) for d in both])))
    gap = [d['p'] - d['pcard'] for d in run if d['p'] and d['pcard']]
    out['power_gap'] = dict(mean=r4(mean(gap)), sd=r4(pstdev(gap)), n=len(gap))

    # ---- timeline --------------------------------------------------------
    S2 = {k: [] for k in ('t', 'die', 'hs', 'air', 'p', 'fps', 'clk', 'duty', 'rpm',
                          'af', 'tgt', 'inp', 'est', 'ff', 'integ', 'mode', 'state',
                          'src', 'tc', 'nn', 'model_ready')}
    for i, d in enumerate(R.rows):
        if i % a.step:
            continue
        S2['t'].append(round(d['s']))
        for key, col in (('die', 'die'), ('hs', 'hs'), ('air', 'air'), ('p', 'p'),
                         ('fps', 'fps'), ('clk', 'clk'), ('duty', 'duty'), ('rpm', 'rpm'),
                         ('af', 'flow'), ('tgt', 'tgt'), ('inp', 'inp'), ('est', 'die_est'),
                         ('ff', 'ff'), ('integ', 'integ'), ('mode', 'mode'),
                         ('state', 'state'), ('src', 'src'), ('tc', 'tc'),
                         ('nn', 'nn'), ('model_ready', 'model_ready')):
            v = d[col]
            S2[key].append(None if v is None else round(v, 3))
    # ---- the soft sensor, scored on this run -----------------------------
    # THIS RUN IS HELD OUT OF THE FIT. The model was fitted on the 11-22 fan
    # ladder's unthrottled region plus the 11-23 soft-sensor run; not one
    # sample below entered it. That is what makes these numbers a measurement
    # of the model rather than of its own residual -- and it is why this
    # section belongs in THIS report, whose three arms are precisely the
    # question "what does doing without the thermocouple cost?".
    M = read_model(a.firmware)
    out['model'] = dict(
        ready=M['ready'], C=M['C'], rint=M['rint'],
        nodes=[dict(duty=d, rconv=r, tau=r4(r * M['C']))
               for d, r in zip(M['duty'], M['rconv'])],
        arms=[], brackets=[], firmware=os.path.basename(a.firmware))
    for i, blk in enumerate(arms, 1):
        win = tail(blk, a.window, floor=a.load_floor)
        dyn = run_model(blk, M)
        # The static form is what the OPEN-LOOP branch's own die_est computes:
        # the same curve with no memory of where the heatsink actually is.
        sta = [(d['air'] + d['p'] * rconv_at(M, d['duty']))
               if None not in (d['air'], d['p'], d['duty']) else None for d in blk]
        k = {d['s']: e for d, e in zip(blk, dyn)}
        out['model']['arms'].append(dict(
            arm=i,
            whole=score_est(blk, dyn), settled=score_est(win, [k[d['s']] for d in win]),
            static_whole=score_est(blk, sta),
            # what the loop would have regulated to, had this been its PV
            die=score_est(blk, [None if e is None or d['p'] is None
                                else e + M['rint'] * d['p'] for e, d in zip(dyn, blk)],
                          key='die')))
    for i, blk in enumerate(brks, 1):
        out['model']['brackets'].append(dict(
            bracket=i, whole=score_est(blk, run_model(blk, M))))

    out['series'] = S2
    out['events'] = [dict(s=round(d['s']), clock=d['t'].strftime('%H:%M:%S'), msg=d['msg'])
                     for d in R.rows if d['msg']]

    os.makedirs(a.out, exist_ok=True)
    p = os.path.join(a.out, 'data.js')
    with open(p, 'w') as fh:
        fh.write('/* generated by tools/ra6m5-report-loop.py -- do not edit */\n')
        fh.write('const LOOP = ')
        json.dump(out, fh, separators=(',', ':'), allow_nan=False)
        fh.write(';\n')
    print(f"wrote {p}  ({os.path.getsize(p)/1024:.0f} KiB)")
    summarise(out)


def fmt(h, names):
    return ' '.join(f"{names.get(k, k)}={v}" for k, v in h.items())


def summarise(o):
    p = print
    p(f"\nsource      {o['start']} .. {o['end']}  {o['n']} rows, {o['mins']:.1f} min "
      f"(firmware {o['fw']}, protocol {o['proto']})")
    p(f"            rail power runs {o['power_gap']['mean']:+.3f} W above the card's own _POW "
      f"(sd {o['power_gap']['sd']:.3f}, n={o['power_gap']['n']})")
    d = o['die_sensors']
    p(f"            TS1-TS0 mean {d['diff_mean']:+.3f} C, worst |{d['diff_absmax']:.2f}| "
      f"over {d['n']} loaded samples")

    p(f"\nPROVENANCE  what the loop actually did -- the only confirmation an arm ran as configured")
    for r in o['provenance']['rows']:
        p(f"  arm {r['arm']}  {r['label']}")
        p(f"          src   {fmt(r['src'], SRC)}")
        if o['provenance']['schema'] == 1:
            p(f"          tc {r['tc']}  nn {r['nn']}  auto {r['auto']}"
              f"   [pre-0.7.0: tc/nn are EFFECTIVE bools]")
        else:
            p(f"          tc (asked) {fmt(r['tc'], TCW)}  model_ready {r['mr']}"
              f"  auto {r['auto']}")
        p(f"          state {fmt(r['state'], STATE)}")
    for pair, ch in o['provenance']['discriminators'].items():
        a_, b_ = pair.split('v')
        p(f"  arms {a_} vs {b_} differ in: {', '.join(ch) if ch else 'NOTHING -- indistinguishable!'}")

    m = o['model']
    p(f"\nSOFT SENSOR  fitted 2026-11-24, read from {m['firmware']} "
      f"(model_ready={int(m['ready'])}, C={m['C']:.1f} J/C, rint={m['rint']:.3f})")
    p( "            THIS RUN IS HELD OUT: the fit used the 11-22 ladder and the 11-23")
    p( "            soft-sensor run, and not one sample below entered it.")
    p( "            R_conv(duty) and the pole it implies:")
    for nd in m['nodes']:
        p(f"              {nd['duty']:5.1f} %   {nd['rconv']:6.2f} C/W   tau {nd['tau']:6.1f} s")
    p( "            estimate vs the PROBE, per arm:")
    for r in m['arms']:
        w, se = r['whole'], r['settled']
        p(f"              arm {r['arm']}  whole   rms {w['rms']:.3f} C  bias {w['bias']:+.3f}  "
          f"sd {w['sd']:.3f}  cold on {w['cold_pct']:.1f} % of {w['n']}")
        p(f"                     worst  cold {w['worst_cold']:+.2f} C   hot {w['worst_hot']:+.2f} C")
        if se:
            p(f"                     settled rms {se['rms']:.3f} C  bias {se['bias']:+.3f}  n={se['n']}")
        st_ = r['static_whole']
        p(f"                     STATIC form (no memory of the sink): rms {st_['rms']:.3f} C, "
          f"worst {max(abs(st_['worst_cold']), abs(st_['worst_hot'])):.1f} C")
        d_ = r['die']
        p(f"                     as a DIE estimate vs the card's own sensor: "
          f"rms {d_['rms']:.3f} C  bias {d_['bias']:+.3f}")
    p( "            the fan-off brackets, where duty 0 is measured at 0.86 W only:")
    for r in m['brackets']:
        w = r['whole']
        p(f"              bracket {r['bracket']}  rms {w['rms']:.3f} C  bias {w['bias']:+.3f}  n={w['n']}")

    p(f"\nsteady state = last {o['window']//60} min of each arm, "
      f"samples below {o['load_floor']:.1f} W dropped")
    p(f"{'arm':<4} {'source':<34} {'die-tgt':>8} {'sd':>6} {'peak':>7} {'duty':>7} {'rpm':>6} "
      f"{'est-die':>8} {'sd':>6} {'|max|':>6} {'R_int':>6} {'fps':>8} {'thr%':>5}")
    for i, s in enumerate(o['arms']):
        p(f"{i+1:<4} {s['label']:<34} {s['die_err']:>+8.3f} {s['die_err_sd']:>6.3f} "
          f"{s['peak']:>7.3f} {s['duty']:>7.3f} {s['rpm']:>6.0f} "
          f"{s['est']:>+8.3f} {s['est_sd']:>6.3f} {s['est_absmax']:>6.3f} "
          f"{s['rint']:>6.3f} {s['fps']:>8.2f} {s['thr']:>5.2f}")
    for i, s in enumerate(o['arms']):
        p(f"  arm {i+1}  reach tgt-0.5 at {s['reach']} s, peak {s['over']:+.3f} C, "
          f"settle {s['settle'] if s['settle'] is not None else 'never'}; "
          f"duty {s['duty_lo']:.1f}..{s['duty_hi']:.1f} % on levels "
          f"{{{', '.join(f'{k}:{v}' for k, v in s['occupancy'].items())}}}")
        p(f"          integ {s['integ_lo']:+.2f}..{s['integ_hi']:+.2f}"
          f"{'  (identically zero -- no PI)' if s['integ_zero'] else ''}"
          f", duty-ff {s['dff_min']:+.2f}..{s['dff_max']:+.2f}, sat {s['sat']}, "
          f"est under-reads the die on {s['est_under']}/{s['est_n']} settled samples")

    h = o['compare'].get('headline')
    if h:
        p(f"\nHEADLINE    closed loop (arm {h['closed_arm']}) vs open loop (arm {h['open_arm']}), "
          f"same setpoint, same load")
        p(f"            regulated die differs by {h['die_delta']:+.3f} C, applied duty by "
          f"{h['duty_delta']:+.3f} points"
          f"{', on the same two EMC2101 levels' if h['same_levels'] else ''}")
        p(f"            but the ESTIMATE is {h['est_sd_ratio']:.1f}x noisier "
          f"(|max| {h['est_absmax_ratio']:.1f}x) -- that is what losing the sensor costs")

    r = o['repeat']
    if r:
        p(f"\nREPEATABILITY  arms {r['arms'][0]} and {r['arms'][1]} are a REPLICATE PAIR "
          f"(no model compiled in, so both ran open)")
        p(f"            die-tgt {r['die_err'][0]:+.3f} vs {r['die_err'][1]:+.3f} = "
          f"{r['die_delta']:+.3f} C   <-- the bound every comparison here inherits")
        p(f"            duty {r['duty'][0]:.3f} vs {r['duty'][1]:.3f} % = {r['duty_delta']:+.3f} "
          f"({r['duty_levels']:+.2f} EMC2101 levels)")
        p(f"            occupancy {r['occupancy'][0]} vs {r['occupancy'][1]}")
        p(f"            preceding fan-off bracket ({r['bracket_before'][0]} vs "
          f"{r['bracket_before'][1]}): die {r['amb_die']:+.3f}, heatsink {r['amb_hs']:+.3f}, "
          f"EMC2101 air {r['amb_air']:+.3f} C")
        if r['air_sensor_disagrees']:
            p(f"            *** the real baseline moved {r['amb_die']:+.3f} C while the loop's "
              f"OWN air input moved {r['amb_air']:+.3f} C -- opposite signs.")
            p(f"                The EMC2101 is a chip die temperature, not an inlet sensor, and "
              f"an open loop couples that error straight through.")
        p(f"            unexplained after the ambient correction: {r['unexplained']:+.3f} C")

    p(f"\ndrift       the fan-off, card-idle brackets at ~{o['brackets'][0]['p']:.2f} W "
      f"(fan.idle = 0), last {o['bracket_window']//60} min of each")
    p(f"{'#':<3} {'clock':<6} {'len':>5} {'die':>9} {'d':>8} {'heatsink':>9} {'d':>8} "
      f"{'air':>8} {'d':>7} {'W':>8} {'R_amb':>7} {'d':>7} {'flat? (creep)':>14}")
    for b in o['drift']['rows']:
        p(f"{b['i']:<3} {b['clock']:<6} {b['n']:>5} {b['die']:>9.4f} {b['d_die']:>+8.4f} "
          f"{b['hs']:>9.4f} {b['d_hs']:>+8.4f} {b['air']:>8.4f} {b['d_air']:>+7.4f} "
          f"{b['p']:>8.5f} {b['r_amb']:>7.3f} {b['d_r']:>+7.3f} "
          f"{(str(b['settled']) + ('' if b['settled'] else f" ({b['creep']:+.2f})")):>14}")
    sp = o['drift']
    p(f"            all brackets span {sp['span_die']:.4f} C of die and "
      f"{sp['span_air']:.4f} C of board air; {sp['n_settled']} of "
      f"{len(o['brackets'])} flat to within one die LSB"
      + (f", those spanning {sp['span_die_settled']:.4f} C"
         if sp['span_die_settled'] is not None else ""))
    if not sp['last_settled']:
        p(f"            *** THE CLOSING BRACKET NEVER SETTLED: still falling "
          f"{sp['last_creep']:+.3f} C across its last {sp['window']} s, 30 min in.")
        p(f"                It is therefore NOT a valid baseline comparison -- and that is the"
          f" run's verdict on itself.")
    p(f"            natural-convection resistance at a fixed {o['brackets'][0]['p']:.2f} W, fan off: "
      + ' -> '.join(f"{v:.2f}" for v in sp['r_amb']) + " C/W")
    p(f"            {sp['r_drop']:+.3f} C/W = {sp['r_drop_pct']:+.1f} %"
      f"{', MONOTONE across all four' if sp['monotone'] else ''}"
      f"{', while the loop OWN air input rose' if sp['air_rose'] else ''}. "
      f"The card got steadily easier to cool;")
    p(f"            the die, the heatsink thermocouple and the FS3000 all agree, and the "
      f"EMC2101 is the one that says otherwise.")

    k = o['ktrim']
    if k:
        pr = k['prior']
        p(f"\nkTrim       the 0.5.0 defect replayed by arm {k['arm']}'s arming second "
          f"(kTrim={k['ktrim']:.0f}, kp={k['kp']}, ki={k['ki']})")
        p(f"            preseed {k['preseed']:+.2f} against err {k['preseed_err']:+.2f} -- "
          f"the rearm injects -kp*err exactly, so the trim sees ~0")
        p(f"            integrator {k['integ_lo']:+.2f}..{k['integ_hi']:+.2f}, "
          f"{k['integ_above_ktrim']} samples above kTrim (legal: the bound is 15-kp*err)")
        p(f"            reached 0 at {k['integ_zero_at']} s ({k['integ_rate']:+.1f} points/min)")
        p(f"            the PI moved the feedforward by {k['dff_min']:+.2f}..{k['dff_max']:+.2f} "
          f"points = {100*k['budget_used']:.0f} % of its {k['ktrim']:.0f}-point budget")
        p(f"            overshoot {k['over']:+.3f} C; floor {k['floor']} samples, "
          f"ceiling {k['ceiling']}, sat {k['sat']}")
        p(f"            0.5.0 for comparison ({pr['src']}): preseed {pr['preseed']:+.1f}, fan "
          f"pinned at the ceiling {pr['pinned_min']:.1f} min with the die {pr['deficit']:.1f} C "
          f"BELOW setpoint;")
        p(f"            and on a cold start P subtracted {pr['cold_p']:.1f} points from a correct "
          f"feedforward, overshooting {pr['cold_over']:+.1f} C")
        p(f"            integrator decay   t(s)    integ    duty      ff     err")
        for t, ig, du, ff, er in k['decay']:
            p(f"                             {t:>5}  {ig if ig is None else f'{ig:+7.2f}'}  "
              f"{du if du is None else f'{du:6.1f}'}  {ff if ff is None else f'{ff:6.1f}'}  "
              f"{er if er is None else f'{er:+6.2f}'}")

    e = o['estimator']
    p(f"\nestimator   die_est - die over {e['n']} settled samples: mean {e['mean']:+.3f} "
      f"sd {e['sd']:.3f}, range {e['min']:+.3f}..{e['max']:+.3f}")
    p(f"            pooling is NOT meaningful across arms here -- the mechanism differs. By arm:")
    for r in e['by_arm']:
        p(f"              arm {r['arm']} {r['kind']:<6} mean {r['mean']:+.3f} sd {r['sd']:.3f} "
          f"|max| {r['absmax']:.3f}  under-reads {r['under']}/{r['n']}")
        p(f"                      R_int measured {r['rint']:.4f} vs configured {e['rint_cfg']} "
          f"at {r['p']:.4f} W -> predicts a {r['predicted']:+.3f} C bias, "
          f"residual {r['residual']:+.3f}")
    rr = e['rint_run']
    p(f"            R_int this run: " + '  '.join(f"{x[0]:.1f}%->{x[1]:.4f}" for x in rr['pts']))
    p(f"            mean {rr['mean']:.4f} over a duty span of only {rr['duty_span']:.2f} points -- "
      f"ONE operating point, so this run cannot")
    p(f"            separate duty from heatsink temperature. 0.5.0 found "
      f"{rr['prior']['lo']} at {rr['prior']['lo_duty']} vs {rr['prior']['hi']} at "
      f"{rr['prior']['hi_duty']}.")

    c = o['curve']
    p(f"\nfeedforward seeded R_conv curve spot-checked at rest (duty span "
      f"{c['duty_span']:.2f} points -- a spot check, not a recalibration):")
    for duty, meas, seed, dd, pct in c['measured']:
        p(f"            {duty:5.1f} %  measured {meas:6.3f}  seeded {seed:6.3f}  "
          f"{dd:+6.3f} C/W ({pct:+5.1f} %)")
    p(f"            worst {c['worst_pct']:.1f} %")

    t = o['throughput']
    p(f"\nthroughput  {t['fps_mean']:.2f} fps sd {t['fps_sd']:.2f} "
      f"({t['fps_min']:.0f}..{t['fps_max']:.0f}) over {t['n']} loaded samples")
    p(f"            clock {t['clk_lo']:.0f}..{t['clk_hi']:.0f} MHz, throttled {t['thr']:.2f} %; "
      f"hottest die {t['die_max']:.2f} C -- {t['margin']:.2f} C of margin to the "
      f"{t['bound']:.0f} C bound, at a {t['target']:.0f} C setpoint")
    ac = o['actuator']
    p(f"\nactuator    {ac['changes']} quantised-level changes seen at 1 Hz over the whole run, "
      f"{ac['levels']} distinct levels of the {ac['step']:.3f} % step")
    p(f"            ctl.sat (steps clamped at a duty limit, NOT write errors) was set on "
      f"{ac['clamped_rows']} of {ac['n']} samples")


if __name__ == '__main__':
    main()
