#!/usr/bin/env python3
"""Generate docs/reports/ra6m5-soft-sensor/ -- how the soft sensor was fitted.

Writes `data.js` (one `const SS = {...}`) beside a hand-written `index.html`
that renders every table and chart from it. NOTHING in the page re-derives a
number and nothing is typed in, the same contract as the other two report
generators. Run it twice and it must produce a byte-identical file.

WHY THIS IS A SEPARATE PAGE from `ra6m5-loop/`. That one reports ONE run -- the
three arms that asked what doing without the thermocouple costs. This one
reports the fit, which spans four runs and whose whole point is that the data
came from somewhere before the model did. Merging them would bury the
characterisation that makes the fit believable.

The model's constants are PARSED OUT OF Control.h, never copied: the page
quotes R_conv, the heat capacity and the time constants they imply, and a
second copy here would be free to drift from the firmware that runs.

  tools/ra6m5-report-softsensor.py \
      --ladder   docs/data/ra6m5-thermal-hailo8-20261122.csv \
      --probe    docs/data/ra6m5-loadprobe-20261123.csv \
      --fit      docs/data/ra6m5-softsensor-fit-20261123.csv \
      --validate docs/data/ra6m5-loop-tc-20261123.csv \
      --validate docs/data/ra6m5-loop-4setpoint-20261122.csv \
      --out      docs/reports/ra6m5-soft-sensor
"""
import argparse, csv, json, math, os, re
from datetime import datetime

GOP = {'ResNet-50': 8.174, 'YOLOv8s': 28.60, 'YOLOv8m': 78.94, 'SCRFD-10G': 26.68,
       'SCRFD-2.5G': 6.86, 'SCRFD-500M': 1.47, 'ArcFace MobileFaceNet': 0.88,
       'OSNet x1.0': 1.96}


def num(v):
    if v is None or v == '':
        return None
    try:
        f = float(v)
    except ValueError:
        return None
    return None if math.isnan(f) else f


def mean(v):
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else None


def sd(v):
    v = [x for x in v if x is not None]
    if len(v) < 2:
        return None
    m = sum(v) / len(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def r(v, d=4):
    return None if v is None else round(v, d)


class Run:
    """One CSV, columns resolved BY SUFFIX because the BDF and the USB port
    path both move across reboots."""

    def __init__(self, path):
        self.path = path
        with open(path) as fh:
            rd = csv.reader(fh)
            hdr = next(rd)
            ix = {n: i for i, n in enumerate(hdr)}

            def c(suf, req=True):
                for n in hdr:
                    if n.endswith(suf):
                        return ix[n]
                if req:
                    raise SystemExit(f"{path}: no column ending in {suf!r}")
                return None

            col = dict(ts0=c('_TS0'), ts1=c('_TS1'), air=c('_EMC2101'), hs=c('_MAX31855'),
                       p=c('_INA228_POWER'), clk=c('_CLK'), duty=c('_FAN_DUTY'),
                       rpm=c('_FAN_RPM', False))
            self.rows = []
            for row in rd:
                d = {'t': datetime.fromisoformat(row[0]),
                     'st': row[ix['bench_state']], 'model': row[ix['bench_model']],
                     'cap': row[ix['bench_target_fps']], 'fps': num(row[ix['hailo_fps']])}
                for k, i in col.items():
                    d[k] = None if i is None else num(row[i])
                dies = [x for x in (d['ts0'], d['ts1']) if x is not None]
                d['die'] = max(dies) if dies else None
                self.rows.append(d)
        t0 = self.rows[0]['t']
        for d in self.rows:
            d['s'] = (d['t'] - t0).total_seconds()

    @property
    def mins(self):
        return self.rows[-1]['s'] / 60.0

    def segments(self):
        """Contiguous blocks of one (state, model, cap) -- one tread each."""
        out, cur, key = [], None, None
        for d in self.rows:
            k = (d['st'], d['model'], d['cap'])
            if k != key:
                cur = []
                out.append((k, cur))
                key = k
            cur.append(d)
        return out


def overlay(block, step, nmax):
    """One tread as (heatsink - air) on a clock that starts at the tread, so
    treads taken hours apart can be laid over each other. Rise above air
    rather than raw heatsink, because that is the quantity the model predicts
    and it divides out whatever ambient was doing that afternoon."""
    y = []
    for i in range(0, min(len(block), nmax), step):
        d = block[i]
        y.append(None if (d['hs'] is None or d['air'] is None) else round(d['hs'] - d['air'], 2))
    return y


def throttled_pct(block):
    clk = [d['clk'] for d in block if d['clk'] is not None]
    return 100.0 * sum(1 for c in clk if c < 399.5) / len(clk) if clk else None


def expfit(ys):
    """T(t) = Tinf + (T0-Tinf)exp(-t/tau): grid over tau, least squares in the
    two amplitudes. FIT THE EXPONENTIAL, DO NOT REGRESS THE TAIL -- a decaying
    exponential still has slope while sitting within a degree of its asymptote,
    so a tail slope calls every settled tread unsettled."""
    ys = [y for y in ys if y is not None]
    n = len(ys)
    if n < 30:
        return None, None
    best = None
    for ti in range(20, 1200, 2):
        tau = float(ti)
        S1, Se, See, Sy, Sey = n, 0.0, 0.0, 0.0, 0.0
        for i, y in enumerate(ys):
            e = math.exp(-i / tau)
            Se += e; See += e * e; Sy += y; Sey += e * y
        det = S1 * See - Se * Se
        if abs(det) < 1e-12:
            continue
        a = (See * Sy - Se * Sey) / det
        b = (S1 * Sey - Se * Sy) / det
        ss = sum((y - (a + b * math.exp(-i / tau))) ** 2 for i, y in enumerate(ys))
        if best is None or ss < best[0]:
            best = (ss, a, tau)
    return best[1], best[2]


# ------------------------------------------------------------ the model ----
def read_model(path):
    src = open(path).read()

    def arr(name):
        m = re.search(name + r'\[kCurveN\]\s*=\s*\{([^}]*)\}', src)
        if not m:
            raise SystemExit(f"{path}: no {name}[kCurveN] -- Control.h was "
                             "refactored; fix read_model()")
        return [float(x) for x in m.group(1).replace('\n', ' ').split(',') if x.strip()]

    def const(pat):
        m = re.search(pat, src)
        if not m:
            raise SystemExit(f"{path}: cannot find {pat}")
        return float(m.group(1))

    duty, rc = arr('d'), arr('r')
    if len(duty) != len(rc):
        raise SystemExit(f"{path}: {len(duty)} duty nodes vs {len(rc)} resistances")
    if any(b > a for a, b in zip(rc, rc[1:])):
        raise SystemExit(f"{path}: curve_rconv is not monotone non-increasing")
    return dict(duty=duty, rconv=rc,
                C=const(r'kHeatCapacity\s*=\s*([0-9.]+)'),
                rint=const(r'double\s+rint\s*=\s*([0-9.]+)'),
                ready='model_ready() const { return true; }' in src)


def rconv_at(M, d):
    du, rc = M['duty'], M['rconv']
    if d <= du[0]:
        return rc[0]
    if d >= du[-1]:
        return rc[-1]
    for i in range(1, len(du)):
        if d <= du[i]:
            f = (d - du[i - 1]) / (du[i] - du[i - 1])
            return rc[i - 1] + f * (rc[i] - rc[i - 1])
    return rc[-1]


def simulate(rows, M):
    """The firmware's own recurrence, at the log's 1 Hz. Inputs are held
    forward exactly as Control::freshen() holds them. Returns (dynamic,
    static) -- the second being the memoryless form the open-loop branch's
    die_est uses, kept so the page can show what the pole is worth."""
    dyn, sta = [], []
    w = air = duty = None
    tc_hat, prev = None, None
    for d in rows:
        w = d['p'] if d['p'] is not None else w
        air = d['air'] if d['air'] is not None else air
        duty = d['duty'] if d['duty'] is not None else duty
        if w is None or air is None or duty is None:
            dyn.append(None); sta.append(None); continue
        R = rconv_at(M, duty)
        ss = air + w * R
        sta.append(ss)
        if tc_hat is None:
            tc_hat = d['hs'] if d['hs'] is not None else ss
        elif prev is not None:
            dt = min(max(d['s'] - prev, 0.0), 5.0)
            tau = R * M['C']
            if dt > 0 and tau > 0:
                tc_hat += min(dt / tau, 1.0) * (ss - tc_hat)
        prev = d['s']
        dyn.append(tc_hat)
    return dyn, sta


def score(rows, est, key='hs'):
    e = [a - d[key] for a, d in zip(est, rows) if a is not None and d[key] is not None]
    if not e:
        return None
    mu = sum(e) / len(e)
    return dict(n=len(e), rms=r(math.sqrt(sum(x * x for x in e) / len(e)), 3),
                bias=r(mu, 3), worst_cold=r(min(e), 2), worst_hot=r(max(e), 2),
                cold_pct=r(100.0 * sum(1 for x in e if x < 0) / len(e), 1))


# ------------------------------------------------------------------ main ----
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ladder', required=True, help='the 59-level fan ladder')
    ap.add_argument('--probe', required=True, help='the eight-model load probe')
    ap.add_argument('--fit', required=True, help='the 24-tread fitting run')
    ap.add_argument('--validate', action='append', default=[],
                    help='a run HELD OUT of the fit; repeatable')
    ap.add_argument('--firmware', default='firmware/mb-probe-ra6m5/Control.h')
    ap.add_argument('--out', required=True)
    ap.add_argument('--step', type=int, default=15, help='timeline decimation (s)')
    a = ap.parse_args()

    M = read_model(a.firmware)
    out = dict(model=dict(C=M['C'], rint=M['rint'], ready=M['ready'],
                          firmware=os.path.basename(a.firmware)), runs=[])

    def add_run(path, role, gave, R):
        out['runs'].append(dict(file=os.path.basename(path),
                                where='docs/data' if '/data/' in path else 'logs',
                                date=R.rows[0]['t'].strftime('%d %b'),
                                n=len(R.rows), mins=r(R.mins, 1), role=role, gave=gave))

    # ---- 1. the fan ladder: the curve's SHAPE, and where it is unusable ----
    L = Run(a.ladder)
    add_run(a.ladder, 'characterise',
            'R_conv against duty across 59 levels, at one load', L)
    lev, seen = [], {}
    for d in L.rows:
        if d['st'] != 'running' or d['duty'] is None:
            continue
        seen.setdefault(round(d['duty'], 1), []).append(d)
    for duty in sorted(seen):
        blk = seen[duty]
        tail = blk[-60:]
        w, tc, air = mean([x['p'] for x in tail]), mean([x['hs'] for x in tail]), mean([x['air'] for x in tail])
        thr = throttled_pct(blk)
        lev.append(dict(duty=duty, n=len(blk), w=r(w, 3), tc=r(tc, 2), air=r(air, 2),
                        rconv=r((tc - air) / w, 4) if w else None, thr=r(thr, 1),
                        curve=r(rconv_at(M, duty), 4)))
    clean = [x for x in lev if x['thr'] == 0.0]
    out['ladder'] = dict(levels=lev, clean_from=min(x['duty'] for x in clean),
                         n_throttled=sum(1 for x in lev if x['thr'] and x['thr'] > 0),
                         n_levels=len(lev))
    hi = [x for x in clean if x['duty'] >= 73.0]
    out['ladder']['flat_above_73'] = dict(n=len(hi), lo=r(min(x['rconv'] for x in hi), 3),
                                          hi=r(max(x['rconv'] for x in hi), 3))

    # ---- 2. the load probe: the W AXIS, and the triangle -------------------
    P = Run(a.probe)
    add_run(a.probe, 'characterise',
            'the rail power of all eight models -- does a load axis exist at all?', P)
    mods = []
    for (st, md, _), blk in P.segments():
        if st != 'running' or len(blk) < 120:
            continue
        tail = blk[-60:]
        w = mean([x['p'] for x in tail]); fps = mean([x['fps'] for x in tail])
        tc = mean([x['hs'] for x in tail]); air = mean([x['air'] for x in tail])
        g = GOP.get(md)
        mods.append(dict(model=md, gop=g, fps=r(fps, 1), w=r(w, 3),
                         tops=r(g * fps / 1000.0, 2) if (g and fps) else None,
                         die=r(mean([x['die'] for x in tail]), 1),
                         rconv=r((tc - air) / w, 3) if w else None,
                         thr=r(throttled_pct(blk), 2)))
    seenm = set()
    uniq = [m for m in mods if not (m['model'] in seenm or seenm.add(m['model']))]
    uniq.sort(key=lambda m: m['w'])
    # ONE PANEL PER MODEL, all on identical axes. Eight curves on one plot
    # would need eight distinguishable colours and would hide exactly what
    # matters -- that the SHAPE is the same every time and only the height
    # changes. Small multiples put that comparison in the reader's eye instead
    # of in a legend.
    STEP, NMAX = 2, 299
    pan = []
    for (st, md, _), blk in P.segments():
        if st != 'running' or len(blk) < 120:
            continue
        tail = blk[-60:]
        y = overlay(blk, STEP, NMAX)
        pan.append(dict(model=md, w=r(mean([x['p'] for x in tail]), 3),
                        plateau=y[-1], y=y))
    seeno = set()
    pan = [x for x in pan if not (x['model'] in seeno or seeno.add(x['model']))]
    pan.sort(key=lambda x: -x['plateau'])
    for x in pan:
        x['rconv'] = r(x['plateau'] / x['w'], 3)
    out['probe'] = dict(panels=dict(t=list(range(0, NMAX, STEP)), rows=pan,
                                    ymax=5 * math.ceil(max(x['plateau'] for x in pan) / 5)),
                        models=uniq, n_treads=len(mods),
                        w_lo=min(m['w'] for m in uniq), w_hi=max(m['w'] for m in uniq))
    out['probe']['spread'] = r(out['probe']['w_hi'] / out['probe']['w_lo'], 1)

    # ---- 3. the fitting run: four duties x a cap ladder --------------------
    F = Run(a.fit)
    add_run(a.fit, 'fit', '24 treads at four fixed duties, each a (W, duty, tc) point', F)
    treads = []
    for (st, md, cap), blk in F.segments():
        if st != 'running' or len(blk) < 120:
            continue
        duty = mean([x['duty'] for x in blk])
        winf, _ = expfit([x['p'] for x in blk])
        tcinf, tau = expfit([x['hs'] for x in blk])
        air = mean([x['air'] for x in blk[-60:]])
        thr = throttled_pct(blk)
        treads.append(dict(t=len(treads) + 1, model=md, cap=(cap or 'max'),
                           n_rows=len(blk),
                           duty=r(duty, 2), w=r(winf, 3), tc=r(tcinf, 2), air=r(air, 2),
                           tau=r(tau, 0), thr=r(thr, 1),
                           fps=r(mean([x['fps'] for x in blk[-60:]]), 1),
                           cap_n=('max' if not cap else str(int(float(cap)))),
                           die=r(max(x['die'] for x in blk if x['die'] is not None), 1),
                           rconv=r((tcinf - air) / winf, 4)))
    out['fit'] = dict(treads=treads, n=len(treads))

    # the cap ladder at one duty, corrected to a common heatsink temperature:
    # leakage follows temperature at ~18 mW/C, so an uncorrected comparison
    # credits the cap with the cooling it also caused.
    ref = [t for t in treads if t['duty'] > 70 and t['model'] == 'ResNet-50' and t['thr'] == 0]
    tc_ref = mean([t['tc'] for t in ref])
    leak = 0.01789
    grp = {}
    for t in ref:
        grp.setdefault(t['cap'], []).append(t)
    caps = []
    for cap, g in grp.items():
        fps = mean([x['fps'] for x in g])
        w = mean([x['w'] for x in g])
        wc = mean([x['w'] - leak * (x['tc'] - tc_ref) for x in g])
        caps.append(dict(cap=('max' if cap == 'max' else str(int(float(cap)))),
                         fps=r(fps, 1), w=r(w, 3), w_corr=r(wc, 3), n=len(g),
                         mj=r(1000.0 * w / fps, 2) if fps else None))
    caps.sort(key=lambda c: -(c['fps'] or 0))
    out['fit']['caps'] = dict(rows=caps, tc_ref=r(tc_ref, 1), leak_mw=r(leak * 1000, 2))

    # R_conv per duty, clean treads only
    byduty = {}
    for t in treads:
        if t['thr'] == 0.0:
            byduty.setdefault(round(t['duty'], 1), []).append(t)
    duties = []
    for duty in sorted(byduty, reverse=True):
        # ResNet-50 ONLY. Pooling models here would fold the model-identity
        # effect (up to 3.5 %) into what is meant to be the hardware's own spread,
        # and that effect is reported separately below.
        g = [x for x in byduty[duty] if x['model'] == 'ResNet-50']
        if not g:
            continue
        rc = [x['rconv'] for x in g]
        tau = [x['tau'] for x in g]
        duties.append(dict(duty=duty, n=len(g), rconv=r(mean(rc), 4), sd=r(sd(rc), 4),
                           sd_pct=r(100 * sd(rc) / mean(rc), 2) if sd(rc) else None,
                           tau=r(mean(tau), 1), C=r(mean(tau) / mean(rc), 2),
                           curve=r(rconv_at(M, duty), 4)))
    out['fit']['duties'] = duties
    out['fit']['throttled'] = [dict(t=t['t'], duty=t['duty'],
                                    cap=('max' if t['cap'] == 'max' else str(int(float(t['cap'])))),
                                    fps=t['fps'],
                                    die=t['die'], w=t['w'], thr=t['thr'])
                               for t in treads if t['thr'] and t['thr'] > 0]
    # model identity, at matched duty against ResNet-50
    ident = []
    for duty in sorted(byduty, reverse=True):
        g = byduty[duty]
        base = mean([x['rconv'] for x in g if x['model'] == 'ResNet-50'])
        if base is None:
            continue
        for x in g:
            if x['model'] == 'ResNet-50':
                continue
            ident.append(dict(model=x['model'], duty=duty, w=x['w'], rconv=x['rconv'],
                              base=r(base, 4), pct=r(100 * (x['rconv'] - base) / base, 2)))
    out['fit']['identity'] = ident

    # ONE PANEL PER FAN DUTY, every tread at that duty drawn on it. Within a
    # panel the curves SHOULD coincide -- they differ only in frame-rate cap,
    # which the cap table shows does almost nothing to the power -- so a panel
    # where they fan apart is telling you something. The 20.6 % panel is the
    # whole point: three of its four treads throttled and sit well above the
    # one that did not, while reporting their commanded frame rate exactly.
    STEP2, NMAX2 = 5, 780
    blocks = [(k, b) for k, b in F.segments() if k[0] == 'running' and len(b) > 120]
    panels = []
    for duty in sorted({round(t['duty'], 1) for t in treads}, reverse=True):
        rows = []
        for t in treads:
            if round(t['duty'], 1) != duty:
                continue
            blk = next(b for (st, md, cap), b in blocks
                       if md == t['model'] and (cap or 'max') == t['cap']
                       and abs(mean([x['duty'] for x in b]) - t['duty']) < 0.05
                       and len(b) == t['n_rows'])
            rows.append(dict(cap=t['cap_n'], model=t['model'], w=t['w'],
                             thr=bool(t['thr'] and t['thr'] > 0), die=t['die'],
                             y=overlay(blk, STEP2, NMAX2)))
        rows.sort(key=lambda x: (x['thr'], -(x['w'] or 0)))
        panels.append(dict(duty=duty, rows=rows,
                           n_thr=sum(1 for x in rows if x['thr'])))
    ym = max(max(v for v in x['y'] if v is not None)
             for pn in panels for x in pn['rows'])
    out['fit']['panels'] = dict(t=list(range(0, NMAX2, STEP2)), rows=panels,
                                ymax=10 * math.ceil(ym / 10))

    # ---- 4. the model, and the nodes the fit MOVED -------------------------
    OLD = {0.0: 16.48, 9.5: 16.17, 20.6: 15.20, 30.2: 12.97, 39.7: 11.29,
           50.8: 9.88, 60.3: 8.37, 69.8: 7.11, 73.0: 6.74}
    why = {0.0: 'natural convection, from four fan-off brackets at 0.86 W',
           9.5: 'two settled 9.5 % blocks, 0.86 W; at its floor the fan turns 262 rpm',
           20.6: 'the one unthrottled low-duty tread that exists (n=1)'}
    out['model']['nodes'] = [
        dict(duty=d, rconv=rc, tau=r(rc * M['C'], 0), old=OLD.get(d),
             pct=r(100 * (rc - OLD[d]) / OLD[d], 1) if d in OLD else None,
             why=why.get(d, "the ladder's unthrottled region, lag-corrected by the joint fit"))
        for d, rc in zip(M['duty'], M['rconv'])]

    # ---- 5. validation, on runs held out of the fit ------------------------
    out['valid'] = dict(runs=[], series=None)
    for path in a.validate:
        V = Run(path)
        add_run(path, 'validate (held out)',
                'closed-loop duty the fit never saw -- the real test', V)
        dyn, sta = simulate(V.rows, M)
        load = [i for i, d in enumerate(V.rows) if d['st'] == 'running']
        lr = [V.rows[i] for i in load]
        out['valid']['runs'].append(dict(
            file=os.path.basename(path), n=len(V.rows),
            dyn=score(lr, [dyn[i] for i in load]),
            static=score(lr, [sta[i] for i in load]),
            duty_lo=r(min(d['duty'] for d in lr if d['duty'] is not None), 1),
            duty_hi=r(max(d['duty'] for d in lr if d['duty'] is not None), 1),
            w_lo=r(min(d['p'] for d in lr if d['p'] is not None), 2),
            w_hi=r(max(d['p'] for d in lr if d['p'] is not None), 2)))
        if out['valid']['series'] is None:
            S = dict(t=[], tc=[], est=[], stat=[], duty=[], w=[])
            # Decimate on ELAPSED TIME, not `s % step`. `s` is a float from
            # total_seconds() -- 1.001, 2.003, ... -- so a modulo test matches
            # only s == 0.0 and silently yields a one-point series. It did
            # exactly that, and a chart with one point draws nothing at all.
            last = None
            for i, d in enumerate(V.rows):
                if last is not None and d['s'] - last < a.step:
                    continue
                last = d['s']
                S['t'].append(round(d['s'] / 60.0, 2))
                S['tc'].append(r(d['hs'], 2)); S['est'].append(r(dyn[i], 2))
                S['stat'].append(r(sta[i], 2)); S['duty'].append(r(d['duty'], 1))
                S['w'].append(r(d['p'], 2))
            vals = [v for k in ('tc', 'est', 'stat') for v in S[k] if v is not None]
            S['ymin'] = 5 * math.floor(min(vals) / 5)
            S['ymax'] = 5 * math.ceil(max(vals) / 5)
            S['tmax'] = max(S['t'])
            if len(S['t']) < 100:
                raise SystemExit(f"{path}: timeline decimated to {len(S['t'])} points "
                                 "-- a chart cannot be drawn from that; check --step")
            out['valid']['series'] = S
            out['valid']['series_file'] = os.path.basename(path)

    os.makedirs(a.out, exist_ok=True)
    p = os.path.join(a.out, 'data.js')
    with open(p, 'w') as fh:
        fh.write('/* generated by tools/ra6m5-report-softsensor.py -- do not edit */\n')
        fh.write('const SS = ')
        json.dump(out, fh, separators=(',', ':'), allow_nan=False)
        fh.write(';\n')
    print(f"wrote {p}  ({os.path.getsize(p)/1024:.0f} KiB)")
    summarise(out)


def summarise(o):
    p = print
    p("\nRUNS USED")
    for x in o['runs']:
        p(f"  {x['date']}  {x['file']:<40s} {x['n']:>6d} rows {x['mins']:>6.1f} min  {x['role']}")
        p(f"            {x['gave']}")

    L = o['ladder']
    p(f"\n1  THE FAN CURVE'S SHAPE -- {L['n_levels']} duty levels")
    p(f"   clean only from {L['clean_from']:.1f} % up: {L['n_throttled']} of {L['n_levels']} levels "
      f"had samples below 400 MHz")
    p(f"   flat above 73 %: {L['flat_above_73']['n']} levels read "
      f"{L['flat_above_73']['lo']:.2f}-{L['flat_above_73']['hi']:.2f} C/W, no trend")

    P = o['probe']
    p(f"\n2  THE LOAD AXIS -- {len(P['models'])} models, {P['w_lo']:.3f} to {P['w_hi']:.3f} W "
      f"= {P['spread']:.1f}x")
    for m in P['models']:
        p(f"   {m['model']:<24s} {m['gop']:>6.2f} GOP {m['fps']:>8.1f} fps  "
          f"{m['w']:>6.3f} W  die {m['die']:>5.1f} C  R_conv {m['rconv']:.3f}")

    F = o['fit']
    p(f"\n3  THE FITTING RUN -- {F['n']} treads")
    p(f"   the cap moved the RATE and not the rail (ResNet-50 at 73 % duty, "
      f"corrected to tc = {F['caps']['tc_ref']:.1f} C at {F['caps']['leak_mw']:.1f} mW/C):")
    for c in F['caps']['rows']:
        p(f"     cap {str(c['cap']):>6s}  {c['fps']:>7.1f} fps  {c['w']:.3f} W  "
          f"(corrected {c['w_corr']:.3f})  {c['mj']:>6.2f} mJ/frame")
    p("   R_conv per duty, unthrottled ResNet-50 treads only:")
    for d in F['duties']:
        sdp = f"+/- {d['sd']:.4f} ({d['sd_pct']:.2f} %)" if d['sd'] is not None else "single tread"
        p(f"     {d['duty']:>5.1f} %  {d['rconv']:7.4f} C/W  {sdp:<24s} n={d['n']}  "
          f"tau {d['tau']:>5.1f} s  C {d['C']:.2f} J/C   curve says {d['curve']:.4f}")
    if F['throttled']:
        p(f"   {len(F['throttled'])} treads THROTTLED, and the frame rate could not see it:")
        for t in F['throttled']:
            p(f"     T{t['t']:<3d} duty {t['duty']:.1f} %  cap {str(t['cap']):>6s} -> "
              f"fps {t['fps']:.1f}  die {t['die']:.1f} C  {t['thr']:.0f} % of samples below 400 MHz")
    if F['identity']:
        p("   model identity at matched duty, against ResNet-50:")
        for i in F['identity']:
            p(f"     {i['model']:<12s} {i['duty']:>5.1f} %  {i['w']:.2f} W  "
              f"R_conv {i['rconv']:.4f} vs {i['base']:.4f}  {i['pct']:+.2f} %")

    M = o['model']
    p(f"\n4  THE MODEL  (read from {M['firmware']}, model_ready={int(M['ready'])})")
    p(f"   tc_ss = air + W * R_conv(duty);  tau = R_conv * C;  C = {M['C']:.1f} J/C")
    p("     duty    R_conv     tau      was    change   provenance")
    for nd in M['nodes']:
        old = f"{nd['old']:.2f}" if nd['old'] is not None else "   -"
        ch = f"{nd['pct']:+.1f} %" if nd['pct'] is not None else "   -"
        p(f"   {nd['duty']:>5.1f} %  {nd['rconv']:6.2f}  {nd['tau']:>5.0f} s  {old:>6s}  "
          f"{ch:>8s}   {nd['why']}")

    p("\n5  VALIDATION, on runs held out of the fit")
    for v in o['valid']['runs']:
        d, s = v['dyn'], v['static']
        p(f"   {v['file']}  ({v['n']} rows, duty {v['duty_lo']:.1f}-{v['duty_hi']:.1f} %, "
          f"W {v['w_lo']:.2f}-{v['w_hi']:.2f})")
        p(f"     fitted model   rms {d['rms']:.3f} C  bias {d['bias']:+.3f}  "
          f"reads cold on {d['cold_pct']:.1f} % of {d['n']}  worst cold {d['worst_cold']:+.2f}")
        p(f"     static form    rms {s['rms']:.3f} C  bias {s['bias']:+.3f}  "
          f"worst {max(abs(s['worst_cold']), abs(s['worst_hot'])):.1f} C "
          f"<- what the SAME curve gives with no memory")


if __name__ == '__main__':
    main()
