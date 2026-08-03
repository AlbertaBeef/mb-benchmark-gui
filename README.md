# mb-benchmark-gui

A C++ / GTK4 (gtkmm) desktop GUI that **benchmarks edge-AI NPUs** and charts what
they achieve: frame rate, power, and the two efficiency figures that matter when
comparing accelerators — **fps per watt** and **joules per frame**.

It is the benchmarking companion to [`mb-powermon-gui`](../mb-powermon-gui),
which monitors the same cards' power and temperature. This one adds the load:
it links each vendor's runtime directly and drives inference itself, so the
frame rate on the graph and the watts underneath it come from the same moment on
the same card.

![NPU Benchmarking GUI](assets/screenshot.png)

## What it does

Pick a **model** or a **multi-inference pipeline** from the left panel, tick the
accelerators to run it on, choose a target frame rate (or max speed), and press
Start. One worker thread per card loops inference on its own device; the graphs
update once a second.

Six stacked graphs, each a scrolling 60-second window:

| Graph | What it shows |
| ----- | ------------- |
| **Power (W)** | Live draw per card — on-die sensors and external INA228 shunts. |
| **Temperature (°C)** | Per-sensor die temperatures. |
| **Frequency (MHz)** | Core clock per NPU. Sits right below temperature because a clock sagging while a die heats *is* thermal throttling. Collapsed by default. |
| **Frame Rate (fps)** | Achieved rate per card. An idle card sits at 0. |
| **Efficiency (fps/W)** | *Instantaneous*: this second's frame rate divided by this second's watts. |
| **Energy (mJ/frame)** | This second's watts divided by this second's frame rate — the raw energy cost of a frame. **Lower is better** — the only graph here where that is true. Collapsed by default. |

Power, temperature and frequency come first because they are live whether or not
a benchmark is running; the three below them only mean anything during a run.

All four cards report a clock, each from a different place:

| Card | Series | Source | Idle → load |
| ---- | ------ | ------ | ----------- |
| Hailo-8 | `CLK` | `hailo_extended_device_information_t` | 400 (fixed) |
| DeepX M1 | `C0`–`C2` | `dxrt-cli -s`, same line as the temperature | 1000 (fixed) |
| MemryX MX3 | `C0`–`C3` | SDK `get_frequency_effective()` | **300 → 600** |
| Axelera Metis | `C0`–`C3` | `axcmd --clock-all-actual` | **50 → 800** |

Two of them are DVFS-managed and visibly move, which is the point of the graph:
MemryX idles at half its configured 600 MHz, and Axelera's AI cores idle at
50 MHz against a configured 800. For both, use the *actual/effective* reading —
`get_frequency()` and `--get-ck-profile` return the configured target and would
sit flat however hot the part got.

The Axelera trace also exposes something the frame-rate graph cannot: at one
stream **only `C0` ramps to 800 MHz while `C1`–`C3` stay at 50 MHz**, i.e. three
of the four AI cores are idle. Raising **Streams** in the Axelera tab wakes them
one at a time — see [Per-accelerator controls](#per-accelerator-controls).

**The graphs are never cleared.** Starting or stopping a run does not wipe the
traces, so consecutive runs stay on one axis and can be read against each other
— change the model or the API mode and the step is visible in place. Idle
stretches sit at zero on all three benchmark graphs. The window is still the
rolling 60 s all the graphs share.

`fps/W` and `mJ/frame` are now both raw per-sample figures, which makes them
**exact reciprocals** — the same measurement in two units, not two different
readings. Keep whichever you think in; Energy ships collapsed since Efficiency
already carries the information. Energy is shown in **milli**joules because at
these rates a frame costs on the order of 1–30 mJ, and plain joules would render
as `0.00`.

An idle card reads 0 mJ/frame. On a lower-is-better axis that could be misread
as perfect efficiency — it means "not running", and the frame rate beside it is
0 too.

Every graph uses **one fixed colour per card**, the same everywhere and in the
sibling `mb-powermon-gui`, so a trace is recognisable without reading the legend:

| Hailo | MemryX | DeepX | Axelera | Qualcomm |
| :---: | :----: | :---: | :-----: | :------: |
| Coral `#d9694f` | Sage `#8aa67e` | Slate Blue `#4e7ca1` | Amber `#e0a24b` | Plum `#7e6699` |

Cards appear in the order **Hailo · MemryX · DeepX · Axelera** throughout. An
external INA228 mapped onto a card inherits that card's colour; anything else
(unmapped bridges, the board ambient sensor) falls back to a cycling palette.

The legends are a single compact row of swatches and values and carry no
explanatory text; what is running, which cards are emulated in the current API
mode, and any load failure all go to the **status line** under the Start button
— so a card sitting at 0 always has a reason visible somewhere.

## Supported accelerators

| Card | Sync inference | Async inference | Power | Temperature |
| ---- | -------------- | --------------- | ----- | ----------- |
| **Hailo-8** | `InferVStreams::infer()` | `InferModel::run_async()` | firmware `POW` + INA228 | `TS0` / `TS1`, clock `CLK` |
| **DeepX M1** | `InferenceEngine::Run()` | `RunAsync()` / `Wait()` | INA228 (no on-die sensor) | `T0`–`T2`, clock `C0`–`C2` |
| **Axelera Metis** | `axr_run_model_instance()` | **none** — `double_buffer` only | INA228 | `SYS` / `AI0`–`AI3` (needs a live collector), clock `C0`–`C3` |
| **MemryX MX3** | **none** — 1 frame in flight | `connect_stream()` | MemryX SDK + INA228 | `T0`–`T3`, clock `C0`–`C3` |

Each backend is **optional and auto-detected at build time**. A card whose SDK is
absent still appears in the UI, greyed out with the reason, and its telemetry
still graphs — the build works on any host.

## Sync API vs Async API

The **Inference API** control switches every backend between a vendor's blocking
call and its asynchronous/streaming one. This is the single biggest lever on the
numbers — far bigger than the model choice — so it is worth understanding what
each mode actually drives.

The four SDKs are **not symmetric**, and the UI does not pretend otherwise. In
each mode exactly one vendor has no native equivalent and is emulated; the
status line under the Start button names that card and how, e.g.
`Axelera: double-buffered · no async API`.

**Async is the default**, because it is what the hardware can actually do.

- **Sync** — one frame at a time, waiting for each. What a latency-bound
  real-time pipeline does. Native on Hailo, DeepX and Axelera. MemryX has no
  blocking call at all, so it is emulated by holding exactly one frame in flight.
- **Async** — several frames outstanding. Peak throughput. Native on Hailo
  (`run_async` + `AsyncInferJob`), DeepX (`RunAsync`/`Wait`, 4 jobs deep) and
  MemryX (`connect_stream`, 4 deep). Axelera's runtime exposes **no async
  inference API whatsoever** — 38 `axr_*` entry points, exactly one of which is
  inference, and it blocks — so "async" there means enabling the runtime's own
  `double_buffer` property, which is a much weaker mechanism.

Measured on this host, max speed:

| YOLOv8s | Sync | Async | |
| ------- | ---: | ----: | --- |
| Hailo-8 | 123.0 | **490.7** | 4.0× |
| DeepX M1 | 32.2 | **148.1** | 4.6× |
| Axelera Metis | 131.2 | 154.3 | 1.2× *(double-buffered)* |

| ResNet-50 | Sync | Async | |
| --------- | ---: | ----: | --- |
| Hailo-8 | 297.3 | **1370.1** | 4.6× |
| DeepX M1 | 392.9 | **1086.7** | 2.8× |
| Axelera Metis | 352.1 | 376.1 | 1.1× *(double-buffered)* |

Two things worth drawing out. DeepX's poor *sync* YOLOv8s figure is an artifact
of the mode, not the card — DXRT is built around its job queue, and in async it
gains 4.6×. And Axelera's small gain is the honest consequence of its API: a
double buffer is not a job queue, and no amount of host threading would make it
one.

**Compare within a mode, never across.** A sync number and an async number are
answers to different questions.

## Per-accelerator controls

Below the `Inference` frame is a tab per card, for settings that configure the
*device* rather than the run:

- **MemryX — Core frequency** (200–850 MHz, default 600). 600 is the 14 TOPS
  mode, 850 the 20 TOPS one. Applied before the run starts. There is no C++
  setter — `set_mpu_frequency` exists only in the Python `mxa` module, absent
  from both `memx.h` and `MxAccl` — so this is a one-shot interpreter call at
  load time, never in the timed loop.
- **Axelera — Streams** (1–4). Concurrent model instances, one host thread each.

**Streams matter more than anything else on the Metis.** libaxruntime has no
async API *and* a single model instance occupies only one AI core, which the
Frequency graph shows directly — with one stream `aicore0` runs at 800 MHz while
`aicore1`–`3` sit at 50 MHz. Measured on ResNet-50:

| Streams | fps | AI core clocks |
| ------: | --: | -------------- |
| 1 | 437 | `800 · 50 · 50 · 50` |
| 2 | 671 | `800 · 50 · 800 · 50` |
| 4 | **805** | `800 · 800 · 800 · 800` |

2.1× from 1 → 4, with the cores waking one at a time. Cores are divided across
`stages × streams`, so a two-stage pipeline at two streams still covers the card.

## Models and pipelines

The catalog is **[`config/models.conf`](config/models.conf)** — that file *is*
the Models and Pipelines lists. Adding a model is an edit there, not a rebuild.
Its contents mirror [`envic_ai_cpp`](../envic_ai_cpp)'s pipeline registry, minus
the DeGirum widerface YOLOv8n face detectors, and its download sources are
lifted from the vendor scripts in the sibling project
(`get_hailo8_models.sh`, `get_deepx_models.sh`, `get_axelera_models.sh`).

Entries are **logical** — one row resolves to a different compiled artifact per
vendor (`yolov8s.hef` / `YoloV8S.dxnn` / `yolov8s-coco-onnx`), which is what lets
the same model run on three cards at once and be compared trace against trace.

### Automatic download

Missing artifacts are fetched on first launch into

```
models/<vendor>/<accelerator>/
  hailo/hailo-8      axelera/metis      deepx/dx-m1      memryx/mx3
```

resolved relative to the binary, so the working directory doesn't matter. It runs
on a background thread — the window opens immediately and each row becomes
selectable the moment its artifact lands; until then the card shows in
parentheses, e.g. `Hailo · (DeepX) · Axelera`. Downloads land in a `.part`
sibling and are renamed on success, so an interrupted fetch never leaves a
truncated file that looks like a valid model.

Each vendor's archives have a different shape, so `fetch` in the config says
which to expect:

| `fetch` | Vendor | Shape |
| ------- | ------ | ----- |
| `file` | Hailo, DeepX | `GET <url>/<artifact>`, saved as-is |
| `zip` | Axelera | `GET <url>/<artifact>.zip`, whose `strip` subtree is flattened into a directory named `<artifact>` |
| `zipfile` | MemryX | `GET <url>/<stem>.zip`, from which only the `<artifact>` member is taken |

MemryX archives also carry a `<name>_post.onnx|tflite` head for host-side YOLO
decode; it is deliberately **not** extracted, since no backend runs
post-processing inside the timed loop.

**The first run pulls roughly 1.5 GB** — the Axelera prebuilt zips dominate
(564 MB for `yolov8s-coco-onnx` alone, extracting to ~113 MB). Set
`MB_BENCH_NO_DOWNLOAD=1` to skip fetching entirely and use only what's on disk.
Needs `wget`, plus `unzip` for the Axelera and MemryX archives.

Two artifacts have **no public source** and are marked `source = local` in the
config; they are never fetched, and are used only if you drop them in yourself:

| Artifact | Why |
| -------- | --- |
| `deepx/dx-m1/osnet.dxnn` | Not in the DeepX ModelZoo — compiled locally with DX-COM (see `ai_deepx/models/osnet/deploy.sh` in the sibling project). Without it, DeepX has no People Tracking pipeline. |
| `axelera/metis/arcface_mobilefacenet--112x112_quant_axelera_metis_1` | The Voyager prebuilt catalog ships `facenet-lfw-onnx`, not ArcFace; this is a custom Voyager compile. |

**Models**

| Model | Task | Hailo | DeepX | Axelera | MemryX |
| ----- | ---- | :---: | :---: | :-----: | :----: |
| YOLOv8s / YOLOv8m | person detection, 640×640 | ✅ | ✅ | ✅ | ✅ |
| SCRFD-500M / 2.5G / 10G | face detection, 640×640 | ✅ | ✅ | — | — |
| OSNet x1.0 | person re-identification, 256×128 | ✅ | local | ✅ | — |
| ArcFace MobileFaceNet | face recognition, 112×112 | ✅ | ✅ | local | — |
| ResNet-50 | image classification, 224×224 | ✅ | ✅ | ✅ | ✅ |

(✅ = downloaded automatically; *local* = no public source, see below.)

**Pipelines** — every stage runs once per benchmark frame, in order, on the same
card:

- **People Tracking** — person detection + OSNet ReID (YOLOv8s or YOLOv8m)
- **Face Recognition** — face detection + ArcFace (SCRFD-500M, 2.5G or 10G)

A row with no artifact configured for a vendor simply isn't offered there; a
pipeline is offered only where *every* stage resolves. Nothing here is fatal —
the lists show what you actually have.

## What is being measured

This matters for reading the numbers honestly.

- **Synthetic input, no host pipeline.** There is no camera, no decode, no
  letterbox, no NMS, no drawing. Each stage gets a fixed pseudo-random buffer of
  the model's native input size, prepared once at load. What is timed is the
  accelerator plus its SDK's inference call — not this host's CPU.
- **No host-side conversion in the loop.** Hailo output vstreams are created with
  `HAILO_FORMAT_TYPE_AUTO` (device-native) rather than FLOAT32, so HailoRT does
  not dequantize on the host inside `infer()`. DeepX outputs stay in the
  engine-owned buffers. Axelera outputs are left in their raw padded device
  layout. All three would otherwise be measuring this CPU.
- **One mode at a time, and one call means one frame.** Whichever API mode is
  selected applies to every card in the run, and an async runner tops its
  pipeline up and then blocks until a frame *completes* — so the engine's frame
  counter means the same thing in both modes. See
  [Sync API vs Async API](#sync-api-vs-async-api) for which vendor is emulated
  in which mode; compare within a mode, never across.
- **Pipeline stages run once per frame.** In a real cascade a recognizer runs
  once per *detected object*; a synthetic benchmark has no detections, so each
  stage counts once. (`BenchMember::reps` exists to lift this without a
  redesign.)
- **One subject per card at a time.** Two models sharing a card would split its
  throughput *and* its watts, and neither efficiency figure would mean anything
  per model. The UI enforces one selection at a time.
- **No watts, no efficiency.** A card with no power reading reports a real frame
  rate but sits at 0 on both efficiency graphs, rather than being given an
  invented number. On this host every card is now instrumented, so this only
  applies to a card whose INA228 is unmapped or absent — DeepX and MemryX have
  no on-die sensor at all and depend entirely on their external shunt.
- **Async depth is 4** on DeepX and MemryX, and whatever HailoRT reports as its
  async queue size (capped at 8) on Hailo — deep enough to keep the NPU fed,
  shallow enough that the figure stays a throughput measurement rather than a
  latency-hiding contest.
- **Axelera core allocation.** The card's sub-devices are split evenly across a
  pipeline's stages (a single model gets them all), falling back to one core per
  stage if a model's deploy-time core cap refuses the wider connection. The
  legend shows what it got.

## How it compares to the vendors' own benchmark tools

ResNet-50, Async mode, steady state, measured on this host — against each
vendor's utility run on the *same* machine where possible:

| Card | Vendor tool | Vendor fps | This app | Ratio |
| ---- | ----------- | ---------: | -------: | ----: |
| **Hailo-8** | `hailortcli benchmark` | 1371.3 | **1372.3** | 100 % |
| **DeepX M1** | `dxbenchmark` | ~1009–1070 | **1080.3** | ~101–107 % |
| **MemryX MX3** | `mx_bench -f 50000` | 1795.9 | **1115.8** | 62 % |
| **Axelera Metis** | `inference.py … --aipu-cores 4` (4 streams) | 2054 | **380.3** | 18 % |

Hailo and DeepX land at parity, which is the result that matters most: it says
the async path, the frame accounting and the "one call retires one frame"
contract are all sound. The two shortfalls are both understood, and both are
concurrency, not the device:

- **Axelera — 18 %.** The vendor figure comes from the Voyager *pipeline*
  running **four parallel video streams** with OpenCL preprocessing across 4
  AIPU cores. This app issues a single blocking `axr_run_model_instance` on one
  model instance. Since libaxruntime exposes no async inference API at all, the
  only way to express that concurrency is N model instances driven from N host
  threads — which this app does not (yet) do. Note the comparison is unfair *in
  our favour* on scope — their 2054 fps is end-to-end including video decode,
  ours is inference-only — so the whole gap is parallelism.
- **MemryX — 62 %.** One stream with 4 frames in flight, where `mx_bench`
  drives more. Corroborated by the power: 7.6 W against the vendor run's 11.0 W,
  i.e. the device is not being saturated. More `connect_stream` ids and/or
  `set_num_workers` should close it.

Efficiency, same runs:

| Card | This app | Vendor run |
| ---- | -------: | ---------: |
| Hailo-8 | 321.4 fps/W @ 4.27 W | 343 fps/W @ 4.0 W |
| DeepX M1 | 203.5 fps/W @ 5.30 W | 214 fps/W @ 4.7 W |
| MemryX MX3 | 148.2 fps/W @ 7.49 W | 163 fps/W @ 11.0 W |
| Axelera Metis | 104.2 fps/W @ 3.61 W | 270 fps/W @ 7.6 W |

Efficiency figures are only comparable at comparable *load* — a card that is
being under-driven draws less power but also does proportionally less work, and
the fixed overhead (PCIe, LPDDR) is then amortised over fewer frames. That is
why Axelera's fps/W looks far worse here than in the vendor run: it is idling
between frames, not being inefficient.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mb-benchmark          # needs a display (X11/Wayland)
```

Requires `gtkmm-4.0`. Everything else is optional and reported at configure time:

```
-- Hailo-8 backend:   ENABLED (/usr/local/lib/libhailort.so)
-- DeepX M1 backend:  ENABLED (/usr/local/lib/libdxrt.so)
-- Axelera backend:   ENABLED (/opt/axelera/runtime-1.7.0-1/lib/libaxruntime.so)
-- MemryX MX3 backend: ENABLED (/usr/lib/x86_64-linux-gnu/libmx_accl.so)
-- INA228 power:      ENABLED (libftdi1 1.5)
```

- **HailoRT** — `libhailort` + `/usr/local/include/hailo` (no pkg-config/CMake
  package, found via `find_library`/`find_path`).
- **DXRT** — installed system-wide; ships a CMake package (`find_package(dxrt)`).
- **libaxruntime** — auto-detected under `/opt/axelera/runtime-*`; override with
  `-DAXELERA_DIR=<dir>` (must contain `include/axruntime/axruntime.h` and
  `lib/libaxruntime.so`).
- **memx-accl** — the MemryX runtime. Link **`libmx_accl`**, not `libmemx`:
  the latter is the *driver* library and carries none of the `MxAccl` symbols.
  Headers at `/usr/include/memx/accl/`.
- **libftdi1** — the **1.x** dev package (`apt install libftdi1-dev`), for INA228
  external power. Without it the efficiency graphs stay at 0 for any card with no
  on-die power sensor.

Axelera JIT-compiles each model's `kernel_function.c` with
`riscv64-unknown-elf-gcc` at model load, and that compiler is not on a default
`PATH`. **The app finds it for you** — it looks under `/opt/axelera/*/bin` at
startup and prepends it — so no wrapper script is needed. If your toolchain
lives elsewhere, point `MB_AXELERA_TOOLCHAIN` at the directory containing the
compiler.

Without it, the runtime fails every Axelera model with
`Failed to create executor for model: kernel_function`, which names the model
rather than the missing compiler; the app appends the real explanation to that
message.

## Desktop integration

`mb-benchmark-gui.desktop` is a ready launcher; it points `Exec` at
`build/mb-benchmark`, so the repo has to stay where it is (or edit the path).

```bash
# application menu
install -Dm644 mb-benchmark-gui.desktop ~/.local/share/applications/mb-benchmark-gui.desktop
update-desktop-database ~/.local/share/applications

# icon (its own M_benchmarking, so it is distinguishable from mb-powermon-gui)
install -Dm644 assets/M_benchmarking.svg ~/.local/share/icons/hicolor/scalable/apps/M_benchmarking.svg
gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor
```

For a launcher on the desktop itself, copy it to `~/Desktop` **executable and
marked trusted** — GNOME's `ding` extension needs both, or it renders as a plain
text file rather than an app:

```bash
install -m 755 mb-benchmark-gui.desktop ~/Desktop/
gio set ~/Desktop/mb-benchmark-gui.desktop metadata::trusted true
```

## Configuration

**Model catalog.** [`config/models.conf`](config/models.conf) — vendor download
sources, the model list, and the pipeline definitions. Override its location
with `$MB_BENCH_MODELS_CONFIG`; override where artifacts are stored with
`$MB_BENCH_MODELS_ROOT` (default `<repo>/models`). Both resolved paths are
logged at startup.

**INA228 labels.** The app's config lives in [`config/`](config/), versioned
alongside the source. [`config/ina228.conf`](config/ina228.conf) maps each
FT232H bridge's USB port-path to the accelerator whose rail it measures:

```ini
1-1 = Hailo
1-2 = Axelera
```

Naming an accelerator exactly (`Hailo`, `DeepX`, `Axelera`, `MemryX`) folds that
shunt onto the card's legend row, gives it the card's color, and feeds its fps/W
and mJ/frame figures. An unmapped bridge gets a standalone `INA228#<n>` row
and feeds nothing. Search order:

1. `$MB_INA228_CONFIG`
2. `<repo>/config/ina228.conf` — resolved relative to the binary, so it works
   from any working directory
3. `$XDG_CONFIG_HOME/mb-benchmark-gui/ina228.conf` (else `~/.config/…`)

## Known issues

### Fixed — kept here because the symptoms are misleading

If you see one of these again, this is what it means:

- **INA228 bridges not detected after a reboot, but fine after unplug/replug.**
  A udev rule granting access to the FT232H is overridden by the system
  defaults if its filename sorts *before* them. `MODE` is a plain assignment and
  the last one wins, so `11-ftdi.rules` loses to `50-udev-default.rules`
  (`SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", MODE="0664"`) and the nodes
  come up `crw-rw-r-- root root` — unopenable by a normal user. Name the rule so
  it sorts *after* 50:

  ```bash
  sudo mv /etc/udev/rules.d/11-ftdi.rules /etc/udev/rules.d/99-ftdi.rules
  sudo udevadm control --reload
  sudo udevadm trigger --action=add --subsystem-match=usb   # no replug needed
  ```

  Diagnose with `udevadm test /sys/bus/usb/devices/<port>` — it prints every
  `MODE` assignment and which rule made it. Note the devices are tagged `seat`
  but **not** `uaccess`, so logind grants no ACL; the file mode is all there is.

- **`Gtk-CRITICAL … gtk_list_box_get_selected_row: assertion 'GTK_IS_LIST_BOX
  (box)' failed`, then `Segmentation fault` on exit.** GTK widget members are
  destroyed in *reverse* declaration order, so the two `Gtk::ListBox`es died
  before the `Gtk::Notebook` that outlives them; tearing the notebook down then
  emitted `switch_page`, whose handler read the already-destroyed list box.
  Fixed by severing every widget-signal connection in `~ControlPanel` — the
  destructor body is the last moment at which all the widgets are still alive —
  and by disconnecting the 1 Hz tick in `~MainWindow` so it cannot fire into a
  window that is coming apart.
- **`BrokenPipeError: [Errno 32] Broken pipe` traceback on exit.** Cosmetic, and
  not from the app itself: the MemryX power helper is a small Python subprocess,
  and when the app exits its stdout pipe closes mid-write. Python printed the
  traceback to the stderr it inherited — your terminal. The helper now exits via
  `os._exit(0)` on a write failure, which skips both the traceback and the
  interpreter's shutdown flush.

- **`corrupted double-linked list`, intermittently, in Async mode.** Not heap
  corruption in the usual sense: HailoRT ran a completion callback on its own
  worker thread after the per-model state it writes into had been destroyed.
  Detached async jobs are still in flight at teardown unless you wait for them.
  Reproduced on every YOLOv8s run and *never* on ResNet-50 — the fast model
  drained before teardown, which made it look model-specific when it was purely
  a race. Fixed by draining in-flight jobs and tearing down the configured model
  before the slot bookkeeping.
- **`[HailoRT] [warning] read_async() was provided an unaligned buffer`.** Buffers
  handed to HailoRT must be page-**aligned** *and* page-**rounded**; a plain
  `std::vector` is neither. Beyond the performance note in the warning,
  `infer_model.hpp` warns that an under-sized *output* allocation risks memory
  corruption past the last page. Fixed with a page-aligned allocator.
- **`Failed to create executor for model: kernel_function` on every Axelera
  model.** Reads like a bad model; it is a missing compiler. The Voyager runtime
  JIT-compiles each model's `kernel_function.c` with `riscv64-unknown-elf-gcc`,
  which is not on a default `PATH`. The app now locates the toolchain itself.
- **All Axelera models silently skipped at download time.** The tool-presence
  probe ran `unzip --version` — not a valid invocation, so unzip printed its
  usage and exited non-zero, and the fetcher concluded unzip was missing. Now the
  test is whether the binary can be *started*, not its exit status.
- **A graph section that would not grow when expanded.** `vexpand` was sampled
  once at construction, so a section that started collapsed could never share
  vertical space, and one that started expanded kept claiming it after being
  collapsed. Now bound to the expanded state.
- **An accelerator checkbox staying off after switching models.** Cards are
  unticked when the selected model has no build for them; the user's intent is
  now remembered separately and restored when a model that supports the card is
  selected again.

## Notes

- Only one process can hold a Hailo device's power measurement at a time.
  Running `mb-powermon` / `mb-powermon-gui` alongside this will fail one of them
  with `DVM_ALREADY_IN_USE` — stop one.
- **Axelera temperatures need two gates cleared, and both are lost on every
  reboot** — the card's runtime firmware is RAM-only and nothing loads it at
  boot. Until then the Axelera row shows no values and the app logs
  `collector idle`. [`tools/axelera-temps.sh`](tools/axelera-temps.sh) clears
  both (loads the firmware if the card is still in bootloader state, then sets
  the collector to `inf`), and
  [`tools/axelera-temps.service`](tools/axelera-temps.service) runs it once per
  boot:

  ```bash
  sudo cp tools/axelera-temps.service /etc/systemd/system/
  sudo systemctl daemon-reload
  sudo systemctl enable --now axelera-temps.service
  ```

  The script is idempotent, so running it by hand at any time is safe. Note the
  collector level is a *global* device log setting — that is why the app's probe
  never sets it itself, staying passive and reporting `collector idle` instead
  of changing device state behind your back.
- Kill stray instances with `pkill -x mb-benchmark` (exact name) — `pkill -f
  build/mb-benchmark` also matches your own shell command.
