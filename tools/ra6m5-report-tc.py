#!/usr/bin/env python3
"""Generate the data file for docs/reports/ra6m5-thermocouple/.

    tools/ra6m5-report-tc.py \
        --run  docs/data/ra6m5-thermal-hailo8-20261122.csv \
        --prev docs/data/ra6m5-thermal-hailo8-20261121.csv \
        --out  docs/reports/ra6m5-thermocouple

Writes `data.js` (one `const TC = {...}`) and prints a summary of every scalar
the page's prose quotes, so the prose can be checked against the data rather
than remembered.  `index.html` renders the tread table and all charts from this
file; nothing in the page re-derives a number.

Conventions `index.html` depends on -- the same ones as
`tools/ra6m5-report-data.py`, for the same reasons:

  * `series` is DECIMATED (every Nth row, not averaged) with `t` in seconds
    from the first row of the file, so the timeline's cursor lands on a real
    sample.  Averaging would smooth away the throttle dither, which is the
    thing worth seeing.
  * `die` is the HOTTEST on-die sensor, whatever the card calls it -- here
    max(TS0, TS1).  A mean would bury the sensor that actually throttles.
  * The first benchmarked row is dropped everywhere except the timeline: the
    engine's first sample divides a frame-counter delta by a partial interval
    and reads high.  See "Discard the first BenchEngine::sample()" in CLAUDE.md.
  * A missing reading is JSON `null`, never 0.  `0.0 W` is a real INA228
    overflow signal and 0 °C is a plausible temperature.
  * Power is the INA228 RAIL power (`_INA228_POWER`), never the card's own
    `_POW`.  Thermal resistance needs what the card dissipates, and the rail is
    the whole of it; `_POW` is the chip's own estimate of a subset (it runs
    ~0.18 W lower under load).  Never substitute one for the other.

The one thing this report adds over the two-run study: with a thermocouple on
the heatsink, the die-to-air resistance splits into two series resistances that
can be reported separately --

    R_int  = (die - heatsink) / P      junction to heatsink, incl. on-die spreading
    R_conv = (heatsink - air) / P      heatsink to board air, what the fan changes
    R_tot  = (die - air) / P           what the earlier report could only lump

`air` is the EMC2101's own die temperature, used as the board-air proxy exactly
as the earlier report used it -- it is the same sensor on the same board, so the
two reports' R_tot figures are comparable.
"""
import argparse, csv, json, math, os, statistics as st
from datetime import datetime


class Run:
    """One session CSV.  Columns are resolved by SUFFIX, because the BDF and the
    USB port path in each name move across reboots (CLAUDE.md, 'Host specifics')."""

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
                ts0=c('_TS0'), ts1=c('_TS1'), air=c('_EMC2101'), hs=c('_MAX31855', False),
                p=c('_INA228_POWER'), clk=c('_CLK'), flow=c('_AIRFLOW'),
                rpm=c('_FAN_RPM'), duty=c('_FAN_DUTY'),
            )
            self.rows = []
            for row in r:
                d = {'t': datetime.fromisoformat(row[0]), 'st': row[ix['bench_state']]}
                v = row[ix['hailo_fps']]
                d['fps'] = float(v) if v.strip() else None
                for k, i in cols.items():
                    if i is None:
                        d[k] = None
                        continue
                    v = row[i]
                    d[k] = float(v) if v.strip() else None
                dies = [x for x in (d['ts0'], d['ts1']) if x is not None]
                d['die'] = max(dies) if dies else None
                self.rows.append(d)
        self.t0 = self.rows[0]['t']
        self.load = [r for r in self.rows if r['st'] == 'running'][1:]   # drop first sample
        self.cool = [r for r in self.rows if r['st'] == 'stopped']
        self.has_tc = any(r['hs'] is not None for r in self.rows)

    def secs(self, r):
        return (r['t'] - self.t0).total_seconds()

    # ---- helpers -------------------------------------------------------
    @staticmethod
    def mean(rows, k):
        v = [r[k] for r in rows if r.get(k) is not None]
        return sum(v) / len(v) if v else None

    def treads(self, settle=60):
        """Group the load window by distinct FAN_DUTY readback.

        The plan asks in exact 1/63 steps so request and readback agree, but it
        is the READBACK that is grouped on -- the EMC2101 quantises to 64 levels
        and the board's own figure is the only one that is true (CLAUDE.md).
        Each tread is summarised over its last `settle` seconds, so the figure
        is an equilibrium rather than the lag into it.
        """
        groups, cur = [], None
        for r in self.load:
            k = round(r['duty'], 1)
            if cur is None or k != cur['duty']:
                cur = {'duty': k, 'rows': []}
                groups.append(cur)
            cur['rows'].append(r)
        out = []
        for g in groups:
            rows = g['rows']
            tail = [r for r in rows if (rows[-1]['t'] - r['t']).total_seconds() <= settle]
            if len(tail) < 10:
                tail = rows[-min(10, len(rows)):]
            m = lambda k: self.mean(tail, k)
            die, hs, air, p = m('die'), m('hs'), m('air'), m('p')
            rec = dict(duty=g['duty'], secs=len(rows), mins=round(self.secs(rows[0]) / 60, 1),
                       clock=rows[0]['t'].strftime('%H:%M:%S'),
                       die=die, hs=hs, air=air, p=p, fps=m('fps'),
                       rpm=m('rpm'), af=m('flow'), clk=m('clk'))
            rec['rint'] = (die - hs) / p if (hs is not None and p) else None
            rec['rconv'] = (hs - air) / p if (hs is not None and p) else None
            rec['rtot'] = (die - air) / p if p else None
            # how much of this tread the card spent below full clock
            n = len(rows)
            thr = sum(1 for r in rows if r['clk'] is not None and r['clk'] < 399)
            rec['thr'] = round(100 * thr / n, 1) if n else None
            out.append({k: (round(v, 4) if isinstance(v, float) else v) for k, v in rec.items()})
        return out

    def series(self, step):
        S = {k: [] for k in ('t', 'die', 'hs', 'air', 'p', 'fps', 'clk', 'duty', 'rpm', 'af', 'dt')}
        for i, r in enumerate(self.rows):
            if i % step:
                continue
            S['t'].append(int(round(self.secs(r))))
            for key, col in (('die', 'die'), ('hs', 'hs'), ('air', 'air'), ('p', 'p'),
                             ('fps', 'fps'), ('clk', 'clk'), ('duty', 'duty'),
                             ('rpm', 'rpm'), ('af', 'flow')):
                v = r[col]
                S[key].append(None if v is None else round(v, 3))
            # the interface drop itself, the series this report exists for
            S['dt'].append(round(r['die'] - r['hs'], 3)
                           if (r['die'] is not None and r['hs'] is not None) else None)
        return S


def fit(xs, ys):
    """Least squares y = a + b x, with R^2.  Hand-rolled to keep this script
    dependency-free, like Catalog's INI reader."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    sst = sum((y - my) ** 2 for y in ys)
    sse = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    return a, b, (1 - sse / sst if sst else float('nan')), n


def tau(rows, key, tinf, secs, t0):
    """Single-exponential time constant from the slope of ln(excess) vs t.

    Fitted over the first `secs` of cool-down and only while the excess is
    above 0.5 C -- below that the fit is reading sensor quantisation, not
    physics (both sensors step in 1/16 C).
    """
    xs, ys = [], []
    for r in rows:
        dt = (r['t'] - t0).total_seconds()
        if dt > secs:
            break
        e = r[key] - tinf
        if e > 0.5:
            xs.append(dt)
            ys.append(math.log(e))
    if len(xs) < 20:
        return None
    a, b, r2, n = fit(xs, ys)
    return dict(tau=round(-1 / b, 1), r2=round(r2, 4), n=n)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--run', required=True, help='the session WITH the thermocouple')
    ap.add_argument('--prev', help='an earlier run of the same plan, for the control')
    ap.add_argument('--out', required=True)
    ap.add_argument('--step', type=int, default=16, help='timeline decimation (default 16 s)')
    a = ap.parse_args()

    R = Run(a.run)
    if not R.has_tc:
        raise SystemExit(f"{a.run}: the MAX31855 column is empty -- no thermocouple in this run")
    P = Run(a.prev) if a.prev else None

    out = {}
    out['start'] = R.rows[0]['t'].isoformat(timespec='milliseconds')
    out['run_start'] = int(round(R.secs(R.load[0])))
    out['load_end'] = int(round(R.secs(R.load[-1])))
    out['series'] = R.series(a.step)
    out['treads'] = R.treads()

    # ---------------- the interface, which is what the thermocouple buys ----
    full = [r for r in R.load if r['clk'] == 400 and r['fps'] and r['fps'] > 1360
            and r['hs'] is not None and r['p']]
    rint = [(r['die'] - r['hs']) / r['p'] for r in full]
    delta = [r['die'] - r['hs'] for r in full]
    a0, b0, r20, n0 = fit([r['p'] for r in full], delta)
    out['interface'] = dict(
        n=len(full),
        rint_mean=round(sum(rint) / len(rint), 4), rint_sd=round(st.pstdev(rint), 4),
        rint_min=round(min(rint), 4), rint_max=round(max(rint), 4),
        delta_mean=round(sum(delta) / len(delta), 3),
        delta_min=round(min(delta), 3), delta_max=round(max(delta), 3),
        fit=dict(slope=round(b0, 4), intercept=round(a0, 3), r2=round(r20, 4), n=n0),
        p_min=round(min(r['p'] for r in full), 3), p_max=round(max(r['p'] for r in full), 3),
    )
    # binned by heatsink temperature: is the interface temperature-dependent?
    bins = []
    for lo in range(40, 110, 10):
        g = [r for r in full if lo <= r['hs'] < lo + 10]
        if len(g) < 30:
            continue
        p = sum(r['p'] for r in g) / len(g)
        d = sum(r['die'] - r['hs'] for r in g) / len(g)
        bins.append(dict(lo=lo, hi=lo + 10, n=len(g), p=round(p, 3),
                         delta=round(d, 3), rint=round(d / p, 4)))
    out['interface']['bins'] = bins
    # the scatter the page plots: rail power against the interface drop
    k = max(1, len(full) // 1200)
    out['interface']['scatter'] = [[round(r['p'], 3), round(r['die'] - r['hs'], 2)]
                                   for r in full[::k]]

    # ---------------- idle cross-check: the two sensors with no load --------
    idle = [r for r in R.cool[-600:] if r['hs'] is not None and r['p']]
    if idle:
        out['idle'] = dict(
            n=len(idle),
            p=round(sum(r['p'] for r in idle) / len(idle), 4),
            die=round(sum(r['die'] for r in idle) / len(idle), 3),
            hs=round(sum(r['hs'] for r in idle) / len(idle), 3),
            air=round(sum(r['air'] for r in idle) / len(idle), 3),
            delta=round(sum(r['die'] - r['hs'] for r in idle) / len(idle), 3),
        )
        out['idle']['predicted'] = round(out['interface']['rint_mean'] * out['idle']['p'], 3)

    # ---------------- cool-down: one body or two? --------------------------
    cd = [r for r in R.cool if r['hs'] is not None]
    if cd:
        t0 = cd[0]['t']
        tail = cd[-120:]
        inf_d = sum(r['die'] for r in tail) / len(tail)
        inf_h = sum(r['hs'] for r in tail) / len(tail)
        inf_a = sum(r['air'] for r in tail) / len(tail)
        cool = dict(secs=int((cd[-1]['t'] - t0).total_seconds()), n=len(cd),
                    die0=round(cd[0]['die'], 2), hs0=round(cd[0]['hs'], 2),
                    die_inf=round(inf_d, 2), hs_inf=round(inf_h, 2), air_inf=round(inf_a, 2),
                    die_excess=round(cd[0]['die'] - inf_d, 2),
                    hs_excess=round(cd[0]['hs'] - inf_h, 2),
                    die_tau=tau(cd, 'die', inf_d, 600, t0),
                    hs_tau=tau(cd, 'hs', inf_h, 600, t0))
        # the decaying excess, for a log plot
        step = max(1, len(cd) // 900)
        cool['t'] = [int((r['t'] - t0).total_seconds()) for r in cd[::step]]
        cool['die_e'] = [round(r['die'] - inf_d, 3) for r in cd[::step]]
        cool['hs_e'] = [round(r['hs'] - inf_h, 3) for r in cd[::step]]
        out['cool'] = cool

    # ---------------- the fan's ceiling, now visible on the heatsink -------
    tr = out['treads']
    knee = None
    for i in range(1, len(tr)):
        later = tr[i:]
        if len(later) < 5:
            break
        rpms = [x['rpm'] for x in later]
        if max(rpms) - min(rpms) < 40:          # flat to within 40 rpm from here on
            knee = tr[i]['duty']
            break
    sat = [r for r in R.load if r['duty'] >= (knee or 73.0) - 0.5]
    out['ceiling'] = dict(
        duty=knee, treads=sum(1 for x in tr if x['duty'] >= (knee or 73.0) - 0.05),
        n=len(sat), mins=round(len(sat) / 60, 1),
        rpm=round(sum(r['rpm'] for r in sat) / len(sat), 1),
        rpm_sd=round(st.pstdev([r['rpm'] for r in sat]), 2),
        af=round(sum(r['flow'] for r in sat) / len(sat), 3),
        af_sd=round(st.pstdev([r['flow'] for r in sat]), 3),
        hs=round(sum(r['hs'] for r in sat) / len(sat), 2),
        hs_sd=round(st.pstdev([r['hs'] for r in sat]), 3),
        die=round(sum(r['die'] for r in sat) / len(sat), 2),
        die_sd=round(st.pstdev([r['die'] for r in sat]), 3),
    )
    below = [x for x in tr if abs(x['duty'] - ((knee or 73.0) - 1.5873)) < 0.3]
    out['ceiling']['rpm_below'] = round(below[0]['rpm'], 0) if below else None

    # ---------------- thermal gain, for the control loop --------------------
    fullt = [x for x in tr if x['thr'] == 0.0]
    gain = []
    for x, y in zip(fullt, fullt[1:]):
        dd = y['duty'] - x['duty']
        if dd <= 0:
            continue
        gain.append(dict(duty=x['duty'], die=x['die'], hs=x['hs'], rpm=x['rpm'],
                         g=round((y['die'] - x['die']) / dd, 4)))
    out['gain'] = gain
    if fullt:
        out['authority'] = dict(
            from_duty=fullt[0]['duty'], from_die=fullt[0]['die'],
            to_die=out['ceiling']['die'],
            span=round(fullt[0]['die'] - out['ceiling']['die'], 2))
        for thr in (0.5, 0.25, 0.1):
            k2 = next((g['duty'] for g in gain if abs(g['g']) < thr), None)
            out['authority'][f'below_{str(thr).replace(".", "")}'] = k2

    # ---------------- leakage, at a fixed frame rate -----------------------
    aL, bL, r2L, nL = fit([r['die'] for r in full], [r['p'] for r in full])
    dlo = min(r['die'] for r in full)
    dhi = max(r['die'] for r in full)
    out['leakage'] = dict(mw_per_c=round(bL * 1000, 2), intercept=round(aL, 4),
                          r2=round(r2L, 4), n=nL,
                          die_lo=round(dlo, 1), die_hi=round(dhi, 1),
                          swing=round(bL * (dhi - dlo), 3),
                          share=round(100 * bL * (dhi - dlo) / (aL + bL * dhi), 1))

    # ---------------- the control: the same plan a day earlier -------------
    if P:
        pt = P.treads()
        rows, dd, dair, drpm = [], [], [], []
        for x, y in zip(pt, tr):
            if abs(x['duty'] - y['duty']) > 0.05:
                break
            rows.append(dict(duty=y['duty'], die_prev=x['die'], die=y['die'],
                             d_die=round(y['die'] - x['die'], 3),
                             air_prev=x['air'], air=y['air'],
                             rpm_prev=x['rpm'], rpm=y['rpm'],
                             thr_prev=x['thr'], thr=y['thr']))
            dd.append(y['die'] - x['die'])
            dair.append(y['air'] - x['air'])
            drpm.append(y['rpm'] - x['rpm'])
        ld = P.load
        out['control'] = dict(
            prev=os.path.basename(a.prev), prev_start=P.rows[0]['t'].isoformat(timespec='seconds'),
            prev_rows=len(P.rows), rows=len(R.rows),
            prev_has_tc=P.has_tc, treads=len(rows), per_tread=rows,
            die_mean=round(sum(dd) / len(dd), 3), die_min=round(min(dd), 3), die_max=round(max(dd), 3),
            air_mean=round(sum(dair) / len(dair), 3),
            rpm_mean=round(sum(drpm) / len(drpm), 1),
            prev_thr=round(100 * sum(1 for r in ld if r['clk'] is not None and r['clk'] < 399) / len(ld), 1),
            thr=round(100 * sum(1 for r in R.load if r['clk'] is not None and r['clk'] < 399) / len(R.load), 1),
        )
        # where the two runs' agreement breaks: the hysteresis band
        band = [r for r in rows if abs(r['d_die']) > 1.0]
        out['control']['band'] = dict(
            n=len(band), lo=min((r['duty'] for r in band), default=None),
            hi=max((r['duty'] for r in band), default=None),
            outside_max=round(max((abs(r['d_die']) for r in rows if abs(r['d_die']) <= 1.0),
                                  default=0), 3))

        # ---- what the probe itself cost -------------------------------
        # Compared on RISE ABOVE AIR, not on die temperature: the two days'
        # ambient differed by a few tenths and that would otherwise be read as
        # an effect of the probe.  Two regions where neither run can be lagging
        # -- the fan-off soak (both pinned on the same throttled plateau) and
        # the saturated plateau (90 minutes at one fan speed).
        def rise(sel):
            g = [r for r in rows if sel(r['duty'])]
            if not g:
                return None
            a = sum(r['die_prev'] - r['air_prev'] for r in g) / len(g)
            b = sum(r['die'] - r['air'] for r in g) / len(g)
            return dict(treads=len(g), prev=round(a, 2), run=round(b, 2),
                        delta=round(b - a, 2), pct=round(100 * (b - a) / a, 2))
        knee = out['ceiling']['duty'] or 73.0
        out['intrusion'] = dict(
            still=rise(lambda d: d <= 20.6),
            forced=rise(lambda d: d >= knee - 0.05),
            note='rise above board air; the probe blocks one fin channel')

    os.makedirs(a.out, exist_ok=True)
    dst = os.path.join(a.out, 'data.js')
    with open(dst, 'w') as fh:
        fh.write('const TC=' + json.dumps(out, separators=(',', ':'),
                                          allow_nan=False) + ';\n')
    print(f"wrote {dst}  ({os.path.getsize(dst)} bytes)")
    summary(out)


def summary(o):
    p = print
    p()
    p("=== every scalar the page's prose quotes ===")
    i = o['interface']
    p(f"interface   R_int {i['rint_mean']:.3f} C/W  sd {i['rint_sd']:.3f}  "
      f"range {i['rint_min']:.3f}..{i['rint_max']:.3f}   n={i['n']}")
    p(f"            drop  {i['delta_mean']:.2f} C  range {i['delta_min']:.2f}..{i['delta_max']:.2f}"
      f"   over P {i['p_min']:.3f}..{i['p_max']:.3f} W")
    p(f"            fit   delta = {i['fit']['slope']:.3f} P {i['fit']['intercept']:+.2f}  "
      f"R2={i['fit']['r2']:.3f}  (narrow P range -- do not over-read)")
    for b in i['bins']:
        p(f"            hs {b['lo']:3d}-{b['hi']:3d} C  n={b['n']:5d}  {b['p']:.3f} W  "
          f"drop {b['delta']:.2f}  R_int {b['rint']:.3f}")
    if 'idle' in o:
        d = o['idle']
        p(f"idle        {d['p']:.3f} W  die {d['die']:.2f}  hs {d['hs']:.2f}  air {d['air']:.2f}  "
          f"drop {d['delta']:+.2f} C  (R_int*P would be {d['predicted']:+.2f})")
    t = o['treads']
    ends = [t[0], t[-1]]
    for e in ends:
        p(f"tread {e['duty']:5.1f} %  die {e['die']:6.2f}  hs {e['hs']:6.2f}  air {e['air']:5.2f}  "
          f"{e['p']:.3f} W   R_int {e['rint']:.2f}  R_conv {e['rconv']:5.2f}  R_tot {e['rtot']:5.2f}  "
          f"(R_int is {100*e['rint']/e['rtot']:.1f} % of total)")
    if 'cool' in o:
        c = o['cool']
        p(f"cool-down   {c['secs']} s, fan full.  die {c['die0']:.2f}->{c['die_inf']:.2f}  "
          f"hs {c['hs0']:.2f}->{c['hs_inf']:.2f}  air {c['air_inf']:.2f}")
        p(f"            tau  die {c['die_tau']['tau']:.1f} s (R2 {c['die_tau']['r2']:.4f})   "
          f"heatsink {c['hs_tau']['tau']:.1f} s (R2 {c['hs_tau']['r2']:.4f})")
        p(f"            settled above air: die {c['die_inf']-c['air_inf']:+.2f}  "
          f"hs {c['hs_inf']-c['air_inf']:+.2f} C")
    c = o['ceiling']
    p(f"ceiling     {c['duty']} % duty: {c['treads']} treads, {c['mins']:.0f} min of dead travel")
    p(f"            rpm {c['rpm']:.0f} sd {c['rpm_sd']:.1f} (from {c['rpm_below']:.0f} one tread below)  "
      f"air {c['af']:.2f} sd {c['af_sd']:.2f}")
    p(f"            heatsink {c['hs']:.2f} sd {c['hs_sd']:.2f}   die {c['die']:.2f} sd {c['die_sd']:.2f}")
    if 'authority' in o:
        au = o['authority']
        p(f"authority   {au['from_duty']} % ({au['from_die']:.2f} C) -> ceiling ({au['to_die']:.2f} C) "
          f"= {au['span']:.1f} C")
        p(f"            |gain| < 0.5 above {au['below_05']} %, < 0.25 above {au['below_025']} %, "
          f"< 0.1 above {au['below_01']} %")
    l = o['leakage']
    p(f"leakage     {l['mw_per_c']:.2f} mW/C  R2={l['r2']:.3f}  n={l['n']}  "
      f"{l['swing']:.3f} W over {l['die_lo']:.1f}..{l['die_hi']:.1f} C = {l['share']:.1f} % at the hot end")
    if 'control' in o:
        c = o['control']
        p(f"control     {c['prev']} ({c['prev_rows']} rows vs {c['rows']}), thermocouple fitted: {c['prev_has_tc']}")
        p(f"            die diff mean {c['die_mean']:+.2f}  range {c['die_min']:+.2f}..{c['die_max']:+.2f}  "
          f"over {c['treads']} treads")
        p(f"            board air {c['air_mean']:+.2f} C   fan {c['rpm_mean']:+.0f} rpm   "
          f"throttled {c['prev_thr']:.1f} % vs {c['thr']:.1f} %")
        b = c['band']
        p(f"            treads disagreeing by >1 C: {b['n']}, all between {b['lo']} and {b['hi']} % duty; "
          f"worst elsewhere {b['outside_max']:.2f} C")
    if 'intrusion' in o:
        i = o['intrusion']
        p("intrusion   what fitting the thermocouple cost, as rise above board air:")
        for k, lbl in (('still', 'still air  '), ('forced', 'full fan   ')):
            d = i[k]
            p(f"            {lbl} {d['prev']:.2f} -> {d['run']:.2f} C  "
              f"{d['delta']:+.2f} C ({d['pct']:+.2f} %)  over {d['treads']} treads")


if __name__ == '__main__':
    main()
