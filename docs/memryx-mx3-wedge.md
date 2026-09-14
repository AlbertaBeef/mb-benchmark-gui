# MemryX MX3: the card wedges at ~1796 fps on the EPYC host, but did not on Strix Halo

**Status: open. Waiting on a configuration capture from the AMD Strix Halo
machine.** This file is the handoff — it carries everything measured on the
EPYC host so the other machine can take the next step without re-deriving any
of it.

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

## What to run on the Strix Halo machine

```bash
./tools/mx3-capture-env.sh > docs/mx3-env-strixhalo.txt
```

Read-only — sysfs, dpkg and lspci only. No device access, no root needed.
Commit the output next to this file.

**The single most important question it answers is not a version at all:**

> **Is the MX3 in the EPYC box the same physical module that produced the
> article's runs, or a second card?**

- **Same card** → it sustained 12 W / 1796 fps / 90 °C in June and now dies at
  70 °C, so the difference is entirely host-side (kernel, SDK, PCIe width, power
  delivery, cooling) and every item below is testable.
- **Different card** → this module may simply be the weaker one, the 80 → 11 s
  trend is degradation, and no software rollback will reproduce June.

Then compare, in this order of suspicion:

| # | variable | why it is suspected | how to test |
| --- | --- | --- | --- |
| 1 | **kernel** 6.8.0 vs 6.17.0 | the failure is a *kernel-driver* timeout, and DKMS rebuilt the unchanged 2.2.4 driver source across a 6.8 → 6.17 jump (DMA-API / PCIe / IOMMU churn) | `linux-image-6.8.0-139-generic` is still in noble-updates; install, reboot, let DKMS rebuild, re-run |
| 2 | **SDK version** | the whole stack is newer than the article's, and the install is skewed (2.2.2 / 2.2.4 / 2.2.5) | MemryX's repo still carries `memx-accl` 2.2.4/2.2.2/2.2.1/2.2.0 and `memx-drivers` 2.2.1-3/2.2.0-1. Userspace-only downgrade needs **no reboot** |
| 3 | **PCIe width** | MX3 runs **x2** on the EPYC host while the other three cards are x4. Probably native for the M.2 module, but unverified | the capture reports it on both machines |
| 4 | **cooling / airflow** | EPYC chassis has a known thermal problem — only two fans reporting, at 500 and 800 RPM | compare idle and loaded die temps |

Cheapest first: **the SDK downgrade needs no reboot; the kernel rollback does.**

## The measurement to reproduce

Identical on both machines, so the comparison is direct:

```bash
mx_bench -v -d ResNet_50_MXA_Optimized_224_224_3_onnx.dfp -f 200000
```

Article baseline: **1796.54 fps, ran to completion.**
EPYC host today: **wedges at ~57 s**, needs a power cycle.

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
