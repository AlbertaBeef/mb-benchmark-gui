# MemryX MX3: the card wedges at ~1796 fps on **both** hosts

**Status: open. The Strix Halo capture is in (2026-09-14) and it overturns the
premise this file was written on.** The wedge is not EPYC-specific — the Strix
Halo machine reproduces it too, and does so on the **unchanged May software
stack** that produced the published 200 000-frame runs. Two of the four
suspects are dead, one is promoted, and the decisive experiment has moved to
the Strix Halo machine.

Read **What the Strix Halo machine measures** before acting on anything in the
EPYC sections — several of their conclusions are superseded.

## The question

`mariobergeron.com/posts/edge-ai-power-p06-memryx-mx3/` reports `mx_bench` runs
made **2026-05-21 … 06-03 on an AMD Strix Halo machine**, at 600 MHz / 14 TOPS:

| run | result |
| --- | --- |
| `mx_bench -v -d ResNet_50_MXA_Optimized_224_224_3_onnx.dfp -f 50000` | 1796.36 fps |
| same, repeated | 1796.36 fps |
| same, repeated | 1796.42 fps |
| `-f 100000` | 1796.46 fps |
| **`-f 200000`** | **1796.54 fps** |

200 000 frames at 1796 fps is **111 seconds at full rate, with no failure**.
The article records power rising 11.0 → 11.4 → 12.1 W across consecutive runs
and die temperature reaching **~65 °C, ~70 °C, then ~90 °C** — it got hot and
kept working.

On the **EPYC host** (`abbeefepyc-ROMED8-2T`) the same workload kills the card
within 11–80 s, every time, and only a power cycle recovers it.

**Why?**

## What the EPYC host measures

From `mb-benchmark-gui-memryx-brown-out-00.csv` (untracked, in the repo root on
that machine) — both runs are in the one file, ~50 s apart:

| | depth 4 | depth 8 |
| --- | ---: | ---: |
| fps | 1180 | 1796 |
| INA228 power | **7.9 W** | **11.9 W** |
| die T0 | **60 °C, flat** | **54 → 70 °C, +0.7 °C/s, never plateaus** |
| VBUS | 3.147 V | 3.079–3.092 V |
| outcome | fine | **dead 23 s in** |

At the moment of death fps goes 1803 → 272 → 0, INA228 power drops 12.0 → 1.02 W,
and every die sensor returns the `65262000` millidegree sentinel (−274 °C, gated
to NaN by `plausible_temp()`). The kernel logs
`memryx: admin timeout device status 1 subop N chip 0`.

Five reproductions, all at depth 8 / ~1796 fps: wedged after **80, 80, 57, 33
and 11 s**.

### What it is not

- **Not thermal.** The article's card reached **90 °C and survived**; this one
  dies at **70 °C**. Depth 4 here has run to **86 °C for 244 s** without
  trouble. Temperature is not the discriminator.
- **Not our harness.** `mx_bench -f 200000` wedges identically at ~57 s on the
  EPYC host. (An earlier `-f 30000` comparison looked clean and was invalid:
  at 1796 fps that is only 16.7 s, shorter than the failure window.)
- **Not the `mxa-manager` daemon.** One reproduction was `local_mode`, which the
  daemon never sees.
- **Not brown-out.** VBUS read 3.074 V at a wedge, 74 mV above the 3.003 V M.2
  floor, and the latched INA228 undervoltage detector (BUVL 3.00 V, ALATCH) has
  never tripped.
- **Not a frame count.** ~143 k frames at depth 8 against ~269 k at depth 4 with
  no trouble.
- **Not host CPU.** During a run the busiest host thread uses 19% of one core
  and the whole process 75% of one core, on 128 cores.

### Two observations that point in opposite directions

1. **Toward a software regression.** In the article, overloading the card
   **degraded gracefully** — the 850 MHz runs went 2335 → 1887 → 1425 fps as it
   throttled. Here it does not throttle, it stops answering. A part that used to
   back off and now dies is a driver-layer behaviour change.
2. **Toward hardware degradation.** Time-to-wedge is **monotonically
   decreasing**: 80 → 80 → 57 → 33 → 11 s. A version difference should fail at
   roughly consistent times, not progressively sooner.

   **Superseded 2026-09-14.** Strix Halo's two reproductions ran 64 s then
   79 s — *increasing*. Pooled over all seven (80, 80, 57, 33, 11 / 64, 79)
   there is no monotone trend, so the ordering on the EPYC host was noise and
   carries no evidence of degradation. Do not re-derive a degradation argument
   from run order alone.

Nothing so far distinguishes these. **The capture below is what would.**

## EPYC host configuration (2026-09-14)

Produced by `tools/mx3-capture-env.sh`:

```
kernel                 6.17.0-40-generic        (installed 2026-07-15)
cpu                    AMD EPYC 7B13 64-Core
distro                 Ubuntu 24.04.4 LTS

memx-accl              2.2.5-1   ]
memx-drivers           2.2.4-1   ]  all installed 2026-07-17, no prior version
mxa-manager            2.2.5-1   ]  in a dpkg log covering 2025-08-05 .. now
python memryx          2.2.2        (venv, 2026-07-19) -- this is what mx_bench is
module version         1.3.13.1

pcie  0000:c1:00.0     8.0 GT/s x2  (max x2)   <-- only card here below x4
      hailo / deepx / axelera       8.0 GT/s x4
```

Two things stand out:

- **Everything post-dates the article.** The kernel moved 6.8.0 → 6.17.0 on
  2026-07-15 and the whole MemryX stack was installed 2026-07-17. The dpkg log
  has no gap and no `remove`/`purge`, so the article's stack was never installed
  through apt on this machine — consistent with the runs having been made
  elsewhere.
- **Version skew**: Python SDK 2.2.2, C++ runtime 2.2.5, drivers 2.2.4.

## What the Strix Halo machine measures (2026-09-14)

Capture: `docs/mx3-env-strixhalo.txt`.

### It wedges here too, and it already had

Two reproductions on this machine on **2026-09-08**, in the app's own log
(`mb-benchmark-gui-amd-halo-memryx-brown-out-01.csv` and `-02.csv`, untracked,
repo root on this machine). They were taken before this file was written and
were not consulted when it was:

| | halo-01 | halo-02 | EPYC |
| --- | ---: | ---: | ---: |
| depth / mode | 8 · local | 8 · local | 8 |
| peak fps | 1798 | 1799 | 1796–1803 |
| survived | **64 s** | **79 s** | 11–80 s |
| T0 at wedge | 67 °C | 69 °C | 70 °C |
| die sensors after | `65262000` sentinel | sentinel | sentinel |
| app message | `MemryX inference timed out` | same | same |

Same workload, same signature, same recovery requirement. **The question this
file asks — why does one host wedge and the other not — has no answer because
its premise is false.**

### Host comparison

| | Strix Halo | EPYC |
| --- | --- | --- |
| cpu | Ryzen AI MAX+ 395 (Strix Halo) | EPYC 7B13 |
| kernel | **7.0.0-31-generic** | **6.17.0-40-generic** |
| memx-accl | **2.2.2-1** | **2.2.5-1** |
| memx-drivers | **2.2.1-3** | **2.2.4-1** |
| mxa-manager | 2.2.2-1 | 2.2.5-1 |
| module version | 1.3.13 | 1.3.13.1 |
| python `memryx` | 2.2.2 | 2.2.2 |
| stack installed | **2026-05-19**, never upgraded | 2026-07-17 |
| pcie | 8.0 GT/s **x2, max x2** | 8.0 GT/s **x2, max x2** |
| idle die temp | **37–40 °C** | **57–59 °C** |

### Suspects, re-ranked

- **#2 SDK version — NOT dead. This bullet was wrong and is retracted**
  (see *The SDK upgrade*, below). The reasoning was: Strix Halo runs the exact
  2.2.2-1 / 2.2.1-3 stack the published runs were made on and wedges anyway,
  therefore 2.2.4/2.2.5 did not introduce this. The premise is true and the
  conclusion does not follow — "the old stack also fails" does not make the two
  stacks equivalent. Measured 2026-09-14 on 2.2.5: our depth-8 path survives
  **309 s instead of 64–79 s**. The SDK is the only thing that has moved the
  failure at all so far.
- **#3 PCIe width — dead.** `x2` equals `max_link_width` on *both* hosts, so it
  is the module's native width, not a link-training fault. Nothing to test.
- **#4 cooling — dead as the discriminator.** Strix Halo idles 20 °C cooler
  than EPYC (37–40 vs 57–59) and still wedges, at 67–69 °C. Consistent with the
  article's card surviving 90 °C: temperature does not decide this.
- **#1 kernel — promoted, and the test belongs on this machine.** It is now the
  only structural variable left, and Strix Halo offers a **single-variable**
  experiment that EPYC cannot, because its SDK is provably unchanged across the
  working → failing transition:

  | | kernel | SDK | result |
  | --- | --- | --- | --- |
  | Strix Halo, 2026-05/06 | **6.17.0-23 / -29** (or -1020/-1023-oem) | 2.2.2 / 2.2.1 | 200 000 frames, 111 s, **survived**, 90 °C |
  | Strix Halo, 2026-09-08 | **7.0.0-31-generic** | *identical* | **wedged at 64 s and 79 s** |

  Note this **kills the 6.8-vs-6.17 framing above**: Strix Halo never ran 6.8.
  In May it was on 6.17.0 and working; EPYC is on 6.17.0-40 today and failing.
  So if a kernel is responsible, the boundary lies *inside* 6.17.0 or above it,
  not at the 6.8 → 6.17 jump — and rolling EPYC back to 6.8.0-139 tests a
  configuration that never worked here in the first place.
- **New: two different physical cards both wedge.** The two captures were taken
  19 minutes apart on 2026-09-14, each showing a live bound module (EPYC at
  `0000:c1:00.0` idling at 57 °C, Strix Halo at `0000:c5:00.0` at 37 °C), so
  one module cannot account for both. That removes card degradation as the
  explanation for the *class* of failure, though it cannot rule it out for any
  individual card.

### One difference worth chasing, not yet explained

The kernel-level signature is **not** the same on the two hosts:

```
EPYC         memryx: admin timeout device status 1 subop N chip 0
Strix Halo   memryx: fops_write: wait timeout 1(s), retrying again   (repeating)
             memryx: fops_read:  wait timeout 10(s), retrying again
```

`admin timeout` appears **zero** times in the entire Strix Halo journal. Most
likely this is a logging difference between module 1.3.13 and 1.3.13.1 rather
than two different faults — both are "the chip stopped answering" — but that is
an assumption, not a measurement. Check the 2.2.1 driver source for the string
before treating the two as the same event.

## The kernel is exonerated on this machine (2026-09-14, run before any reboot)

Before spending a reboot on the rollback, the baseline was re-run on the
**current** kernel. It does not fail. Three consecutive runs, the exact May
command and artifact, nothing else touching the card:

```bash
mx_bench -v -d ResNet_50_MXA_Optimized_224_224_3_onnx.dfp -f 200000
```

| run | frames | fps | peak die T | outcome |
| --- | ---: | ---: | ---: | --- |
| 1 | 200 000 | **1796.51** | 73 °C | completed, 116 s |
| 2 | 200 000 | **1796.51** | 87 °C | completed, 116 s |
| 3 | 200 000 | **1796.51** | **99 °C** | completed, 115 s |

600 000 frames, ~347 s of unbroken 1796 fps, back to back with only 5 s
between runs so the card climbed the whole way. Article baseline: **1796.54**.
The card was healthy afterwards at 86–89 °C.

**So on kernel 7.0.0-31 with the May SDK, the published workload still runs
perfectly.** Consequences:

- **The kernel rollback is off.** There is nothing for 6.17.0-29 to fix here;
  do not install it. This is exactly why the baseline was run first.
- **Thermal is dead, conclusively.** 99 °C sustained and working, *hotter* than
  the article's 90 °C, while the app wedges at **67–69 °C**. Temperature is not
  merely "not the discriminator" — the failing case is the cool one.
- **Card degradation is dead for this module.** It does today what it did in
  May, to four significant figures.

### Which means the discriminator on this host is our runner

| host | `mx_bench -f 200000` | our app, depth 8 |
| --- | --- | --- |
| **Strix Halo** | **3/3 completed**, 1796.51 fps, to 99 °C | **wedges** at 64 s, 79 s, at 67–69 °C |
| **EPYC** | **wedges** at ~57 s | wedges at 11–80 s |

That asymmetry is new and it matters: on EPYC the vendor's own tool fails, on
Strix Halo it does not. **These may well be two different faults that this file
has been treating as one** — a host/SDK-side failure on EPYC that takes down
even `mx_bench`, and something specific to our depth-8 path here.

The CLAUDE.md bullet "**not our runner** — `mx_bench -f 200000` wedges
identically" is therefore true of EPYC **only**, and must not be quoted as a
general result.

### The concrete lead: we hold more frames in flight than mx_bench does

`mx_bench` reports its own latency, so Little's law gives its pipeline depth
directly:

```
1796.51 fps x 3.27 ms = 5.87 frames in flight
```

It reaches **the same 1796 fps we need 8 permits for, with ~6 frames
outstanding.** Our `connect_stream` permit count is a hard 8. So at equal
throughput our runner is pushing meaningfully more concurrency into the card
than the vendor tool ever does — and depth 8 is the one invariant across all
seven wedges.

This also reframes depth 6, which this file already lists as "untested for
stability and may well be fine": it measures 1597 fps, and it is the setting
closest to what `mx_bench` actually does. It is now the **cheapest** next test
and needs no reboot.

## The SDK upgrade (2026-09-14, after a power cycle)

Strix Halo was moved to EPYC's exact combination and power-cycled. Verified
loaded before measuring — module **1.3.13.1** (was 1.3.13):

| package | was | now |
| --- | --- | --- |
| `memx-accl` | 2.2.2-1 | **2.2.5-1** |
| `memx-drivers` | 2.2.1-3 | **2.2.4-1** |
| `mxa-manager` | 2.2.2-1 | **2.2.5-1** |
| `memx-accl-plugins-d12` | 2.2.0-1 | **2.2.5-1** |

The Python `memryx` package stayed at **2.2.2** — identical to EPYC — so
`mx_bench` itself is the same program on both stacks and only the system
`libmemx` / `libmx_accl` it links changed. The installer reported *"Latest
firmware already installed"*, so card firmware is constant across the
comparison.

### `mx_bench` still completes: 3/3

| run | frames | fps | peak die T | outcome |
| --- | --- | ---: | ---: | --- |
| 1 | 200 000 | 1796.51 | 75 °C | completed |
| 2 | 200 000 | 1796.49 | 89 °C | completed |
| 3 | 200 000 | **1596.78** | **100 °C** | completed |

So the vendor tool is now **6/6 across both SDK stacks** on this machine, and
EPYC's `mx_bench` failure is not reproduced by adopting EPYC's SDK. Whatever
kills `mx_bench` on EPYC is not in these four packages.

**Run 3 is the important one: the card throttled and lived.** 1596.78 /
1796.51 = 0.889, consistent with an MPU clock step to ~533 MHz. Graceful
degradation is intact on this stack — which contradicts the "it does not
throttle, it stops answering, therefore driver regression" reading under *Two
observations* above.

### Our harness: 309 s instead of 64–79 s, and it still dies

ResNet-50, depth 8, local mode, through the GUI (`depth 8 · local`,
`Async API · depth 8` in the CSV), logged at 1 Hz:

```
 t=  0   1796 fps   10.51 W   43 °C   C0..C3 = 600 MHz
 t= 60   1796 fps   10.91 W   67 °C     <- both 2.2.2 runs were already dead here
 t=180   1796 fps   11.42 W   85 °C
 t=300   1796 fps   11.98 W   99 °C
 t=308   1796 fps   12.03 W   99 °C
 t=309   1205 fps            all four sensors -> 65262000 sentinel
 t=310      0 fps
```

| stack | survived | T at wedge | fps held |
| --- | ---: | ---: | ---: |
| 2.2.2 / drv 2.2.1 | 64 s | 67 °C | 1798 |
| 2.2.2 / drv 2.2.1 | 79 s | 69 °C | 1799 |
| **2.2.5 / drv 2.2.4** | **309 s** | **99 °C** | 1797 |

**~4x longer and 30 °C hotter.** Throughput was flat to ±0.2% for the whole
309 s (min 1792, max 1797) — no decay, no warning, then one partial second and
gone. Kernel signature unchanged: `fops_write: wait timeout 1(s), retrying
again`, this machine's form of it.

### The sharpest lead so far: we never throttle, and `mx_bench` does

**All four MPUs read a steady effective 600 MHz from t=1 through 99 °C and into
the wedge itself.** No DVFS step at any point. Set that beside `mx_bench` run 3
on *the same card, the same SDK, the same temperature*: it stepped down to
~533 MHz and finished.

So the card's thermal response demonstrably works on this stack — it just does
not engage for our depth-8 path, which stays pinned at full clock until it
stops answering. Whether that is because our submission pattern prevents the
backoff, or because the wedge simply arrives first, is exactly what the next
two tests separate.

**Three wedge temperatures now: 67, 69 and 99 °C.** No single thermal threshold
explains them, so "not thermal" still stands as originally written — but this
run reached the thermal ceiling and died there, which the earlier two did not.

## The next experiment

On **Strix Halo**, no reboot, in this order:

1. **Soak our app at depth 6** (1597 fps) for >400 s — comfortably past the
   new 309 s window, not the old 64–79 s one. Note 1597 is almost exactly the
   rate the card throttled *itself* to under `mx_bench` at 100 °C, which makes
   this the most interesting single number in the investigation. If it
   survives, the shipping answer is a default.
2. **Depth 8 capped to ~1600 fps** with `FramePacer` (`target_fps`). Same
   permit count, same rate as a throttled `mx_bench`. This is the test that
   separates *"8 permits is the problem"* from *"1796 fps is the problem"* —
   nothing measured so far distinguishes them, and they imply different fixes.
3. **Watch `_C0.._C3` in the CSV on both**, because the clock is now the
   discriminator, not the frame rate. A run that throttles and lives is a
   different outcome from one that holds 600 MHz and lives.

Every wedge costs a power cycle, so run them one at a time and read the log
before starting the next.

On **EPYC**, unchanged and independent: its `mx_bench` failure is a separate
question, and the SDK/kernel differences there are still unexplored. Do not
assume this machine's answer transfers.

## The measurement to reproduce

Identical on both machines, so the comparison is direct:

```bash
mx_bench -v -d ResNet_50_MXA_Optimized_224_224_3_onnx.dfp -f 200000
```

Article baseline (Strix Halo, May 2026): **1796.54 fps, ran to completion.**
EPYC host today: **wedges at ~57 s**, needs a power cycle.
Strix Halo today: **wedges at 64–79 s** through the app at depth 8; the
`mx_bench -f 200000` form has **not** been re-run here since May — that is the
next experiment above.

> **A short run proves nothing.** `-f 30000` is 16.7 s at this rate, below the
> shortest observed failure window. Use `-f 200000` or the result is not
> comparable — this mistake was already made once.

## Consequences already applied here

- The app's MemryX depth default is **4**, not 8 (`kAsyncDepth` in
  `bench_memryx.cpp`, `def_depth` in `ControlPanel.cpp` — both must agree).
  Depth 8 is genuinely faster (1077 / 1597 / 1796 fps at depth 4 / 6 / 8) and is
  not survivable on this host. **Raising it again needs evidence from this
  investigation, not the throughput argument** — that argument has been made and
  lost twice, at the cost of several power cycles.
- Depth 6 (1597 fps) is untested for stability and may well be fine; it needs a
  soak before it can be a default.
