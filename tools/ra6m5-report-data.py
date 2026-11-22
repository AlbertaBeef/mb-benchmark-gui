#!/usr/bin/env python3
"""Generate data2.js / data3.js / overlay.js for docs/reports/ra6m5-exploration.

The report's index.html is hand-written prose around three generated data files.
This script is the generator; it was reconstructed on 2026-11-22 after the
original ad-hoc version was lost, so keep it with the report.

    tools/ra6m5-report-data.py \
        --hailo docs/data/ra6m5-thermal-hailo8-20261121.csv \
        --deepx docs/data/ra6m5-thermal-deepx-20261122.csv \
        --out   docs/reports/ra6m5-exploration

Conventions that index.html depends on, so do not change them casually:
  * series are DECIMATED (every Nth row), not averaged, and `t` is seconds
    from the first row of the file;
  * `ts0` is the HOTTEST on-die sensor, whatever the card calls it;
  * the first benchmarked row of each run is dropped everywhere except the
    timeline: the INA228's one-second average there still covers mostly idle
    and reports a wildly high fps/W (the "first-sample artifact" in the footer);
  * a missing reading is JSON null, never 0 -- the chart code breaks the line.
"""
import argparse, csv, json, math, os, sys
from datetime import datetime

NA = None


def num(s):
    if s is None or s == '':
        return NA
    try:
        v = float(s)
    except ValueError:
        return NA
    return NA if math.isnan(v) else v


def rnd(v, d):
    return NA if v is NA else round(v, d)


def mean(vs):
    vs = [v for v in vs if v is not NA]
    return sum(vs) / len(vs) if vs else NA


class Run:
    """One session CSV, with the columns this report needs resolved by suffix."""

    def __init__(self, path, card):
        self.path, self.card = path, card
        with open(path, newline='') as fh:
            rd = csv.DictReader(fh)
            cols = rd.fieldnames
            end = lambda *sufs: [c for c in cols if c.endswith(sufs)]
            one = lambda *sufs: (end(*sufs) or [None])[0]
            self.die_cols = end('_TS0', '_TS1') or end('_T0', '_T1', '_T2')
            self.clk_cols = end('_CLK') or end('_C0', '_C1', '_C2')
            c_it, c_emc = one('_INA228_TEMP'), one('_EMC2101')
            c_pi, c_ph = one('_INA228_POWER'), one('_POW')
            c_v = one('_INA228_VBUS')
            c_af, c_rpm, c_duty = one('_AIRFLOW'), one('_FAN_RPM'), one('_FAN_DUTY')
            if not self.die_cols or not c_pi:
                sys.exit(f'{path}: no die temperature or no INA228 power column')
            self.rows = []
            for d in rd:
                die = [num(d[c]) for c in self.die_cols]
                clk = [num(d[c]) for c in self.clk_cols]
                die = [v for v in die if v is not NA]
                self.rows.append(dict(
                    t=datetime.fromisoformat(d['time']),
                    die=max(die) if die else NA,
                    clk_lo=min(clk) if [v for v in clk if v is not NA] else NA,
                    clk_hi=max(clk) if [v for v in clk if v is not NA] else NA,
                    clks=clk,
                    it=num(d[c_it]) if c_it else NA,
                    emc=num(d[c_emc]) if c_emc else NA,
                    pi=num(d[c_pi]), ph=num(d[c_ph]) if c_ph else NA,
                    v=num(d[c_v]) if c_v else NA,
                    af=num(d[c_af]) if c_af else NA,
                    rpm=num(d[c_rpm]) if c_rpm else NA,
                    duty=num(d[c_duty]) if c_duty else NA,
                    fps=num(d[f'{card}_fps']),
                    eff=num(d[f'{card}_fps_per_w']),
                    mj=num(d[f'{card}_mj_per_frame']),
                    state=d['bench_state'],
                    msg=d.get('message', '')))
        self.t0 = self.rows[0]['t']
        for r in self.rows:
            r['s'] = round((r['t'] - self.t0).total_seconds())
        self.run = [r for r in self.rows if r['state'] == 'running']
        if not self.run:
            sys.exit(f'{path}: no benchmarked rows')
        self.run_start = self.run[0]['s']
        self.run_end = self.run[-1]['s']
        # every row of the load except the first (the first-sample artifact)
        self.bench = self.run[1:]

    # -- fan ladder -------------------------------------------------------
    def levels(self):
        """Contiguous groups of equal applied duty while the load ran, duty > 0."""
        out = []
        for r in self.bench:
            if r['duty'] is NA:
                continue
            if not out or abs(out[-1]['duty'] - r['duty']) > 1e-6:
                out.append(dict(duty=r['duty'], rows=[]))
            out[-1]['rows'].append(r)
        return [g for g in out if g['duty'] > 0]

    def sweep(self):
        rows = []
        for g in self.levels():
            rs = g['rows']
            rows.append(dict(
                duty=round(g['duty'], 1), secs=len(rs),
                rpm=rnd(mean([r['rpm'] for r in rs]), 1),
                af=rnd(mean([r['af'] for r in rs]), 3),
                t_start=rnd(rs[0]['die'], 1), t_end=rnd(rs[-1]['die'], 1),
                p=rnd(mean([r['pi'] for r in rs]), 3),
                fps=rnd(mean([r['fps'] for r in rs]), 1)))
        return rows

    def lvl(self, full_fps, tail=20):
        """Per-level die / thermal resistance / % of cooled rate, from the last
        `tail` seconds of each held level -- as close to settled as a held
        level gets."""
        out = []
        for g in self.levels():
            rs = g['rows'][-tail:]
            die, emc = mean([r['die'] for r in rs]), mean([r['emc'] for r in rs])
            p = mean([r['pi'] for r in rs])
            tr = (die - emc) / p if NA not in (die, emc, p) and p else NA
            out.append(dict(duty=round(g['duty'], 1), die=rnd(die, 2),
                            tr=rnd(tr, 3), af=rnd(mean([r['af'] for r in rs]), 3),
                            pct=rnd(100 * mean([r['fps'] for r in rs]) / full_fps, 1)))
        return out

    # -- reference windows ------------------------------------------------
    def soak(self):
        """The load's fan-off stretch: everything before the first applied duty."""
        first = next((r for r in self.bench if r['duty'] and r['duty'] > 0), None)
        end = first['s'] if first else self.run_end
        return [r for r in self.bench if r['s'] < end]

    def cooled(self, secs=1200):
        """The last `secs` of the load -- the settled, fully-cooled window."""
        return [r for r in self.bench if r['s'] > self.run_end - secs]

    def after(self):
        """The `end` delay: load off, fan still at full, still logging."""
        return [r for r in self.rows if r['s'] > self.run_end]

    def series(self, step):
        S = {k: [] for k in ('t ts0 it emc pi ph v af rpm duty fps clk eff').split()}
        for r in self.rows[::step]:
            S['t'].append(r['s'])
            S['ts0'].append(rnd(r['die'], 3))
            S['it'].append(rnd(r['it'], 3))
            S['emc'].append(rnd(r['emc'], 2))
            S['pi'].append(rnd(r['pi'], 3))
            S['ph'].append(rnd(r['ph'], 3))
            S['v'].append(rnd(r['v'], 3))
            S['af'].append(rnd(r['af'], 3))
            S['rpm'].append(rnd(r['rpm'], 1))
            S['duty'].append(rnd(r['duty'], 1))
            S['fps'].append(rnd(r['fps'], 1))
            S['clk'].append(rnd(r['clk_lo'], 1))
            S['eff'].append(rnd(r['eff'], 1) if r['state'] == 'running' else NA)
        return S

    def scatter(self, target):
        st = max(1, len(self.bench) // target)
        return [[rnd(r['die'], 1), rnd(r['pi'], 3)] for r in self.bench[::st]
                if r['die'] is not NA and r['pi'] is not NA]

    def eff_scatter(self, target, throttled):
        st = max(1, len(self.bench) // target)
        out = []
        for r in self.bench[::st]:
            if r['die'] is NA or r['eff'] is NA:
                continue
            out.append([rnd(r['die'], 1), rnd(r['eff'], 1), 1 if throttled(r) else 0])
        return out


def write_js(path, name, obj):
    with open(path, 'w') as fh:
        fh.write(f'const {name}=' + json.dumps(obj, separators=(',', ':')) + ';\n')
    print(f'  {os.path.basename(path):12s} {os.path.getsize(path):>9,} B')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hailo', required=True)
    ap.add_argument('--deepx', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--step', type=int, default=16,
                    help='timeline decimation in seconds (default 16)')
    a = ap.parse_args()

    H = Run(a.hailo, 'hailo')
    D = Run(a.deepx, 'deepx')
    print(f'hailo {len(H.rows)} rows, load {H.run_start}..{H.run_end} s')
    print(f'deepx {len(D.rows)} rows, load {D.run_start}..{D.run_end} s')

    # each card's fully-cooled rate: the settled last 20 min of its load
    h_full = mean([r['fps'] for r in H.cooled()])
    d_full = mean([r['fps'] for r in D.cooled()])

    # Hailo throttling is visible in its clock; the DX-M1's clock only ever
    # catches the dither, so "throttled" means a sample that caught it.
    h_top = max(r['clk_hi'] for r in H.bench if r['clk_hi'] is not NA)
    d_top = max(r['clk_hi'] for r in D.bench if r['clk_hi'] is not NA)
    h_thr = lambda r: r['clk_lo'] is not NA and r['clk_lo'] < h_top - 1
    d_thr = lambda r: r['clk_lo'] is not NA and r['clk_lo'] < d_top - 1

    for n, (R, full, thr) in enumerate(((H, h_full, h_thr), (D, d_full, d_thr)), 2):
        write_js(os.path.join(a.out, f'data{n}.js'), f'SESSION{n}', dict(
            start=R.t0.isoformat(timespec='milliseconds'),
            run_start=R.run_start,
            series=R.series(a.step),
            scatter=R.scatter(1250),
            eff_scatter=R.eff_scatter(1500, thr),
            sweep=R.sweep()))

    # ---- overlay: both cards on one time axis, t = seconds into the run ----
    def by_s(R):
        return {r['s']: r for r in R.rows}
    hs, ds = by_s(H), by_s(D)
    t_end = min(H.rows[-1]['s'] - H.run_start, D.rows[-1]['s'] - D.run_start)
    ts = list(range(4, t_end + 1, a.step))
    O = dict(t=ts, h_die=[], d_die=[], h_fps=[], d_fps=[], h_p=[], d_p=[],
             duty=[], h_pct=[], d_pct=[], h_eff=[], d_eff=[])
    for t in ts:
        h, d = hs.get(t + H.run_start), ds.get(t + D.run_start)
        for key, r, full in (('h', h, h_full), ('d', d, d_full)):
            if r is None:
                for k in ('die', 'fps', 'p', 'pct', 'eff'):
                    O[f'{key}_{k}'].append(NA)
                continue
            running = r['state'] == 'running' and r['s'] > (
                H.run_start if key == 'h' else D.run_start)
            O[f'{key}_die'].append(rnd(r['die'], 1))
            O[f'{key}_fps'].append(rnd(r['fps'], 1))
            O[f'{key}_p'].append(rnd(r['pi'], 3))
            O[f'{key}_pct'].append(rnd(100 * r['fps'] / full, 1) if running else NA)
            O[f'{key}_eff'].append(rnd(r['eff'], 1) if running else NA)
        O['duty'].append(rnd(h['duty'], 1) if h else NA)

    O['h_lvl'], O['d_lvl'] = H.lvl(h_full), D.lvl(d_full)

    def ref(R, full):
        sk, cl = R.soak(), R.cooled()
        tail = sk[-600:]
        die, emc = mean([r['die'] for r in tail]), mean([r['emc'] for r in tail])
        p = mean([r['pi'] for r in tail])
        return dict(die=rnd(die, 2), tr=rnd((die - emc) / p, 3),
                    pct=rnd(100 * mean([r['fps'] for r in tail]) / full, 1),
                    eff_soak=rnd(mean([r['eff'] for r in tail]), 1),
                    eff_cool=rnd(mean([r['eff'] for r in cl]), 1))
    O['ref'] = dict(h=ref(H, h_full), d=ref(D, d_full))

    def plateau(R):
        cl = R.cooled()
        return dict(fps=rnd(mean([r['fps'] for r in cl]), 1),
                    die=rnd(mean([r['die'] for r in cl]), 2),
                    w=rnd(mean([r['pi'] for r in cl]), 3))
    O['plateau'] = dict(h=plateau(H), d=plateau(D))

    write_js(os.path.join(a.out, 'overlay.js'), 'OVERLAY', O)
    print(f'cooled rate: hailo {h_full:.1f} fps, deepx {d_full:.1f} fps')
    print(f'overlay: {len(ts)} points, 0..{t_end} s into the run')


if __name__ == '__main__':
    main()
