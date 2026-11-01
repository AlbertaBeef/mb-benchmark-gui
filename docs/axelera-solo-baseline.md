# Axelera solo baseline — before the capture/IOMMU mods

Taken deliberately as the **control** for the `docs/host-freeze-capture.md`
changes, so that a later run under `console=ttyS0`, `crashkernel=`, the fatal
lockup sysctls and `amd_iommu=off` can be compared against a known-good number
on the same binary and the same artifact.

**Why it is needed:** the app records nothing about the kernel command line, so
a CSV from a modified boot looks identical to one from a stock boot. Without a
labelled control, a later comparison against "1967 fps" would be comparing two
different machines — the mistake this project has already made twice with
silently-differing configurations.

## The run

`logs/mb-benchmark-20261101-121654.csv`, 2026-11-01 12:16:54, GUI,
**Axelera alone** — no other card enabled, no stagger.

| | |
| --- | --- |
| kernel cmdline | **stock**: `ro quiet splash vt.handoff=7` — no `console=`, no `crashkernel=`, **IOMMU on** (83 groups, interrupt remapping + X2APIC enabled) |
| lockup sysctls | all 0 (`hardlockup_panic`, `panic_on_oops`, `panic`) |
| `axelera_cfg` | `4x224x224x3 int8 · batch 4 · 4 AIPU cores · 1 instance · no double buffer · 1 MB/instance` |
| API | Sync, depth 1 |
| duration | two runs, ~39 s and ~124 s, 163 running samples total |

| metric | value |
| --- | ---: |
| fps max | **1662** |
| fps mean | **1605** (1614 excluding the first sample) |
| rail peak | **6.57 W** |
| rail VBUS min | **3.137 V** |
| `Link down` / MSI timeout / DMA WR errors this boot | **0** |
| card after | healthy, 0 DMA errors |

Consistent with the other no-double-buffer solo figures (1618.6 standalone,
1670 max / 1593 mean on 2026-10-31), and with the ~1960 fps that double
buffering **on** gives — this run had it **off**, which is the shipped default.

## How to use it

After the mods are applied and `/proc/cmdline` confirms them, repeat **exactly
this**: Axelera alone, Sync, 4 cores, no double buffering, a couple of minutes.

- **fps within a few percent of 1605-1662** → the mods are performance-neutral
  and any later concurrency result is trustworthy.
- **fps far below that** → suspect SWIOTLB bounce buffering, which is the one
  real risk of `amd_iommu=off` on a 251 GB host: a card that cannot address all
  of RAM must copy every buffer through the 64 MB pool. Check with
  `dmesg | grep -i swiotlb` for "buffer is full".
- **long stalls or an apparent hang** → suspect the serial console, not the
  card. `axl: Previous async DMA transfer not cleaned up` is documented to
  flood ~14k messages per boot at Axelera teardown, and at 115200 baud that is
  ~90 s of synchronous blocking. Drop `console=ttyS0` or add `loglevel=4`.

Only then move to the concurrency case (Axelera first, DeepX second, 5 s
stagger) that `docs/host-freeze-capture.md` step 3 is for.
