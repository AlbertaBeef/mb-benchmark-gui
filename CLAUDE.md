# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

`mb-benchmark-gui` is a C++17 / **gtkmm-4.0** desktop GUI that **benchmarks**
edge-AI NPUs — it links each vendor's runtime and drives inference itself, then
charts the achieved frame rate alongside the power it cost.

It was built from `../mb-powermon-gui` and shares its chrome (teal
`Gtk::HeaderBar`, brand palette, `Gtk::Expander` graph sections) and three files
copied nearly verbatim: **`Probes.{h,cpp}`, `GraphArea.{h,cpp}`, `util.h`**.
Those are the *same code*, not a fork with intent to diverge — when fixing a bug
in a probe, fix it in both. Only two deliberate edits to `Probes.cpp`: the
INA228 config-path search (see **Config** below) and the added
`Probes::power_for_device()`.

The model/pipeline catalog is derived from `../envic_ai_cpp`'s
`ai_common/pipeline_registry.cpp` — same models, same detector+recognizer
pairings, **minus the DeGirum widerface YOLOv8n face detectors** (deliberate;
don't add them back without being asked). It now lives in `config/models.conf`
rather than in code; see **Config**.

Window title: **"NPU Benchmarking GUI"**. Binary: `./build/mb-benchmark`.

## Config

App config lives in **`config/`** in the repo, versioned with the source — not
in `~/.config`. Everything there resolves **relative to the binary**
(`<exe>/../config/`, since the binary sits in `build/`), so it works from any
working directory. Put any future config file here and resolve it the same way.

- **`config/models.conf`** — the catalog *and* the download sources. This file
  is what populates the Models and Pipelines lists; nothing about which models
  exist is compiled in. Sections are `[vendor:*]`, `[model:*]`, `[pipeline:*]`;
  `Catalog.cpp` has a ~40-line INI reader (deliberately hand-rolled so Catalog
  keeps zero glib/GTK dependency and stays headlessly testable). Override with
  `$MB_BENCH_MODELS_CONFIG`. Vendor URLs come from the sibling project's
  `get_<vendor>_models.sh`; re-check those scripts before changing a base URL.
- **`config/ina228.conf`** — USB port-path → accelerator name for the INA228
  shunts. Order is `$MB_INA228_CONFIG` → repo `config/` →
  `~/.config/mb-benchmark-gui/`. It does **not** read the sibling
  `mb-powermon-gui`'s copy — that fallback existed briefly and was removed on
  purpose; don't reintroduce it. Verify which file is actually read with
  `strace -f -e trace=openat ./build/mb-benchmark 2>&1 | grep ina228` — cheaper
  than guessing, since a wrong-but-present fallback gives identical labels.

## Models tree and downloading

Artifacts live in `models/<vendor>/<accelerator>/` (`hailo/hailo-8`,
`axelera/metis`, `deepx/dx-m1`, `memryx/mx3`), overridable with
`$MB_BENCH_MODELS_ROOT`. `Fetcher` pulls anything missing on startup, on its own
thread; `MainWindow::poll_downloads()` re-runs `Catalog::refresh_presence()` and
`ControlPanel::refresh_availability()` as each lands, so rows go live mid-download.

- **A first run is ~1.5 GB** — the three Axelera prebuilt zips are ~1.1 GB of it
  (564 MB for yolov8s alone). `MB_BENCH_NO_DOWNLOAD=1` skips fetching; use it for
  any test where you don't want the network.
- Three archive shapes, one `fetch` kind each: `file` (Hailo, DeepX — save as-is),
  `zip` (Axelera — flatten the `strip` subtree into a directory named after the
  artifact), `zipfile` (MemryX — take just the one named member from the archive
  root). MemryX archives also carry a `<name>_post.onnx|tflite` decode head that
  is deliberately not extracted; no backend runs post-processing in the timed loop.
- **No shell anywhere in Fetcher.** `run_cmd()` uses `posix_spawnp` with an
  explicit argv, so config-supplied artifact names can't become shell syntax —
  and unzip gets its member glob (`build/%m/%m/1/*`) unexpanded, which is what it
  needs. Don't "simplify" this to `system()`.
- Everything downloads to a `.part` sibling and is renamed on success, so an
  interrupted fetch can't leave a truncated file that passes the presence check.
- Two artifacts are `source = local` (no public URL): DeepX `osnet.dxnn` (DX-COM
  compiled) and the Axelera ArcFace directory (custom Voyager compile). They are
  never fetched, only used if present. Don't invent URLs for them — the Voyager
  catalog ships `facenet-lfw-onnx`, not ArcFace, and the DeepX ModelZoo has no
  OSNet at all.
- **MemryX models come from the Model Explorer**
  (`https://developer.memryx.com/model_explorer/2p2/<Name>.zip`), *not* from
  locally compiling with `mx_nc`. Don't reach for the compiler — the vendor
  ships precompiled `.dfp`s and that is what the config points at.

## Architecture

Three layers, cleanly separated. Keep it that way.

- **`Catalog.{h,cpp}`** — pure data, no GTK. The `Accel` enum
  (Hailo/DeepX/Axelera/MemryX, order fixes every graph's series order) plus an
  INI reader for `config/models.conf`. Rows are **logical**: one `BenchSubject`
  holds a different artifact per vendor in `members[kAccelCount]`, so the same
  model can run on three cards at once and be compared. Two distinct notions,
  don't conflate them: `configured(a)` = the config names an artifact for that
  card; `runnable(a)` = it is also on disk. The UI greys on the first and shows
  "(pending)" on the second. To add a model, add a `[model:*]` section — no code
  change, no rebuild.
- **`Bench.{h,cpp}`** — the engine, no GTK. One `Worker` thread per card, each
  owning a `BenchRunner` for its whole life (several SDKs keep thread-affine
  state — **create, use and destroy a runner on one thread**). The hot path is
  lock-free: a worker only bumps `std::atomic<uint64_t> frames`, and the GUI
  derives fps from that counter's delta on its 1 Hz tick. Nothing in a worker
  ever waits on the UI, so a slow model load or a wedged device cannot freeze the
  window. `BenchEngine`'s ctor **and** dtor are defined out-of-line in the .cpp
  because `Worker` is incomplete in the header.
- **`bench_<backend>.cpp`** — one `BenchRunner` per SDK (all four now), each
  behind `MB_HAVE_<BACKEND>` and added to the target only when CMake found that
  SDK. Each takes an `ApiMode` and implements both sync and async — see
  **API modes** below. `bench_runners.cpp` holds the single `make_runner()`
  dispatch plus the `api_mode_is_native()` / `api_mode_note()` table; adding a
  backend means a new file plus two lines there.
- **`ControlPanel.{h,cpp}`** — the left panel: a `Gtk::Notebook` of two
  single-selection `Gtk::ListBox`es (Models / Pipelines), the frame-rate controls,
  the per-accelerator checkboxes, Start/Stop, status. `selection()` turns the
  current row plus the ticked cards into `std::vector<BenchItem>`.
- **`MainWindow.{h,cpp}`** — `Gtk::Paned`: controls left, a scrolling column of
  five `Gtk::Expander` graph sections right. Owns the 1 Hz tick.

## Invariants worth keeping

- **One benchmark subject per card at a time.** Two models sharing a card split
  its throughput *and* its watts, so per-model fps/W and mJ/frame would be
  meaningless. This is why the lists are single-selection and the three benchmark
  graphs are fixed at exactly `kAccelCount` series.
- **Fixed per-accelerator series.** `fps_`/`eff_`/`energy_` always have 4 series
  in `Accel` order, regardless of what's running; `on_tick` folds the engine's
  variable-length `series()` onto them and leaves the rest at 0. An idle card
  really is doing 0 fps — don't special-case it to a gap.
- **Color means one card, everywhere.** `accel_color_[]` is looked up from
  `device_palette_` by matching `accel_name()` against the Probes `device_name`,
  so a card's frame-rate trace and its power trace are the same color. Colors
  come only from the brand palette in `util.h` — no ad-hoc RGB.
- **Efficiency needs real watts.** `Probes::power_for_device()` returns NaN for a
  card with no sensor (DeepX here) and the engine leaves both efficiency figures
  at 0. **Never estimate watts** to fill the gap — a fabricated number sitting
  next to genuine INA228 readings is worse than a zero.
- **fps/W and mJ/frame are both raw per-sample values and therefore exact
  reciprocals** (`p / fps` vs `fps / p`) — the same measurement in two units.
  This replaced a cumulative run-average energy figure; the integration state
  (`Series::energy_j`) is gone, so don't reintroduce a running total without
  being asked. Energy's section is **collapsed by default**, since Efficiency
  already carries the information.
- **Never reset the benchmark graphs on start/stop.** This was the opposite
  earlier — Start used to call `reset()` on all three. The user wants one
  continuous record so successive runs sit on the same axis and can be compared
  directly. Consequence: `BenchEngine::stop()` must zero `joules_per_frame` as
  well as `fps`/`fps_per_w`, or an idle app keeps drawing the last run's energy
  figure as a flat line that reads like a live measurement.
- **`Series::joules_per_frame` is SI joules; the UI multiplies by 1000.** Keep
  the engine in SI and do the unit choice at the display edge. It is also the
  **only lower-is-better series** on screen — don't fold it into a shared
  "higher is better" assumption if summary/sorting is ever added.
- **Graph order is Power, Temperature, Frame Rate, Efficiency, Energy.** The two
  telemetry graphs lead because they are live with no run in progress.
- **`make_section(title, content, expanded)`** seeds `vexpand` from `expanded`
  *and* binds it to the property signal. Both halves matter: seeding alone was
  the original bug (a section that started collapsed could never grow), and
  binding alone would start a collapsed section claiming space it isn't using.
- **The three benchmark legends are one row of up to four cards** (wrapping at
  four via `i % 4, i / 4`) and carry no explanatory text. Failures and loading
  state therefore go to the status line in `on_tick` — do not let a card sit
  silently at 0, which is how a broken model looks exactly like a slow one.

## API modes (Sync / Async)

The control panel switches every backend between a vendor's blocking call and
its async/streaming one. **The four SDKs are not symmetric** — this was verified
against the installed headers and exported symbols, not assumed:

| | Sync | Async |
| --- | --- | --- |
| Hailo | `InferVStreams::infer()` | `InferModel` → `run_async()` → `AsyncInferJob` (`hailo/infer_model.hpp`) |
| DeepX | `InferenceEngine::Run()` | `RunAsync()` → jobId → `Wait()` |
| Axelera | `axr_run_model_instance()` | **none exists** |
| MemryX | **none exists** | `connect_stream(in_cb, out_cb)` + `start`/`stop` |

So in each mode exactly one vendor is emulated, and the **status line** says
which, via `api_mode_note()` (it was the frame-rate legend until that legend's
text was dropped — if the status line is ever reworked, this must land
somewhere). **Do not quietly present an emulated mode as equivalent** — that
tag is the whole reason the toggle is honest. **Async is the default mode.**

- **Axelera really has no async API.** 38 `axr_*` entry points, exactly one is
  inference, and it blocks. The `zeCommandQueue*` symbols in `libaxruntime.so`
  are Level Zero internals (the runtime is built on oneAPI L0), not public API.
  "Async" there sets the runtime's own `double_buffer=1` property — worth only
  ~7–18%. Don't "improve" this by fanning out host threads; that would be
  fabricating a number the API does not offer.
- **MemryX really has no blocking call.** Its whole public surface is ctor,
  `connect_stream`, `start`, `wait`, `stop`, `set_num_workers`,
  `get_num_streams`. Sync is emulated by permitting one frame in flight.
- **`run_frame()` must retire exactly one frame in both modes.** An async runner
  tops its pipeline up and *then* blocks for one completion, so the engine's
  frame counter needs no special case. Keep that contract.
- Async depth is 4 (DeepX, MemryX) or HailoRT's reported queue size capped at 8.

## Measurement choices (don't quietly change these)

The runners deliberately keep host work out of the timed loop, so the number is
the accelerator, not this CPU:

- **Hailo, DMA buffers** — every buffer handed to HailoRT is **page-aligned and
  page-rounded** (`PageBuffer` in `bench_hailo.cpp`, `posix_memalign` + size
  rounded up to `_SC_PAGESIZE`). Not cosmetic: an unaligned address makes the
  runtime warn about `read_async()` and take a bounce copy, and `infer_model.hpp`
  warns that an *output* buffer whose allocation is not a whole number of pages
  risks "memory corruption at the end of the last page". Don't swap these back
  for plain `std::vector`.
- **Hailo, async callback lifetime** — `run_async` callbacks fire on HailoRT's
  worker threads and touch `Stage`. Jobs are detached, so at teardown there can
  still be work in flight; `Stage::drain()` waits for `in_flight == 0` and then
  resets `configured` *before* the slot bookkeeping is destroyed. Without it the
  callback writes into a freed `free_slots` vector. That presents as an
  intermittent `corrupted double-linked list` abort, only on models slow enough
  to still have jobs outstanding at exit (YOLOv8s reproduced it every time;
  ResNet-50 at 1370 fps drained fast enough to look fine). ASAN finds it
  instantly — reach for `-fsanitize=address` on any async-lifetime suspicion.
- **Hailo** — output vstreams use `HAILO_FORMAT_TYPE_AUTO`, *not* FLOAT32.
  FLOAT32 makes HailoRT dequantize on the host inside the call. (Worth knowing:
  the `mb-hailo` skill claims bypassing that dequant needs a different API
  surface entirely — it does not. `AUTO` on the plain `InferVStreams` path is
  enough.) VDevice uses the ROUND_ROBIN scheduler so a multi-stage pipeline
  keeps several network groups configured without per-frame activate.
- **DeepX** — `Run()`/`Wait()` return engine-owned buffers; nothing is copied out.
- **Axelera** — the input is quantized+padded once at load (it's synthetic data,
  so re-quantizing an identical buffer every frame would time our own loop), and
  outputs are left in raw padded device layout — no unpad/dequant.
- **MemryX** — outputs are not read back with `get_data()`; the input float
  buffer is filled once on the first callback, not per frame.
- Input is a fixed xorshift pattern (`bench_input.h`) — plausible bytes,
  identical on every run and every card.
- **Compare within a mode, never across.** A sync figure and an async figure
  answer different questions; the README states this and the UI labels the run.

## Build / verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mb-benchmark                      # needs a display
```

Every accelerator SDK is optional; configure prints which were found.

**Axelera's RISC-V toolchain is put on `PATH` by the app itself** —
`prepare_backend_environment()` (called from the `MainWindow` ctor, on the main
thread, *before* any worker exists because it uses `setenv`). It scans
`/opt/axelera/*/bin` for `riscv64-unknown-elf-gcc`; `$MB_AXELERA_TOOLCHAIN`
overrides. Headless drivers must call it too, or every Axelera model fails.

Without the compiler the runtime reports only
`Failed to create executor for model: kernel_function` — it names the model, not
the missing toolchain, which sends you debugging the wrong thing. The runner now
appends the real cause to that message. This bit a user who launched the GUI
normally; don't regress it back to "export PATH first".

No test suite. **Everything except the widgets is GTK-free and testable
headlessly** — prefer this over driving the GUI, which belongs to the user:

- **Runners** — `Catalog.cpp Bench.cpp bench_runners.cpp bench_*.cpp`; a ~50-line
  main that calls `Catalog::discover()`, `make_runner(accel, mode)`, `load()`
  and loops `run_frame()` prints fps per card, per mode.
- **Downloads** — `Catalog.cpp Fetcher.cpp`; drive `Fetcher::start(catalog)` and
  poll `snapshot()` to verify a new `[vendor:*]` source end to end.
- **Telemetry** — `Probes.cpp` alone.

Set `MB_BENCH_MODELS_CONFIG`, `MB_BENCH_MODELS_ROOT` **and `MB_INA228_CONFIG`**
when the driver binary lives outside `build/` — every config path resolves
relative to the executable, so a scratch binary otherwise finds no config and
silently reports an empty catalog and unmapped `INA228#<n>` rails (which then
read as "this card has no power sensor").

**Discard the first `BenchEngine::sample()` after starting a run.** `sample()`
divides the frame-counter delta by the `dt` you pass, and `last_frames` starts
at zero — so if a driver lets the run settle for N seconds and *then* takes its
first sample with `dt = 1 s`, that sample reports N× the true rate and poisons
any average built from it. This produced a bogus "we beat `hailortcli` by 24 %"
before `hailortcli` was run on the same host and showed parity. The GUI is
immune because it samples continuously from before the run starts.

**Vendor tools available on this host** for cross-checking, and worth using —
they are the ground truth this app is validated against. **Run them from a
scratch directory, not the repo root:** `dxbenchmark` writes
`DXBENCHMARK_<timestamp>.{csv,html,json}` plus a ~15 MB `profiler.json` into the
current directory, which otherwise litters the working tree (`.gitignore` has a
backstop, but the files still land there).
`hailortcli benchmark <hef>`, `mx_bench -v -d <dfp> -f <frames>`,
`dxbenchmark --dir <dir> --warmup 10 --time 30` (prints a profiler table rather
than an FPS line; divide the M1's core count by "NPU Core" average). See the
comparison table in the README.

### Screenshotting this app (X11)

**GTK4 throttles rendering when the window isn't focused/visible**, so `xwd` on a
background-launched window returns a *stale first frame* — it will look like the
app is showing no data when it is in fact ticking fine. This cost real debugging
time; don't repeat it. Raise and focus the window first (`XRaiseWindow` +
`XSetInputFocus` via `ctypes` on `libX11` works; `xdotool` is not installed
here), then `xwd -id <wid> -out w.xwd && ffmpeg -y -i w.xwd w.png`. `libXtst`'s
`XTestFakeButtonEvent` after an `XWarpPointer` will click Start for you. If
values look frozen, verify with a temporary `g_message` in `on_tick` before
believing the picture.

Kill instances with `pkill -x mb-benchmark` (exact name) — `pkill -f
build/mb-benchmark` also matches your own shell command.

## Known issues / bugs already paid for

Every one of these cost real debugging time. If a symptom here reappears, start
from the cause rather than re-deriving it.

### Open

None currently known. (The long-standing `SIGSEGV`-on-exit is diagnosed and
fixed — see the first row below. If it recurs, it is a *different* bug.)

### Fixed — recorded so they are recognised, not re-debugged

| Symptom | Real cause | Guard |
|---|---|---|
| `Gtk-CRITICAL … gtk_list_box_get_selected_row: assertion 'GTK_IS_LIST_BOX (box)' failed` then **`SIGSEGV` on exit** | Widget members are destroyed in **reverse declaration order**: `model_list_`/`pipeline_list_` are declared *after* `notebook_`, so they die first; destroying `notebook_` then emits `switch_page`, and that handler calls `current_subject()` → `get_selected_row()` on a dead `ListBox`. This is the exit crash that was "open, undiagnosed" for most of the project | `~ControlPanel` disconnects every stored `sigc::connection` (destructor body = the last moment all widgets are alive), and `~MainWindow` disconnects the 1 Hz tick first. **Any new widget-signal handler that reads another widget must be added to `conns_`** |
| `BrokenPipeError: [Errno 32] Broken pipe` traceback on exit | Cosmetic, and not the app: the MemryX power helper subprocess writes to a pipe the parent has closed; Python printed the traceback to the stderr it inherited from us | Helper calls `os._exit(0)` on write failure — skips the traceback *and* the interpreter's stdout flush at shutdown (a plain `break`/`sys.exit` would still emit "Exception ignored"). **`Probes.cpp` is shared with `mb-powermon-gui` — port this there too** |
| `corrupted double-linked list`, Async mode, intermittent | HailoRT completion callback (its worker thread) writing into `Stage::free_slots` after the Stage was destroyed. Jobs are `detach()`ed, so work is still in flight at teardown; and member order destroyed the free-list *before* `ConfiguredInferModel`, so callbacks could still fire | `Stage::drain()` — wait `in_flight == 0`, then reset `configured`, before the rest dies. **Reproduced only on YOLOv8s**; ResNet-50 at 1370 fps drained in time and looked clean, which made a race look model-specific. ASAN named it in one run |
| `[HailoRT] [warning] read_async() … unaligned buffer` | `std::vector` is neither page-aligned nor page-rounded. Also a correctness risk: `infer_model.hpp` warns an under-rounded *output* allocation can corrupt past the last page | `PageBuffer` (`posix_memalign` + size rounded to `_SC_PAGESIZE`). Don't revert to `std::vector` |
| `Failed to create executor for model: kernel_function`, every Axelera model | `riscv64-unknown-elf-gcc` not on `PATH`; the runtime JIT-compiles each model's `kernel_function.c` at instantiation. The message names the *model*, so it reads like a bad artifact | `prepare_backend_environment()` scans `/opt/axelera/*/bin`. Model-independent — verified failing on ResNet-50 *and* YOLOv8s |
| Every Axelera artifact silently skipped by the downloader | `have_tool()` tested the **exit status** of `unzip --version`, which is not a valid invocation — unzip prints usage and exits non-zero | Test whether the binary can be *started* (`posix_spawnp` != -1), not what it returns |
| A graph section that would not grow when expanded | `set_vexpand(expanded)` sampled once at construction | Bind `vexpand` to the `expanded` property signal |
| Accelerator checkbox stuck off after changing model | Unticking on "no build for this card" also discarded the user's intent | `accel_wanted_[]` holds intent; effective state = intent && configured && compiled-in |
| Link errors on every `MxAccl` symbol | Linked `libmemx` (the **driver** library) instead of `libmx_accl` (the runtime, from `memx-accl`) | CMake probes `mx_accl` |
| `invalid application of sizeof to incomplete type` on `BenchEngine` | `std::vector<std::unique_ptr<Worker>>` with `Worker` incomplete in the header | Declare **both** ctor and dtor out-of-line in `Bench.cpp` |
| GUI appears frozen / shows no telemetry in a screenshot | **Not a bug.** GTK4 throttles rendering for an unfocused window, so `xwd` returns a stale first frame | Raise + focus before capture; verify with a temporary `g_message` in `on_tick` before believing a screenshot |
| DeepX "only 32 fps" on YOLOv8s | **Not a bug.** Sync-mode artifact; 4.6× in async | Always state the API mode with a number |

## Host specifics (this machine)

All four cards are present and **all four** inference SDKs build: Hailo-8
`0000:01:00.0`, MemryX MX3 `0000:47:00.0`, DeepX M1 `0000:c1:00.0`, Axelera Metis
`0000:c6:00.0`.

**MemryX is not telemetry-only** — an earlier version of this file said so, and
it was wrong. `mb-powermon-gui`'s note that the MemryX *power-telemetry* classes
are Python-only is true and unrelated; the **MxAccl inference runtime does ship a
linkable C++ library** (`memx-accl`, headers `/usr/include/memx/accl/`). Link
`libmx_accl`, **not** `libmemx` — the latter is the driver library and has none
of the `MxAccl` symbols.

Two INA228 shunts are mapped (Hailo and Axelera), so those two cards have real
efficiency figures and DeepX does not. Running `mb-powermon`/`mb-powermon-gui`
at the same time fights over the Hailo power DVM (`DVM_ALREADY_IN_USE`).

Rough numbers at max speed, for sanity-checking a change:

| | Hailo sync | Hailo async | DeepX sync | DeepX async | Axelera sync | Axelera async |
| --- | --- | --- | --- | --- | --- | --- |
| YOLOv8s | ~123 | ~490 | ~32 | ~148 | ~131 | ~154 |
| YOLOv8m | ~61 | ~67 | | | | |
| ResNet-50 | ~297 | ~1369 | ~393 | ~1087 | ~352 | ~376 |

**Async is not a uniform multiplier.** YOLOv8m gains ~10% on Hailo where YOLOv8s
gains 4× — the medium model is NPU-compute-bound, so there is little transfer
latency left to hide. A small async gain is a property of the workload, not a
sign the async path is broken.

The People Tracking pipeline roughly halves the YOLOv8s figures. Note DeepX
*leads* on ResNet-50 sync while trailing badly on YOLOv8s sync — its weak
YOLOv8s number is a mode artifact (4.6× in async), not a general handicap, so
don't "fix" it by special-casing the DeepX runner.

### Driving the GUI

**Ask before launching or killing the app.** It runs on the user's own display;
they will be using it. `pkill`-ing it out from under them mid-test has happened
and is not acceptable. Prefer the headless drivers below — they need no display
and no GUI instance, only the device to be free.
