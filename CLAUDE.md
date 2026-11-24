# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

`mb-benchmark-gui` is a C++17 / **gtkmm-4.0** desktop GUI that **benchmarks**
edge-AI NPUs — it links each vendor's runtime and drives inference itself, then
charts the achieved frame rate alongside the power it cost.

Five backends: four M.2 cards (Hailo-8, MemryX MX3, DeepX M1, Axelera Metis) and
one **SoC-integrated** NPU (the Qualcomm Hexagon NSP on a Dragonwing IQ-9075).
Every one is optional and auto-detected, and it builds on **x86_64 and aarch64**
— see **Host specifics**, which describes two quite different machines.

It was built from `../mb-powermon-gui` and shares its chrome (teal
`Gtk::HeaderBar`, brand palette, `Gtk::Expander` graph sections) and three files
with it. They are the *same code*, not a fork with intent to diverge — fix a bug
in both — but they are shared to **two different degrees**, and the difference
matters when porting:

- **`GraphArea.{h,cpp}` and `util.h` are byte-identical.** `cmp` them after any
  change; a `cp` in either direction is the correct way to port. Everything
  added here (`RangeMode`, per-series visibility, the adaptive time-label step)
  went into both, even where only one app exposes a control for it.
- **`Probes.{h,cpp}` has diverged** — 1740 lines here against 1619 there. Four
  deliberate edits, none of them portable as a whole file: the INA228
  config-path search (see **Config**); `Probes::power_for_device()`, which needs
  `Catalog.h` and **cannot compile** in the sibling; the whole frequency family;
  and `set_note_sink()`, whose only consumer is our `Logger`. **Never `cp` this
  one**; port hunk by hunk. Doing otherwise breaks that build, which has already
  happened once.

  Things that *should* travel both ways already have: the `plausible_temp()`
  gate, the helper's `os._exit(0)`, (2026-09-03) the whole INA228 accumulator
  family — `DIETEMP`/`ENERGY`/`CHARGE`, `alias_device_order()` and the
  temperature fold — and (2026-09-07 … 09-12) `VBUS`, `CURRENT` with its
  per-rail `invert` flag, the latched undervoltage detector, and
  `plausible_power()`. Those ports were clean precisely because `struct Ina228`,
  `INA228Probe`, `power_device_order()` and `pcie_merge_target()` were still
  **byte-identical** between the two files; check that with `diff` on the block
  before assuming any future INA228 change can be transplanted the same way.

  **Two traps, both paid for during those ports.** A `str.replace`-style edit
  **silently no-ops** when the sibling's signature differs — its `emit`/`fill`
  helpers take no `order` argument — so two hunks "landed" and the voltage
  family came back empty; assert on the replacement count, never on the exit
  status. And a lifted block extracted by searching for
  `root->append(make_section(` mangled the sibling's `MainWindow.cpp` while
  **still compiling**, because the braces happened to balance. After any
  cross-project transplant, print the section order and `diff` against
  `git HEAD` rather than trusting the build.

The model/pipeline catalog is derived from `../envic_ai_cpp`'s
`ai_common/pipeline_registry.cpp` — same models, same detector+recognizer
pairings, **minus the DeGirum widerface YOLOv8n face detectors** (deliberate;
don't add them back without being asked). It now lives in `config/models.conf`
rather than in code; see **Config**.

Window title: **"NPU Benchmarking GUI"** (`kTitle` in `MainWindow.cpp`). Binary:
`./build/mb-benchmark`. The **launcher label is different and deliberately so** —
`Name=mb-benchmark` in the `.desktop`, because the descriptive title wraps to two
lines under a desktop icon. Don't "fix" the mismatch by shortening the window
title or lengthening the launcher label.

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
`axelera/metis`, `deepx/dx-m1`, `memryx/mx3`, `qualcomm/iq9075`), overridable with
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

- **`qnnruntime.{h,cpp}`** — **vendored, not written here.** A flat C-ABI shim
  over the QNN C API, copied from
  `../envic_ai_aerex/envic/src/components/accelerator/qnn/`. The `.cpp` is kept
  **byte-identical** to that copy so fixes can move either way with `cp`; `cmp`
  it after touching anything. The `.h` differs *only* in its header comment
  (the original describes a Vala caller). QNN is never linked — the shim
  `dlopen`s a backend library and asks it for an interface struct, which is why
  CMake needs only headers and `libdl`.
- **A card the build has no backend for — or, since 2026-11-16, whose hardware
  is not on the PCI bus — is hidden completely.** `accel_present()`
  in `Catalog.h` is the single question every UI site asks: no Inference
  checkbox, no Accelerators row, no legend entry, no
  trace, and no mention on the model rows' card line.

  **This reverses the original design**, which showed every card greyed out with
  its reason — so don't "restore" it without being asked. Two things drove the
  change: on a host that builds one or two backends (the IQ-9075 builds Hailo
  and Qualcomm only) the greyed majority was most of the panel; and greying
  carried two unrelated meanings at once — "no SDK in this build" and "this
  model has no artifact for that card" looked identical. Greying now means only
  the second, which is why `refresh_accel_sensitivity()` no longer has an
  "SDK not found" tooltip branch.

  Three implementation notes that are easy to get wrong:
  - **`accel_present()` = compiled in AND enumerated on the PCI bus**
    (2026-11-16). It was build-time only until then, on the argument that
    detecting hardware needed each vendor's enumeration API on the GUI thread,
    and that a false negative would hide a working card. That cost a real
    problem: the Strix Halo has every SDK installed and only a Hailo-8 fitted,
    so it showed four cards, and a run with the absent three ticked sat in
    `starting` for its whole length and took ~20 s to Stop while MemryX's
    connect timed out. The check is now **pure sysfs**: any
    `/sys/bus/pci/devices/*/vendor` equal to the card's id — `0x1e60` Hailo,
    `0x1ff4` DeepX, `0x1fe9` MemryX, `0x1f9d` Axelera, the same ids the
    telemetry probes and `npu-status.sh` use. No vendor API, no device open,
    nothing that can block on a wedged card.
    **It tests enumeration, deliberately NOT driver binding**: a card on the bus
    with no driver (the MemryX D3cold state) is still shown and fails its load
    with a clear error. Hiding is reserved for "not physically there", which is
    the false-negative the original rule was guarding against. Qualcomm is part
    of the SoC and stays build-time. Scanned once per card per process (the
    panel is built once; a hot-plugged card could not appear anyway).
    `$MB_BENCH_ALL_ACCELS=1` restores the build-time rule. `accel_hidden_reason()`
    says which test failed, and the startup note logs it
    (`accelerators shown: Hailo · hidden: Axelera (compiled in, but no such PCI
    device), …` — one note, no `;`, because the plotter splits the startup note
    on `;` and reads any `<Card>: …` part as a probe line). Extend there, not at
    the call sites.
  - **Series counts do not change.** `fps_`/`eff_`/`energy_` still have
    `kAccelCount` series in `Accel` order; a hidden card keeps its slot and is
    hidden with `set_series_visible(i, false)`. The engine still folds onto
    fixed slots, and `GraphArea::push()` still no-ops on a size mismatch.
  - **A null widget pointer now means two opposite things.** In
    `graph_accel_mask()` a hidden card must clear its bit, while a *present*
    card is briefly null during construction and must default to visible —
    hence the explicit `accel_present()` test rather than a bare null check.
    The legend and checkbox rows track a separate `col` counter so hiding a
    card leaves no gap in the row.
- **`Catalog.{h,cpp}`** — pure data, no GTK. The `Accel` enum
  (**Axelera/DeepX/Hailo/MemryX/Qualcomm — alphabetical since 2026-10-31**,
  and the order fixes every graph's series order)
  plus an INI reader for `config/models.conf`. **Append to that enum, never
  insert** — `accel_index()`/`accel_at()` are raw casts, so a card's position is
  what indexes `watts[]`, `Catalog::vendors_[]` and every graph series.
  **It HAS been reordered once, deliberately** (2026-10-31, to alphabetical
  from the write-order `Hailo, MemryX, DeepX, Axelera`). That was safe only
  because nothing in the tree stores a position: the name and vendor tables are
  switches, `watts[]` is filled through `accel_at(i)`/`accel_name()` so it
  follows whatever the enum says, every `[kAccelCount]` initialiser is uniform,
  and the INA228 fold matches on *name*. **Three things had to move together** —
  the enum, `Probes::discover()`'s call order (the telemetry legends group by
  it) and `ACCEL_ORDER` in `tools/csv-to-html-plot.py`. Checked by grep for
  literal indices before the edit, not assumed. The rule still stands for an
  *insert*, and a reorder is still a whole-project change, not a one-liner.

  **One consequence, harmless but worth knowing:** CSV *column order* changes,
  while the column *names* do not. Logs from before and after are no longer
  byte-diffable, but every reader parses by name, so old logs keep plotting.

  Rows are **logical**: one `BenchSubject`
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
- **Per-card settings** reach a runner through `BenchRunner::configure(BenchItem&)`,
  called just before `load()`. `BenchItem` carries `cores` (Axelera), `freq_mhz`
  (MemryX) and `nsps` / `perf_mode` / `qnn_backend` (Qualcomm); defaults mean
  "leave the device alone", except `perf_mode`, which deliberately states
  `burst` rather than inheriting whatever the governor is doing. Adding a knob
  is a field plus a `configure()` override, not a signature change everywhere.
- **`bench_<backend>.cpp`** — one `BenchRunner` per SDK (all five now), each
  behind `MB_HAVE_<BACKEND>` and added to the target only when CMake found that
  SDK. Each takes an `ApiMode` and implements both sync and async — see
  **API modes** below. `bench_runners.cpp` holds the single `make_runner()`
  dispatch plus the `api_mode_is_native()` / `api_mode_note()` table; adding a
  backend means a new file plus two lines there.

  **`make_runner()`'s switch has a `default:`, so a missing case does not
  warn** — it silently returns nullptr and the UI reports "SDK not found at
  build time". The three `Accel` switches in `Catalog.cpp` and
  `api_mode_is_native()` are exhaustive and *will* be caught by `-Wswitch`;
  this one won't. Check it by hand when adding a card.
- **`Logger.{h,cpp}`** — CSV logging, no GTK. **The only file-writing code in
  the app**, so it carries the filesystem discipline the rest of the codebase
  never needed: `std::error_code` throughout, never the throwing `fs::`
  overloads, and it degrades to a no-op rather than throwing out of the tick
  (which runs on the GTK main loop). Always on; `MainWindow` opens it after
  `Probes::discover()` and writes one row at the end of every tick. Schema and
  the rules that make the file useful are under **Invariants**.
- **`ControlPanel.{h,cpp}`** — the left panel: a `Gtk::Notebook` of two
  single-selection `Gtk::ListBox`es (Models / Pipelines), then four frames —
  **Inference** (frame rate), **Accelerators**, **Graphs**, **Telemetry** — and
  Start/Stop with the status line. `selection()` turns the current row plus the
  ticked cards into `std::vector<BenchItem>`.

  **Accelerators is one row per card: name, `Enabled`, then that card's own
  controls** (API mode, depth, and whatever else that vendor exposes).
  **This replaced a `Gtk::Notebook` of one tab per card** (2026-09-18). The tabs
  showed only the selected card's settings, so two cards' configurations could
  never be read against each other and a run could start with a card set up in a
  way nobody had looked at. Don't reintroduce them.

  Three things about that section are deliberate:
  - **`Enabled` is ONE switch for two jobs**: it is what `selection()` targets
    for a run *and* what `graph_accel_mask()` draws. There were two per-card
    checkbox sets until 2026-09-18 — this one under Inference and a trace filter
    under Graphs — and with both headed "Accelerators" they could not be told
    apart. The Graphs row is gone. **What that cost is deliberate and should not
    be rebuilt casually**: a card can no longer be benchmarked with its traces
    hidden, which was useful when one card's watts dwarfed another's on a shared
    axis.
  - **`graph_accel_mask()` reads `accel_wanted_`, the user's INTENT, not the
    checkbox's live state**, and that distinction is load-bearing.
    `refresh_accel_sensitivity()` unticks a card whenever the selected model has
    no artifact for it, so reading the widget would blank four cards'
    temperature and power traces — and the history already on screen, the filter
    being retroactive — merely because someone picked a Hailo-only model. A card
    with no build for this model is *idle*, not absent, and its telemetry is
    exactly what you would want to keep watching. The toggle handler therefore
    returns early while `syncing_accels_` is set: only a click is intent.
  - **A card's controls sit in a `Gtk::FlowBox`, not a Box.** Qualcomm carries
    six controls including two combos and does not fit the 680 px panel on one
    line; the FlowBox reflows to whatever width the pane has. Hard-coding which
    controls wrap would break the moment a knob is added.
  - **The name and `Enabled` are `valign = START`**, so they stay put when a
    wide card's controls wrap onto a second line.
- **`Automation.{h,cpp}`** — unattended schedules, `--automation <file>`. Pure
  data, no GTK, like `Catalog`/`Probes`/`Logger`, and it deliberately does **not**
  resolve ids against the catalog — that is `MainWindow`'s job, and keeping it
  out means a plan parses and validates with no models tree present.

  **This replaced a set of control-panel checkboxes** (Unattended / Sweep Models
  / Sweep Pipelines / two durations), and the reversal was deliberate: checkboxes
  could only say "every model" or "every pipeline", which is the least
  interesting thing to want, and they left **no trace in the log** — a finished
  CSV could not be read back to say what had been configured. A file is
  explicit, ordered, repeatable and reviewable. Don't reintroduce them.

  The format is line-oriented rather than sectioned INI, because the step list is
  **ordered** and INI sections are not: `initial`/`benchmark`/`idle` (minutes,
  fractions allowed), `loop`, accelerator defaults, then repeated
  `model =`/`pipeline =` lines. `[headers]` are accepted and ignored so a file
  can still be laid out readably. Steps name the **catalog id** (`resnet50`) and
  never the display name — ids carry no spaces, so nothing needs quoting.
  Shipped plans: `config/automation-{models,pipelines,test}.conf`.

  **`start` is deliberately separate from `idle`.** The beginning of a session
  wants a longer settle — cards just powered, app just launched, `Fetcher` still
  landing artifacts — and folding that into `idle` would pay it between every
  run. **`end` is the mirror**: hold that long after the last step, then
  `close()` the window. Default is < 0, meaning stay open, so a plan that says
  nothing behaves as before; it is only reachable with `loop = false`.

  **`tick_automation_end()` will not close while `stopping()`.** Quitting with a
  worker still inside a vendor SDK call is precisely what leaves an MX3 session
  unreclaimed and makes the next launch block in `memx_fops_open` before
  `main()` — the hang under **Known issues**. After `kAutoStallSeconds` it logs
  that it is *not* closing and stays open. It also stands down entirely if the
  user starts a run after the plan finished: the app is theirs again. The `end`
  delay is deliberately spent still logging, so the cards' cool-down to idle is
  on record.

  **Every per-card control is settable per step**, written `<vendor>.<knob>`
  (`hailo.api`, `axelera.cores`, `qualcomm.perf`), plus an `accelerators =`
  whitelist.

  **`fps = <n>` | `max` caps the frame rate from a plan** (2026-11-23). It is
  the one honest way to vary a card's RATE without changing anything else:
  same model, same artifact, same layer mix. Varying load by
  swapping models confounds it with where on the die the heat appears. Like the
  fan it is **not per-card** — `BenchEngine::start()` takes one cap for the
  whole run — so it sits beside `fan` in `AutomationSettings` and drives the
  Frame Rate widgets through `apply_automation_settings()` like everything else.
  - **Only honest where the producer is `run_frame()`** — Hailo and DeepX. On
    the other three the SDK's threads free-run and the cap bit only on
    *counting* until `FramePacer` landed; see **the frame-rate cap was cosmetic
    on three backends** below. Confirm the rail moved before trusting a capped
    figure.
  - **IT IS NOT A LOAD KNOB ON HAILO-8 — this entry said it was, and the
    measurement refuted it (2026-11-23/24).** The claim was that capping the
    rate varies power; on a dataflow part it barely does. ResNet-50 at 73 %
    duty, temperature-corrected to a common 52 °C:

    | cap | fps | rail W | mJ/frame |
    | --- | ---: | ---: | ---: |
    | max | 1372.3 | 4.341 | 2.59 |
    | 1000 | 1000.0 | 4.339 | 3.55 |
    | 700 | 700.0 | 4.344 | 5.06 |
    | 500 | 500.0 | 4.325 | 7.08 |
    | 300 | 300.0 | 4.131 | 11.13 |
    | 150 | 150.0 | 3.247 | 22.5 |

    **500, 700, 1000 and max are the same wattage to 14 mW** — 2.7x the work
    for 0.3 % more power. Power saturates by 500 fps and the whole usable range
    is 3.25-4.34 W, 25 %, against the 6.9x that model identity gives. The
    mechanism: on a dataflow architecture the configured network group lights
    and clocks the fabric, so power is set by WHICH MODEL IS RESIDENT, not by
    how many frames pass through it. A plan header that estimated a W ladder by
    scaling linearly in frames (1000 -> 3.4 W, 500 -> 2.1, 150 -> 1.2) was
    wrong by up to **2.7x**. If you need a load axis on this card, swap models;
    if you need a *matched* load at low duty, the cap buys you 150 fps and
    nothing between there and full rate.
  - **UNDER A CAP, THE FRAME RATE CANNOT DETECT THROTTLING — only `_CLK` can.**
    The exact inverse of the DX-M1 rule above, and for a reason that
    generalises: a cap below what the card does when throttled makes the rate
    blind. Measured on three treads at 20.6 % duty — 350 MHz for 63-67 % of
    their samples, die at **104 °C**, all three clamped to 4.73 W — while
    `hailo_fps` read exactly **500.0 / 700.0 / 300.0** the whole way. So
    "confirm the rail moved" is necessary and not sufficient: on a capped
    Hailo run, check `_CLK` per tread or the wattage is not a measure of work.
  - **The spin clamps at `kMaxFps` = 1000, silently.** The Hailo does 1372, so
    full rate is `fps = max` and never a number above 1000. The effective cap is
    in the CSV every tick as `bench_target_fps` (empty = max) — read that, not
    the plan, exactly as you read `<port>_FAN_DUTY` and not the requested duty.

  **The RA6M5 fan is settable too, and it is the one plan key that is not
  per-card** (2026-11-19) — one fan cools whatever is in front of it, so it
  sits beside `benchmark`/`idle` rather than under a `<vendor>.` prefix.
  `fan = <pct>` is the duty held from the start of a run; `fan.start` /
  `fan.step` / `fan.every` add a ramp that takes over `fan.after` into the run
  and stops at `fan.max`. **Intervals are minutes, like every other duration
  in a plan** — mixing seconds in would have been the obvious units bug.
  `FanPlan::at(t)` computes the duty from `t` rather than accumulating, so a
  ramp cannot drift and a missed tick cannot lose a step. Four things are
  deliberate:
  - **A plan that says nothing about the fan does not touch it.** The firmware
    keeps its duty across host disconnects, so a default of 0 would stop a fan
    the user had running. `FanPlan::states_anything()` is that gate.
  - **It drives the panel's spin, not `Probes` directly**, the same reason
    every other setting goes through the widgets: one path to the hardware, and
    the Telemetry row keeps showing what the board was last told.
  - **It is applied at the start of a run and on change only** — one I2C write
    per step, not one per tick — and each change is logged with the time into
    the run, since the ramp appears nowhere else in the CSV.
  - **Duty is quantised to 1/63 (1.587 %) by the EMC2101**, so consecutive 1 %
    requests can land on the same level (`fan 6` and `fan 7` both apply 6.3 %,
    verified on hardware). **Read `<port>_FAN_DUTY`, the board's own readback,
    never the requested percent.** A plan asking for the fan on a host with no
    such board logs that once and carries on.

  **`fan.mode` hands the fan to the board's own control loop instead**
  (2026-11-22, **rewritten 2026-11-23 for firmware ≥ 0.7.0**) — `off` / `on`,
  with **`fan.thermocouple` (alias `fan.tc`) = `off` | `real` | `model`**
  choosing where the loop's one temperature comes from, and `fan.target` in °C
  **or `auto`**. **`fan.nn` is REFUSED at load**, not ignored, with a pointer to
  the new spelling: a plan that silently ran the wrong arm would be worse than
  one that fails. `fan.tc = on` is still accepted and means `real`. The loop runs on
  the RA6M5, so it keeps cooling the card when this app exits or crashes; see
  **The RA6M5 probe board** below and `firmware/mb-probe-ra6m5/Control.h`.
  **`fan.input` is gone** — the setpoint is always the estimated on-die
  temperature, because the old die-vs-heatsink choice was the same control law
  with the observer moved between the setpoint and the error.
  - **A ramp and a loop are mutually exclusive, and a plan asking for both is
    REFUSED at load.** They would both drive the fan. `apply_fan_key()` catches
    them in one block and a post-load pass catches the case it cannot see,
    where the defaults supply one half and a step the other.
  - **`fan.idle` is the ONE fan key allowed beside `fan.mode`** (2026-11-23).
    It is the duty held whenever no loop is driving — the `start` pause, the
    idles between steps, the `end` hold — so it is the complement of the loop
    rather than a rival to it. **Without it, "turn the fan off, wait half an
    hour, then hand it to the board" cannot be written at all**: a bare `fan` is
    refused alongside `fan.mode` (correctly), and `apply_automation_fan()` only
    ever runs inside `if (active)`, so no existing key reaches the start pause.
    `apply_automation_idle_fan()` hands the register back from the loop before
    asking for a duty, since the firmware refuses one while the loop is on.
    **What it turned out to BE worth, 2026-11-23: an ambient probe.** Repeated
    before, between and after the arms it holds the card at a fixed ~0.86 W
    with no forced air, so differencing the brackets separates a plant change
    from a controller difference — which that run needed, because the plant
    moved. Give it the full `idle`/`end` duration rather than trimming it: the
    closing bracket of that run had **not settled after 30 minutes**, and
    learning that is what stopped a 0.016 °C result being reported as real.
    Read a bracket over its last **300 s**, not 600 — every one but the first
    follows a hot soak.
  - **It is sent once per step, not per tick.** Once the loop is running the
    board owns the duty; there is nothing to re-send, and `FanPlan::at()` is
    meaningless in that mode. The handover signature includes `tc`/`nn`, so a
    step that changes only the source still re-sends.
  - **`fan.target = auto` travels as NaN**, all the way to `ctl.target auto`.
    The host never asserts a temperature for a card it cannot identify, and
    `_CTL_AUTO` records which it was: "the card's measured bound" and "95
    degrees" stop meaning the same thing the moment another card is fitted.
  - **The status comes back as the `ctl.*` channels**, i.e. CSV columns
    `<port>_CTL_*`. `_CTL_DIE_EST` is the board's *model* of the die and the
    card's own `_TS0` is the truth, so any run scores the model for free.
    **Read `_CTL_SRC` first** — it says whether the loop was closed on a
    measurement (0), on an estimate (1), or ran open with no feedback (2), which
    is the only thing that confirms a step ran as configured.

  `config/automation-thermal-{hailo,deepx}.conf` are the plans this was built
  for: one continuous run, 30 min with the fan off, then 5 % rising 1 %/min to
  100 %. **The two schedules are deliberately identical**, so a difference
  between the cards' curves is the card and not the timing; the soak is set by
  whichever settles slower, which is the DX-M1 by about 6x. The headers show
  the working, measured from `logs/` — **the Hailo throttles at 180-240 s and
  plateaus at 105-108 C by 300 s, while the DX-M1 flattens at 89 C near
  1200 s.** Both plans have now been run end to end (2026-11-19); the soak
  margin question is settled — the DX-M1 was flat at 89.0 C for the last 5
  minutes of its 30 min. Two consequences worth keeping: the throttled Hailo
  plateau is an *oscillation* (fps cycling 925-1080), so a single sample of it
  means nothing; and the DX-M1 plateau is an oscillation too, for a reason the
  clock column hides — see below.

  **THE DX-M1 DOES THROTTLE, and `_C<n>` is the wrong instrument to see it
  with.** This file and the plan header both said it did not, on the strength
  of a 23.7-minute run whose fps never dropped. The 125-minute run disproves
  it: at the 89 C plateau the frame rate settles at **979 fps against 1083
  while warming** — a 9.6 % loss that persists for the whole soak and recovers
  completely once the fan arrives. What makes it easy to miss is the sampling:
  `dxrt-cli` reports an **instantaneous** clock, and it catches 1000 -> 800 MHz
  in only **9.9 %** of the samples in that window (130 of 1312, every one a
  lone 1 s blip at 88-89 C), while the frame rate — a full 1 s average —
  implies the card is at 800 MHz about **41 %** of the time. Restricting to
  samples that report a flat 1000 MHz on all three cores *still* shows **8.1 %**
  of the loss, so essentially all of it hides inside samples claiming full
  clock. The dither is sub-second and a 1 Hz point read cannot resolve it.
  **Use the frame rate, not the clock, to decide whether this card is
  throttling** — and never conclude "no throttle" from a run that stayed below
  the knee. It stops the moment the fan takes the die under ~88 C, i.e. from
  20.6 % duty. `[automation]` holds the defaults and a step overlays them field by
  field — `AccelSettings::overlay()`, where every field has a `kUnset` sentinel
  so a step can change one knob without restating the rest. Step options are
  **named, not positional**: with a dozen knobs a positional list would be
  unreadable and unextendable, which is why the earlier `, bench, idle` form was
  dropped before it shipped.

  Settings reach the engine through `ControlPanel::apply_automation_settings()`,
  which drives **the widgets** rather than a parallel state. Two reasons: the
  panel keeps showing what is actually running, and `selection()` stays the one
  place a `BenchItem` is built. `set_value()` clamps to each adjustment, so a
  plan asking DeepX for depth 99 lands on its real ceiling of 8 rather than
  being rejected. Axelera is the exception now that its control is a radio
  pair rather than a spin button: `cfg.depth > 1` picks On, anything else Off,
  so a plan written as `axelera.depth = 8` still means "double buffering on",
  and `axelera.double_buffer = true` says the same thing more honestly.

  **It snapshots the panel once and applies every step as baseline + defaults +
  step**, never as a delta on the previous step. Widget state is sticky, so
  without the baseline a per-step setting leaks forward: observed 2026-08-08,
  step 2's `accelerators = hailo deepx` unticked the other two cards and step 3
  — which said nothing about accelerators — silently ran on those two as well,
  so its `axelera.cores=1` applied to a card that never started. A knob a step
  does not mention means "the value it had before automation began", not
  "whatever the last step set".

  **The shipped plans deliberately do not set `accelerators`.** Unset means
  "whatever the panel has ticked", which is every card the build has; pinning a
  list would silently exclude Qualcomm on the IQ-9075, or the four M.2 cards on
  x86_64, depending on which host the file was written for.

  A plan that fails to load is **fatal at startup**, not a warning: the user
  asked for an unattended session and would otherwise return hours later to an
  idle app. `main()` strips `--automation` from argv before `Gtk::Application`
  sees it, since an unknown long option makes GTK exit.

  The driver is `MainWindow::tick_automation()`, one step per the existing 1 Hz
  tick — **not its own timer**, because it must observe exactly the engine state
  the UI is showing, and a separate timer could fire in the window between
  `stop()` and the workers being joined. It acts only through `on_start_stop()`,
  so an automated run is indistinguishable downstream: same status line, same
  CSV, same teardown. Four behaviours that are deliberate:
  - **A hand-started run is never cut short.** `auto_owns_run_` separates a run
    automation started from one the user did.
  - **The app never closes from inside `on_tick()`.** `close()` tears the window
    down synchronously, so calling it there left the rest of the tick driving
    destroyed widgets — `gtk_label_set_markup: assertion 'GTK_IS_LABEL (self)'
    failed`, then SIGSEGV (2026-08-08). `tick_automation_end()` sets `closing_`,
    `on_tick` returns false on seeing it, and the close runs from an idle
    callback with no tick in flight.
  - **A stuck `stopping()` is reported, not worked around.** A card wedged
    inside a vendor SDK call never reaches the between-frames `stop_flag` check,
    so `stopping()` can last forever — seen 2026-08-08, a MemryX timeout held it
    for 3m38s until the app was killed, with automation sitting silent. Nothing
    is safe to *do* (starting anyway would fight a card that still holds the
    device), so after `kAutoStallSeconds` it says so in the status line and the
    log. **Never "fix" this by starting the next run anyway.**
  - **Unrunnable steps are skipped and named**, rather than started and left
    producing nothing for a whole benchmark duration.
  - **Every automation transition is logged**, because the plan's durations
    appear nowhere else in the CSV and a timed stop would otherwise be
    indistinguishable from a card dying.
- **`MainWindow.{h,cpp}`** — `Gtk::Paned`: controls left, a scrolling column of
  nine `Gtk::Expander` graph sections right. Owns the 1 Hz tick.

## Invariants worth keeping

- **One benchmark subject per card at a time.** Two models sharing a card split
  its throughput *and* its watts, so per-model fps/W and mJ/frame would be
  meaningless. This is why the lists are single-selection and the three benchmark
  graphs are fixed at exactly `kAccelCount` series.
- **Fixed per-accelerator series.** `fps_`/`eff_`/`energy_` always have exactly
  `kAccelCount` series in `Accel` order, regardless of what is running or what
  is on screen; `on_tick` folds the engine's variable-length `series()` onto
  them and leaves the rest at 0. An idle card really is doing 0 fps — don't
  special-case it to a gap.

  **Nothing removes a series — hiding is always `set_series_visible()`.** That
  holds for both reasons a card can be off screen: absent from the build
  (`accel_present()`, hidden once at construction) and unticked in Graphs →
  Accelerators. `set_series()` is called once with `kAccelCount` colours and the
  engine folds onto fixed slots, so shortening the vector would silently
  misalign every card's colour and identity. If a future change needs to drop a
  card from a graph, hide it.
- **Colour means one card, everywhere.** `util::device_accent()` in `util.h`
  fixes it: Hailo coral, MemryX sage, DeepX slate blue, Axelera amber, Qualcomm
  plum. Shared verbatim with `mb-powermon-gui`, so a card looks the same in both
  apps. Two subtleties: the fixed colour is applied **before** the INA228 alias
  pass, so a mapped shunt inherits its card's colour rather than a palette slot;
  and the Qualcomm NSP names itself after the board (`IQ9075`, `QCS9075`, …), so
  `device_accent()` matches those forms too while excluding the separate
  `"<board> Board"` ambient probe (any name containing a space). Everything
  unmatched falls back to the cycling palette. No ad-hoc RGB.
- **Two orderings must agree, and they are separate.** `enum class Accel`
  (`Axelera, DeepX, Hailo, MemryX, Qualcomm`) fixes the series order on the three
  benchmark graphs and the Accelerators rows. The **Power /
  Temperature / Frequency** legends instead follow `Probes::discover()`'s call
  order. Change one without the other and the two halves of the UI disagree.
- **Efficiency needs real watts.** `Probes::power_for_device()` returns NaN for a
  card with no reading and the engine leaves both efficiency figures
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
- **The graphs keep 30 minutes and draw a window of it.** `kHistory` is
  `kMaxSpanSeconds + 1` = 1801 samples at 1 Hz — the top of the **Graphs → Time
  Range** control — while `kSpanSeconds` (300) is only the window drawn by
  default, matching the control's own 5-minute default. **Buffering the maximum rather than the window is the point**:
  narrowing the window discards nothing, so widening it again brings the older
  samples straight back instead of leaving a gap that has to refill in real
  time. `GraphArea::view_window()` is the one place the two are reconciled, and
  **both `axis_range()` and `draw()` must use it** — scaling the axis over the
  whole buffer would let a peak from twenty minutes ago flatten a one-minute
  window.

  **`Auto` draws everything collected**, so the traces fill the plot from the
  first sample and the axis widens as the session runs. **It is off by
  default** — a fixed window is what makes two runs comparable, and Auto's axis
  moves while you read it. That it exists at all is not cosmetic:
  with a fixed window the newest sample is anchored at the right edge, so a
  young history is a stub in the last few percent of the plot — with 34 PMD2
  series and a wide legend it was off screen entirely, which is what this
  control was added for.

  `GraphArea` derives its time-label step from the span in view — the largest of
  {1,2,5,10,15,30,60,120,300,600} s that still leaves ~5+ divisions — so a 60 s
  window keeps 10 s marks while 600 s gets 2 min marks instead of sixty
  gridlines. **The list starts at 1 s because of `Auto`**: a few seconds after a
  reset there is no 10 s mark to draw, and the old list left the plot with no
  vertical gridlines at all. Deriving the step from the span rather than
  hardcoding it is what keeps `GraphArea.cpp` byte-identical to
  `mb-powermon-gui`'s copy — the sibling still constructs with 601/600 and has no
  control for any of this, but the widget doesn't care. Beware: `draw()` already
  has a `step` for the trace x-spacing, hence `label_step`.
- **The axis top is a baseline plus a policy.** Percent, `fixed_max` (the 100 °C
  temperature axis) and auto-with-a-`min_axis_max`-floor each supply a *baseline*
  top; `GraphArea::RangeMode` decides what the data does to it, and the **Graphs
  → Values Range** control drives every graph at once (mixing modes between
  graphs would have them answering different questions simultaneously). It was
  called just **Range** until the Time Range row landed beside it; the label is
  12 characters because that is the width the Inference rows align to, which is
  also why it is not "Readings Range":
  - `Fixed` — baseline only, data above it clips. The original behaviour.
  - `Max` (**default**) — baseline until a reading reaches it, then
    `kHeadroom` (1.10) × peak; never shrinks back, so runs stay comparable.
  - `Dynamic` — **both ends** track the data: `trough - m` … `peak + m`, where
    `m` is 10 % of the span. Fills the plot; two runs are no longer comparable
    by eye. Three details that are not obvious:
    - the margin is a share of the **span**, not of the value. Taking
      `max(span*0.1, |peak|*0.05)` lets the second term dominate every narrow
      band — four dies between 37 and 42 °C would get a 34.9…44.1 axis and fill
      half the height, defeating the mode. `|peak|*0.05` is the fallback for a
      *flat* trace only (span 0).
    - a series that never goes negative keeps `lo >= 0`, so an idle frame rate
      rests on the floor instead of floating above -137.
    - all-zero data rests at the baseline rather than on a 1e-9 axis whose
      gridlines would every one format as "0".

  **`MainWindow::all_graphs()` is the single list both controls walk.** There
  were two hand-written lists before, and the three System graphs were missing
  from the Range one — added after the POWER-Z and PMD2 sections and never
  appended to it, so they silently ignored the control. Add a new graph there,
  not at the call sites.

  `draw()` maps values through both ends (`(v - lo) / (hi - lo)`), so the bottom
  gridline is `lo`, not 0 — that is why the axis helper is `axis_range(lo, hi)`
  rather than the old `current_axis_max()`. In `Max` and `Dynamic`,
  `set_fixed_max()` is a starting top, not a cap: a die past 100 °C used to draw as a flat line pinned to the top
  edge, which reads identically to one sitting exactly at 100. The expanded top
  is deliberately **not** `nice_ceil`-rounded — quantizing a 1370 fps peak up to
  2000 would strand the trace in the bottom half of the plot, which is the very
  readability problem the headroom rule exists to fix. `nice_ceil` is therefore
  no longer called from `GraphArea` at all; it stays in `util.h` because that
  file is shared verbatim and must not diverge.
- **`Logger` writes one CSV row per tick, always on, and is GTK-free.** It is
  the only file-writing code in the app; keep it testable headlessly like
  `Probes`/`Catalog`. Rules that are load-bearing rather than stylistic:
  - **A missing reading is an empty field.** Never `nan`, never `0` — `0.0 W`
    is a real INA228 overflow signal, and the sibling plotter renders empty as a
    gap. Same rule mb-powermon follows and for the same reason.
  - **The header is written once, at open, and never revisited.** Legitimate
    here because the metric lists are fixed for the process lifetime:
    `Probes::discover()` runs exactly once and `flatten()` only from its end.
    mb-powermon *does* defer its header up to 10 polls, because Adafruit INA228
    labels mutate after rail classification — don't copy that, it does not apply.
  - **Telemetry columns are `<bdf>_<LABEL>`, byte-compatible with mb-powermon**,
    so `../mb-powermon/csv-to-html-plot.py` plots our files unchanged (verified).
    `MetricInfo::label` is device-qualified (`"MemryX T0"`), so the device prefix
    is stripped first — otherwise the column reads `<bdf>_MemryX_T0`. That tool
    reads column 0 *positionally* as an ISO-8601 timestamp, so `time` must stay
    first and stay ISO. **Verified the hard way**: `host` was briefly column 0
    and `csv-to-html-plot.py` died with `ValueError: Invalid isoformat string`,
    not a warning. `host` is column 1 — put anything new *after* `time`.
  - **Only the three free-text columns are quoted** (`bench_model`, `<v>_cfg`,
    `message`). Telemetry stays unquoted so an idle file is shaped exactly like
    mb-powermon's.
  - **Columns exist for all `kAccelCount` cards**, including those absent from
    the build, and stay empty — two logs from one binary stay diffable.
  - Filesystem discipline follows `Fetcher`: `std::error_code` everywhere, never
    the throwing `fs::` overloads. `on_tick` runs on the GTK main loop, so a
    failed open degrades to a no-op and a failed write self-disables the logger
    rather than throwing out of the tick.
  - **Every row is `fsync`ed.** Line buffering only reaches the page cache, and
    dirty pages can sit there for tens of seconds — so a *machine* crash loses
    exactly the rows that describe it. Learned 2026-08-08 from two host crashes
    during an automation run, where the CSV's last row predated the crash by an
    unknown margin and made the timeline worthless. It is one fsync per second,
    which is nothing beside the benchmark. Line buffering is still set, for
    `tail -f`. **Do not "optimise" this away** — a forensic log missing its final
    seconds is not a forensic log.
- **The startup note names each probe's BDF, and that is the only place the log
  records which card an address is.** `discover()` writes
  `MemryX @ 0000:c1:00.0: 4 temp + 1 power sensor(s)`, and the `@ <bdf>` half is
  load-bearing: telemetry columns are `<bdf>_<LABEL>` with the device name
  stripped, and **BDFs move across reboots** — DeepX and MemryX swapped between
  2026-08-04 and 2026-08-08. Without it a finished log cannot be told which card
  is which, and a plot will silently attribute one card's data to another (this
  happened). Anything that reads a log back identifies cards from here first.
- **Messages are logged on transition, not on repeat.** `Series::error` persists
  for the life of a failed run and `note_` is rewritten every poll, so both are
  compared against the previous value before being written — otherwise a single
  failure becomes 60 lines a minute. Same for `bench_state` and for
  `Fetcher::Status::errors`, which is indexed by how many have been logged.
  `Fetcher::snapshot()` consumes its `finished_since_last` edge, so `on_tick`
  must reuse the one `poll_downloads()` already took.
- **`Series::detail` is what `<vendor>_cfg` logs.** It is the runner's own
  `describe()` and was, until the logger existed, computed every tick and thrown
  away — nothing in the UI reads it. `BenchEngine` does not expose `BenchItem`
  after `start()` (`Worker` is private and incomplete), so `MainWindow` stashes
  the items in `run_items_` to log the configuration of a run in progress.
  **Never log `run_status_`** — it is Pango markup, not plain text.
- **Sensor readings are plausibility-gated before they reach a graph.**
  `plausible_temp()` in `Probes.cpp` maps anything outside -40…150 °C to NaN, at
  all three hwmon parse sites (MemryX, the Qualcomm NSP zones, the board ambient
  probe). Drivers publish sentinels when a chip stops answering — a wedged MX3
  reports `65262000` millidegrees on **every** sensor, which is -274 °C read as
  unsigned 16-bit, just below absolute zero. This became load-bearing the moment
  the axis stopped clipping: one such sample re-tops the temperature graph at
  ~72000 °C and squashes every real card into the bottom pixel row. The Qualcomm
  site matters independently — it takes a `max` across redundant TSENS taps, so
  an ungated sentinel would always win and become that zone's temperature.
  **Never widen this to "just clamp it"**: a fabricated in-range number next to
  genuine readings is worse than a gap.

  **`plausible_power()` is the same gate for watts, and it had to exist.** An
  MX3 populated without power telemetry answers `0xFFFFFFFF` mW, which the app
  reported as **4294967.29 W**. That is worse than a wrong number on a graph:
  `power_for_device()` returns the **max** over the power family and skips NaN
  but *not* a finite huge one, so the sentinel won, became the engine's watts,
  and corrupted every fps/W and mJ/frame figure by six orders of magnitude. The
  gate maps anything outside 0…1000 W to NaN, at all four parse sites, plus a
  matching gate in the Python telemetry helper
  (`w = _w/1000.0 if 0 <= _w < 1e6 else nan`) so the sentinel never crosses the
  pipe in the first place. Both projects have it. **A card with no power sensor
  must read NaN, never a number** — the same rule as "never estimate watts".
- **The Accelerators `Enabled` filter hides series and legend entries; it does
  not drop samples.** Unticking a card calls `GraphArea::set_series_visible()`
  on every graph and hides that card's legend widgets;
  `push()` still receives every reading. That matters twice over: the filter is
  **retroactive** (the ten minutes already on screen disappear too, where masking
  new samples to NaN would have left them until they scrolled off), and a hidden
  series is skipped by `axis_range()`, so the remaining card gets the whole plot
  instead of sharing a scale with traces nobody can see. Series counts stay
  fixed — the legends, colours and `Accel` ordering all depend on that.

  The control is **independent checkboxes, one per card — not a radio group**:
  the point is comparing a chosen subset on one plot, so "Hailo and Axelera, not
  the other two" has to be expressible. It lives on each card's row in the
  **Accelerators** section and is the same switch that decides what a run
  targets; `graph_accel_mask()` returns a bit per card in `Accel` order. Unticking everything legitimately blanks the graphs;
  that is left alone rather than silently re-showing all, which would make the
  control lie about its own state.

  Mapping differs by graph family, and both mappings already exist: the three
  telemetry graphs carry one series per `MetricInfo` and are matched on
  `device_name` (so a mapped INA228 folded onto a card is kept with it), while
  the three benchmark graphs are one series per card in `Accel` order. A metric
  belonging to no card at all — an unmapped INA228, the board ambient sensor —
  has no bit to consult and stays visible.

  **The Telemetry section is a second filter, on the same signal, and it
  selects *instruments* where Accelerators selects cards.** One row per meter, in
  `discover()` order — **POWER-Z**, **PMD2**, **INA228** — which is also the
  order their graphs appear in. POWER-Z and INA228 get a bare `Enabled` switch
  each; they publish a handful of series apiece where the PMD2 publishes 34 and
  earns per-measurement boxes. **A row is built only where the instrument was
  found**, so nothing sits dead: on the x86_64 host INA228 and PMD2 show and
  POWER-Z does not; on the IQ-9075 it is the other way round.

  **INA228 is matched on the LABEL, not the device name, and that is
  load-bearing.** A mapped shunt is *folded* onto its card, so its `device_name`
  is `"Hailo"` and only the label still says so (`Hailo INA228 POWER`) — a
  device-name test would match none of the 20 shunt metrics on this host.
  Unmapped rails keep `INA228#<n>`, which the same substring test catches. The
  card filter has already had its say by then, so an instrument switch is an
  *additional* gate, not an alternative one: a shunt disappears when either its
  card or its instrument is unticked.

  **Telemetry → PMD2 is per measurement *point* rather than per series.** Ticking `ATX12V` governs its
  watts, volts and amps together, because they are three views of one rail;
  `metric_shown()` in `apply_graph_filter()` composes it with the device check.
  The row is laid out in the meter's own tiers so it reads as the hierarchy it
  is: `Enable` **alone** on the first line — it governs everything below it, and
  a reading beside it would read as one more peer of the rails — then the
  summary tier (**TOTAL** and the three group subtotals), then the ten rails
  wrapped at five. `Enabled` is a master switch that **greys** the rest rather
  than clearing them, so a chosen subset survives being switched off and back
  on. All three instrument switches are spelled `Enabled`, the same word, so the
  section reads as one kind of control.

  **The tiers are derived, not listed**, so a firmware that renames or adds a
  rail needs no UI change. `MainWindow` reads them off two structural facts of
  `PMD2Probe::discover()`: the TOTAL is the one power metric with **no family
  suffix** (so `legend_short()` leaves it untouched where it trims every
  other), and a **rail** is a point that also carries a voltage and a current —
  only rails are measured, a group is a POWER-only subtotal. Matching is on the
  exact trimmed name, which is what keeps the `EPS` group apart from the `EPS1`
  / `EPS2` rails, and `PCIE` from `PCIE1..3`. A host with no PMD2 gets no row.

  **Every one of these filters hides the legend entry with the trace**, and
  since 2026-09-18 that is per *cell*, not per row. It used to be per row —
  `LegendRow` held one flat widget list — which was enough while only the card
  filter existed, because that hides a device whole. A Telemetry switch hides
  **part** of a row: turning INA228 off takes the shunt cell off a card that
  keeps its own sensors. So `LegendRow` now carries `head` (the device name and
  its aggregate) and `cells`, each cell knowing **the index of the metric it
  draws**. A cell follows `metric_shown()`; the head follows the last surviving
  cell, because a device name and a "max —" with nothing beside them read as a
  fault rather than as a filter.

  **A hidden trace must not reach the aggregate either.** All nine per-device
  aggregates in `on_tick` skip invisible series through one
  `counted(graph, k)` helper, which asks `GraphArea::series_visible()` — the
  graph's own state, so it answers for every filter at once. Without it a row
  reports "max 3.5 W" from a shunt whose cell and trace are both gone.

  The handles for all of this are collected at build time —
  `AccelSection::cell[]` for the benchmark graphs, `LegendRow{device, head,
  cells}` for the telemetry ones. They have to be captured during construction
  because the telemetry legend is a `Gtk::Grid` laid out by device, with nothing
  to walk back to afterwards.

  **Qualcomm is deliberately one of those unmatched metrics, and this is the one
  place a fifth card is not seamless.** `accel_name(Accel::Qualcomm)` is the
  stable string `"Qualcomm"` — it labels a checkbox, an Accelerators row and a graph
  series, so it cannot vary by board — while `QualcommIQProbe` names its thermal
  rows after the device tree (`IQ9075 N0-0` …), because a temperature row wants
  to say *which board*. They therefore never string-match, so unticking Qualcomm
  in Graphs/Accelerators hides its benchmark series but leaves its six NSP
  thermal traces on the Temperature graph. That was the accepted cost of a
  stable name; the alternatives were renaming the probe (which would change what
  the shared `mb-powermon-gui` shows on this board) or making `accel_name()`
  read the device tree. `power_for_device("Qualcomm")` returning NaN is likewise
  *correct*, not a bug — this board has no power sensor at all.
- **Voyager 1.8 is performance-neutral on the sync path, and fixes nothing
  about the YOLOv8s wedge.** Measured 2026-09-20 on driver 1.6.2 + runtime
  1.8.0 + firmware 1.8.0, AxModel v5 artifacts, headless:
  - **ResNet-50, 4 cores, sync: 1621–1636 fps** — indistinguishable from 1.7's
    1598–1625. The SDK jump costs nothing and buys nothing here.
    **Superseded 2026-09-20: both figures were measured with the instance
    properties silently dropped** (see the property-separator entry below).
    With them applied the same run measures **1967 fps**. The conclusion that
    1.8 is performance-neutral still holds — the gain is our bug, not the
    SDK's — but never quote 1621 as this card's sync rate again.
  - **YOLOv8s, 4 cores: still wedges the card**, first run, with the documented
    signature (`Failed to wait for MSI 0` → `ZE_RESULT_ERROR_DEVICE_LOST` →
    `Link down` → bootloader + 1 DMA error → power cycle). The v5 artifact's
    footprint is **identical** to the v4 one — 22.97 MB DDR / 29.40 MB L2
    against ResNet-50's 0.85 / 27.03 — so the compiler lays it out the same way
    and the DDR discriminator still stands. **Do not run YOLOv8s at 4 cores to
    "check whether the new SDK fixed it"; it does not, and it costs a power
    cycle.**

- **The Axelera stack is Voyager 1.8, installed beside 1.7.** Packages carry
  their version in the name and declare no conflicts, so
  `/opt/axelera/runtime-{1.7.0-1,1.8.0-1}` coexist and the upgrade was staged:
  runtime first, driver later. Three consequences worth keeping:
  - **The C API is purely additive** (3.1 → 3.2, nothing removed), so
    `bench_axelera.cpp` compiled against 1.8 **unchanged**. What breaks is the
    *models* and the *driver*, not the code.
  - **AxModel format 4 → 5: every artifact had to be re-fetched.** The 1.8
    runtime refuses a v4 model outright (`Unsupported model version: 4.0,
    expected at least 5.0`), so `config/models.conf` points at the
    `voyager-sdk-v1.8.0` prebuilt catalog and the old artifacts must be deleted
    rather than kept as a fallback. A v5 artifact carries
    `kernel_function_N.elf` beside the `.c` — that pre-compiled kernel is why
    1.8 no longer needs the RISC-V toolchain at load, and it is the quickest
    way to tell two model directories apart on disk.
  - **`metis-dkms` must be ≥ 1.6.2** ("no earlier driver is accepted"). This
    host is still on **1.5.5**, so *nothing Axelera runs on hardware right
    now*. Note apt offers 1.7.15, newer than the 1.6.2 the 1.8.0 compatibility
    matrix names — pin the matrix version.

  **CMake trap, paid for once.** The configure globs `/opt/axelera/runtime-*`
  and takes the newest, but `find_library` **caches** its result: with 1.8
  installed the cached `AXR_LIB` kept pointing at 1.7 and the build went on
  linking the old runtime **with no warning**. `CMakeLists.txt` now drops the
  cache entry when it no longer lives under the chosen `AXELERA_DIR`. Verify
  with `ldd build/mb-benchmark | grep axruntime`, never the configure log alone.

- **Axelera instance properties are ONE PER LINE, and a `;` is a SILENT
  misconfiguration.** `axr_create_properties()` splits on `'\n'` and nothing
  else — `axr::split(s, 0xa)`, verified in the disassembly of **both** 1.7.0
  and 1.8.0. A `;`-joined string is not rejected: the first key takes the whole
  remainder as its value, `strtoul` stops at the `;`, and every later property
  is dropped without a word. `bench_axelera.cpp` joined with `;` until
  2026-09-20, so **`aipu_cores`, `double_buffer` and `async_mode` had never
  once reached the runtime** — only `input_dmabuf=0` ever applied.

  Measured the day it was found, ResNet-50 batch-4, four cores, 8 s runs:

  | property string | fps |
  | --- | ---: |
  | `;`-joined (what shipped) | 1586.0 |
  | one per line, everything set | 1903.2 · 1938.5 · **1961.6** |
  | one per line, no `double_buffer` | 1618.6 |
  | through the app's own runner, after the fix | **1966.8** |

  **+23%, and it is all `double_buffer`** — `aipu_cores` adds nothing on top of
  the connection's own `num_sub_devices` for a fixed-batch artifact. Two
  consequences: the "~1600 fps ceiling" above is retired, and **any ladder
  measured through this runner before 2026-09-20 measured the same
  configuration twice** (the `double_buffer=4` vs `=1` parity below is an
  example — both were no-ops here). A standalone probe that passed a single
  property is unaffected.

  **And a third: double buffering defaults to OFF — but NOT for the reason
  first recorded here.** The first GUI launch after this fix took the whole
  host down, and this file blamed double buffering, since that was the one
  thing the fix had newly switched on. **That was wrong, and 2026-10-31
  disproved it**: two more host freezes the same morning, both with double
  buffering **off** — one at 10:45 (four cards) and one at 10:58 (DeepX +
  Axelera, staggered). Double buffering is **not** the trigger; see the
  host-freeze entry under **Known issues**.

  The default stays Off out of caution rather than evidence — it is the
  configuration with the most hours on it — and the +23% remains an opt-in
  awaiting a soak. The shipped plans say `axelera.double_buffer = false` for
  the same reason. **Do not re-cite the 09-20 crash as evidence against double
  buffering.**

  **What made this expensive is the verification, not the change.** It was
  validated with seven **8-second** headless runs of the Axelera runner alone,
  on an otherwise idle box, with the card healthy after each. That proves the
  path executes. The documented failure here is *ResNet-50 runs two minutes,
  the next load kills it* — so 8 s proves nothing about survivability, one
  card says nothing about four plus the 1 Hz probe tick, and neither touches
  the GUI. **A change that alters how a card is driven needs a soak and a GUI
  run before it is called verified.**

  **How to check a new key**: pass it ALONE first. An unrecognised key is
  reported (`Invalid properties: Unknown property key: <k>`) only when it is
  parsed as a key rather than swallowed as somebody's value — which is exactly
  why `depth=4` produced a clean error in 1.7 while `aipu_cores` silently did
  nothing.

- **One Axelera zip holds every core build, and `Fetcher` now extracts them
  all.** The archive is `build/<m>/<m>/{1,2,3,4,quantized}/`; taking only
  `.../1/` is what made the `-Ncore` directories a manual step and left a fresh
  checkout running every model at batch 1. The vendor block carries
  `strip = build/%m/%m/%c` and `cores = 1 2 3 4`: the first entry lands in
  `<artifact>`, the rest in `<artifact>-<N>core`, which is where
  `bench_axelera.cpp` looks. One download, four directories. A missing `-Ncore`
  subtree is non-fatal (not every model is compiled for every count); a missing
  primary one is. **Disk cost roughly doubles** — ~225 MB per core build.

- **Axelera AIPU cores defaults to 4, and now selects the fixed-batch build.**
  The control picks the model's `<dir>-Ncore` artifact: a batch-N model on one
  connection with one instance and `aipu_cores=N`. Measured 2026-09-03 on
  ResNet-50: 359 / 632 / 1154 / **1602** fps at 1 / 2 / 3 / 4, so the top of the
  range is where the card performs.

  **The old default was 2 because 4 wedged the card. That was the
  multi-connection implementation** — four `axr_device_connect()` calls, each
  reloading firmware, and four instances contending on one command queue. The
  batched path makes exactly one connection and one instance, so that mechanism
  is gone. `$MB_AXELERA_MULTI_INSTANCE=1` reinstates the old arrangement, and
  reinstates the risk with it.

  **A model with no matching `-Ncore` build falls back to batch 1 on one core**
  and says so in `describe()` (`no 4-core build; using batch 1`), which lands in
  the CSV's `axelera_cfg`. That is a real throughput cost for the four models
  that have no multicore artifact — yolov8s measures 130 fps at batch 1 against
  368 with `MB_AXELERA_MULTI_INSTANCE=1` on four cores. Never label a batch-1 run
  as N cores; report the fallback instead.
- **Inference -> Stagger starts the cards one at a time** (2026-09-21): an
  `Enabled` box and a delay in seconds (default 5, range 1-60, off by
  default). `ControlPanel::selection()` gives the k-th enabled card, in
  Accelerators order, `BenchItem::start_delay_s = k x delay`; the worker waits
  **before `make_runner()`**, so a card's device is untouched until its turn,
  in a new `State::Waiting` (appended to the enum, shown as "waiting for its
  turn", logged as bench_state `starting`). The wait polls `stop_flag` every
  50 ms so Stop is not held for the full delay. A staggered run is noted in the
  CSV (`stagger start: N s apart, Hailo -> …`) because no `<vendor>_cfg`
  column would otherwise show it. **Why it exists:** Axelera survives alone (~1960 fps with double
  buffering, ~1625 without) and failed the moment other cards were involved.
  **Run 2026-09-21 and 10-31, and it is NOT a fix** — it was a test of the
  simultaneous-inrush hypothesis and it **refuted it**: on 09-21 Axelera
  loaded alone 15 s after the other three and still took the link down within
  a second; on 10-31 it loaded fine behind DeepX and the host froze instead.
  Keep the control — staggering is how those two facts were separated, and it
  is the right way to run any future concurrency test — but do not expect it
  to make a mixed-card run safe. Not settable from `--automation` yet.

- **Graph order is System Voltage, System Current, System Power, Accelerator
  Voltage, Accelerator Current, Accelerator Power, Accumulated Energy,
  Temperature, Frequency, Airflow, Frame Rate, Efficiency, Energy.** Airflow
  exists only with an RA6M5 board carrying an FS3000. Each triplet reads
  voltage -> current -> power, because the meter measures V and I and *derives*
  W; board total first, then the per-card breakdown inside it. The telemetry graphs
  lead because they are live with no run in progress.

  **Voltage and Current come before Power deliberately, and that ordering is the
  point of them.** The INA228 *measures* VBUS and the shunt drop and *derives*
  POWER as their product, so in this order a sagging rail reads top-to-bottom:
  current rises, voltage falls, power is what results. Power-first inverted the
  causality — it was the arrangement in use while a 190 mV sag under load went
  unnoticed for the whole MemryX investigation. Accumulated Energy sits under
  Power because it is its integral, and Frequency under Temperature because a
  clock sagging as a die heats is the whole reason to plot it. Everything except
  Power, Temperature and Frame Rate is collapsed by default.

  `tools/csv-to-html-plot.py` mirrors this order; change one and change both, or
  a plot stops matching the app it came from.

  **Three pairs of graphs share a unit and are different quantities.** System vs
  Accelerator Voltage, Current and Power: the first is the whole board from an
  inline supply meter, the second is the per-card rails. Don't merge any pair,
  and never let a board figure reach a card's efficiency numbers.

  **Two graphs have "Energy" in the title and they are different quantities.**
  *Accumulated Energy* is joules read from the INA228 hardware accumulators, one
  series per rail, always live. *Energy* is mJ/frame derived from the benchmark,
  one series per card. Don't merge them.
- **The header bar carries the panel toggle at its start, the About button at
  its end.** The toggle collapses the control pane and gives its width to the
  graphs — the panel is set-and-forget, so once a run is configured it is ~40 %
  of the window spent on controls nobody is touching. `set_panel_visible()` is
  the single place it happens, and it **hides the Paned's start child**: GtkPaned
  gives the whole area to the remaining child and drops the handle. Detaching the
  child instead would re-parent live widgets and reset `set_position()`.
  Nothing is rebuilt, so every control keeps its state and the graphs keep their
  history across a hide/show.

  **One icon on a `Gtk::ToggleButton`, deliberately not two swapped.**
  `sidebar-show-symbolic` is in both Yaru (the active theme) and Adwaita (the
  fallback); **`sidebar-hide-symbolic` is Yaru-only** and would render blank for
  anyone on stock Adwaita. A ToggleButton draws its own checked state, so there
  is nothing to swap.

  **Ctrl+B is the first and only keyboard shortcut in either app.** It is a
  `Gtk::ShortcutController` on the window, scope **MANAGED** so it fires wherever
  focus sits, rather than an action plus `set_accels_for_action()` — nothing here
  holds the `Gtk::Application`, since `main()` uses `make_window_and_run()`. It
  flips the *button*, not the pane, so there is one code path and the button
  cannot disagree with what is on screen. Note `Gtk::CallbackAction` is declared
  in `<gtkmm/shortcutaction.h>`, not a header of its own.

  **The toggle is per-session.** Neither app has GSettings, a KeyFile or any
  window-state save — the only file either writes is a CSV log — so the panel is
  visible on every launch. Persisting it means inventing the first preference
  store in the project; do that deliberately, not as a side effect.
- **Window chrome.** Bottom corners are rounded to 12px to match GNOME's own
  windows; plain GTK4's Adwaita rounds only the top two on a CSD window. The
  radius goes on the `decoration` node (that is what shapes a CSD window), and
  any child painting an opaque background at the window edge — the `Paned`, the
  graph `ScrolledWindow` — must be transparent or it squares the corner off
  again.
- **`make_section(title, content, expanded)`** seeds `vexpand` from `expanded`
  *and* binds it to the property signal. Both halves matter: seeding alone was
  the original bug (a section that started collapsed could never grow), and
  binding alone would start a collapsed section claiming space it isn't using.
- **Per-device legend aggregates: `max` for temperature and power, `avg` for
  frequency.** Temperature averaged its sensors until 2026-09-15 and now takes
  the max, for the reason power always did: a card's sensors sit on different
  dies and the **hottest** one is what throttles or trips, so a mean buries a
  single die running 20 °C above its neighbours — exactly the case the row
  exists to surface. Frequency keeps the mean deliberately: throttling moves a
  card's chips together, so the average reads as "the card's clock". NaN
  readings are skipped in all three, and a device with no valid reading shows
  `max —` rather than a fabricated 0. **Both apps must agree** — the same
  change is in `mb-powermon-gui`, and the member is `temp_agg_labels_` (renamed
  from `temp_avg_labels_`, which said "avg" while displaying max).
- **The three benchmark legends are one row of the cards this build has**
  (`col % kAccelCount, col / kAccelCount`, where `col` counts *visible* cards so
  a hidden one leaves no gap — see `accel_present()` above) and carry no
  explanatory text. Failures and loading
  state therefore go to the status line in `on_tick` — do not let a card sit
  silently at 0, which is how a broken model looks exactly like a slow one.

## Telemetry families (Probes)

`Probes` publishes **twelve** flat metric families — temperature, power,
frequency, energy, charge, voltage, current, the system voltage / current /
power of an inline supply meter, airflow and fan (RA6M5 board) — each with an aligned value
vector refreshed by `poll()`. Frequency alone follows plain discovery order,
because a clock always belongs to the card reporting it and there is nothing to
fold; the other six use `alias_device_order()` and the INA228 fold.

**Voltage and current must never join `power_`.** `power_for_device()` returns
the *max* over the power family and that value becomes the engine's watts — a
3.3 parked there would be reported as watts a card is not drawing, and would
corrupt every fps/W and mJ/frame figure. Same reasoning that keeps energy and
charge separate, and the reason each of these has its own family rather than a
shared "electrical" one: a `GraphArea` carries one unit and one formatter.

**A folded INA228 metric is named `<Card> INA228 <FAMILY>`** — `POWER`, `TEMP`,
`ENERGY`, `CHARGE`, `VBUS`, `CURRENT` — giving CSV columns
`<bdf>_INA228_<FAMILY>`. The families share one CSV namespace, so a bare
`<Card> INA228` for all of them would collide.
Power carried exactly that bare name until 2026-09-03; the plotter still accepts
it so older logs keep working, but nothing writes it any more.

**Energy and charge are separate families on purpose, and neither may join
`power_`.** `power_for_device()` returns the *max* over the power family and that
value becomes the engine's watts — joules parked there would overtake watts within
seconds and silently corrupt every fps/W and mJ/frame figure in the app. And a
`GraphArea` carries one unit and one formatter, so joules and coulombs cannot
share a plot. Energy is graphed; **charge is CSV-only**, which is why it has a
family but no section in `MainWindow`.

**`alias_device_order(bool alias_first)` is not cosmetic.** A folded metric takes
its target's device index, and the legend groups by *contiguous runs* of that
index — so a folded reading emitted in discovery order (the FTDI probes are
discovered last) would open a **second legend row** for a card that already has
one. Power passes `true`, preserving the original "INA228 sits immediately above
Hailo" ordering; temperature, energy and charge pass `false`, so a card's row
reads `TS0 · TS1 · INA228` rather than leading with the shunt.

**The INA228s report four things, not one.** `POWER` (0x08) was the only register
read until 2026-09-03; the shunt monitors also carry `DIETEMP` (0x06),
`ENERGY` (0x09) and `CHARGE` (0x0A), and the ADC was **already converting all of
them** — `ADC_CONFIG = 0xFB6B` is mode `0xF`, continuous bus + shunt + temperature.
Reading them needed no configuration change.

| register | width | scale | notes |
| --- | --- | --- | --- |
| `DIETEMP` 0x06 | 16-bit signed | `7.8125 m°C/LSB` | a *full* 16-bit register — `read24_raw20()` is the wrong helper, and `read16()` is unsigned so it needs the cast |
| `ENERGY` 0x09 | 40-bit unsigned | `16 × 3.2 × current_lsb` J | 488 µJ/LSB as configured; rolls over after ~3.4 years at 5 W |
| `CHARGE` 0x0A | 40-bit signed | `current_lsb` C | sign-extend from bit 39 |
| `VBUS` 0x05 | 24-bit, 20 used | `195.3125 uV/LSB` | unsigned, bits 23:4 |
| `CURRENT` 0x07 | 24-bit, 20 used | `current_lsb` A | **signed** — sign-extend from bit 19; `read24_raw20()` alone reports ~1.05 MA instead of -3.9 A |
| `DIAG_ALRT` 0x0B / `BUVL` 0x0F | 16-bit | `3.125 mV/LSB` (BUVL) | latched undervoltage; read clears |

**`VSHUNT` (0x04) is deliberately not read.** It is the raw measurement, but
`CURRENT` is derived from it through `SHUNT_CAL`, so `VSHUNT / CURRENT` is always
the programmed 15 mOhm — a tautology that confirms the calibration was written,
not that the physical shunt is 15 mOhm. It would add only an ADC-headroom check
(58.5 mV at the 3.9 A peak, 36% of the +/-163.84 mV range at ADCRANGE=0) and a
canary for lost calibration. Worth a one-off probe, not a permanent series.

**The bus-undervoltage detector is armed, and it is the instrument for a
brown-out question.** `BUVL` (0x0F) is set to **3.00 V** — the M.2 3.3 V rail's
own lower limit (3.3 V -9% = 3.003 V), so a trip means the supply left spec, not
merely that it sagged — with `ALATCH` (DIAG_ALRT bit 15) so a trip is held.
Reading `DIAG_ALRT` (0x0B) *clears* the latch, which is the whole point: polled
once a second it answers **"did the rail dip at any point in that second?"**,
catching excursions far shorter than a 1 Hz sample could ever see. Bit 3 is the
flag. Verified on hardware 2026-09-07 by setting the threshold above the live
rail (flag sets), then below it (flag clears); `3.125 mV/LSB` was measured this
way, not taken from memory.

**Exactly one `undervoltage()` call per sensor per poll.** A second read consumes
the latch and the counted read then always sees zero — the detector would
silently never fire. This was written and caught during the port; both projects
now have a comment on the call saying so.

**Its limit is the averaging.** `ADC_CONFIG` sets AVG=64 (~67 ms per conversion)
and the comparator sees that *average*, so a dip much shorter than that is
smoothed away before it can be detected. A negative result therefore rules out
sustained excursions, not microsecond transients — lowering AVG trades noise for
bandwidth if that ever matters.

**Measured on the rail, 2026-09-07**: two clean load points (8.0 W / 2.56 A and
12.0 W / 3.88 A) give a source impedance of **~49 mOhm** and an open-circuit
**3.271 V**. The floor of 3.003 V would need **5.44 A / 16.3 W**; the vendor's
850 MHz figure of 20.2 W implies ~6.7 A and **~2.94 V, below spec**. At 600 MHz
the supply has margin and the detector has never tripped.

**The die temperature is the monitor chip on the breakout, not the accelerator** —
ambient plus shunt self-heating. Measured 2026-09-03 it runs several degrees below
the card it is folded onto (MemryX 48.7 °C against that card's 52 °C on `T0`), which
is the sanity check: if it ever *tracks* a card's die sensor, the fold has attributed
the wrong reading. It is named `<Card> INA228 TEMP` so it cannot be misread as a die
temperature, and it is `plausible_temp()`-gated like every other temperature.

**All four shunts are wired IN+/IN- reversed, and `ina228.conf` now corrects
it.** The INA228 reports the shunt drop *signed*, so reversed leads make a card
that is drawing read negative — which is what `CURRENT` and `CHARGE` did until
2026-09-07. `POWER`, `VBUS` and `ENERGY` come from unsigned registers and were
never affected, which is why the fault showed in only two families.

Confirmed before it was corrected, rather than assumed: `VBUS x |CURRENT|`
matched `POWER` to within **0.2% on all four rails**, so only the direction was
wrong. (The older evidence was `P / |dQ/dt|` landing at 3.16-3.20 V on every
rail, consistent to 1% — the same conclusion by a longer route, from before
`VBUS` and `CURRENT` were read at all.)

The fix is a per-rail `invert` flag in `config/ina228.conf`, applied in
`INA228Probe::poll()` to the two signed families only. **Per-rail, not global**:
the harness can be corrected one breakout at a time, and a global switch would
then be wrong for every other rail. Applying it to `VBUS`/`POWER`/`ENERGY` would
be wrong, not merely redundant — they cannot carry a polarity error.

**Consequence for old logs**: `<bdf>_INA228_CHARGE` is negative before
2026-09-07 and positive after. Anything differencing charge across that boundary
must account for it. Correcting the wiring instead would let the flags be
dropped one line at a time, and is the better long-term fix.

**`configure()` pulses `RSTACC`** (CONFIG bit 14, written then cleared — it is an
ordinary R/W bit and stays latched otherwise) so a session's energy series starts
at zero. Like the Axelera collector level this is a *device-state* write another
client would see; kept because nothing else on either host touches these
accumulators, and a series starting from an arbitrary carried-over value would be
unreadable.

**The accumulators are the accurate path, and the graph is the weak view of them.**
They integrate at the ADC rate (~5 Hz at AVG=64 / 1052 µs), not at the GUI's 1 Hz
sampling. Verified 2026-09-03: `dE/dt` matches the `POW` reading to within 2–3% on
all four rails, which is the single best check that the scale factor is right — a
wrong `current_lsb` multiplier shows up immediately. But over the 10-minute graph
span a monotonic accumulator draws a near-straight line whose *slope* is average
power, which the Power graph already shows directly. **The value is the CSV
column**, where differencing two rows gives exact interval energy. Don't "fix" the
graph for looking boring.

**All four M.2 cards** report a core clock, each from a different place. The
Qualcomm NSP does **not** — its probe is passive thermal only, so the Frequency
graph is empty on a Qualcomm-only host:

| Card | Clock | How |
| --- | --- | --- |
| Hailo | ✅ | `get_extended_device_information()->neural_network_core_clock_rate` (Hz; 400, fixed) |
| DeepX | ✅ per NPU | same `dxrt-cli -s` line as the temperature — parsed in one shell-out, not two (1000, fixed) |
| MemryX | ✅ per chip | `mxa.get_frequency_effective(dev, group)` over the persistent Python helper (300 idle → 600 load) |
| Axelera | ✅ per AI core | `axcmd --clock-all-actual`, parsing `aicore<N>: <F>MHz` (50 idle → 800 load) |

**Twice I wrote a card off as "no clock" and was wrong both times**, in both
cases from grepping headers rather than calling the API. Don't repeat it:
- **Hailo** — grepping for `clock`/`frequency` finds only
  `hailo_mipi_clock_selection_t`, which is *MIPI camera input*. The real field is
  `neural_network_core_clock_rate` and contains neither word in a greppable
  form. `get_throttling_state()` and `current_temperature_throttling_level`
  exist too, if a throttling indicator is ever wanted.
- **Axelera** — the collector log has temperatures but no clock, which looks
  conclusive. `axr_read_device_configuration()` exposes `clock_profile`,
  `min/max/valid_clock_frequencies`, `freq_downscaling`, `sw/hw_throttling`; and
  `axcmd` has `--clock-all-actual`, `--get-ck-profile`, `--set-ck-profile`.

Use the **actual/effective** reading on the two DVFS parts, never the configured
one: `get_frequency()` and `--get-ck-profile` return the target and sit flat
however hot the part gets, which defeats the purpose.

**The Axelera clock graph exposes a real defect in our own runner.** Under a
single-model benchmark only `aicore0` ramps to 800 MHz — `aicore1..3` stay at
50 MHz. Three of the four AI cores are idle, despite `axr_device_connect()`
being asked for 4 sub-devices. That is a large part of the ~18%-of-vendor
ResNet-50 result, and it is a *core utilisation* problem, not the
stream-parallelism story originally assumed. Verified safe: polling `axcmd`
during a run does not disturb the benchmark. The probe is otherwise passive —
its one write is the collector level, once (see `tools/`).

**MemryX's clock is DVFS-managed** and it is the one that visibly moves: 300 MHz
idle, 600 MHz (its configured target) under load. A low idle reading is *not*
evidence of a stuck clock — check it under load before concluding anything about
throughput from it.

MemryX API traps, both hit while wiring this up:
- `get_frequency*` take **`(device, group)`**. A single int raises
  `TypeError: bad argument type` — unlike `get_power(0)`, which takes one.
- The group index is **not bounds-checked**: a group beyond
  `get_total_chip_count()` returns nonsense (2 MHz) instead of failing. Gate the
  loop on the chip count.
- Use `get_frequency_effective`, **not** `get_frequency` — the latter is the
  configured target and sits flat at 600/850 no matter how hot the part gets,
  which defeats the point. Idle, this host reads effective 300 against a
  configured 600.

**The system triplet is separate from the accelerator triplet, all three of
them.** `sysvoltage_` / `syscurrent_` / `syspower_` never join `voltage_` /
`current_` / `power_`. Same units, different subject: 19.9 V of USB-C board
input on the same axis as a 3.3 V card rail, or 22 W of board draw against a
Hailo's 0.85 W, flattens the card trace the graph exists to show. The graphs are
titled **Accelerator** Voltage/Current/Power and **System** Voltage/Current/
Power so neither can be mistaken for the other.

**System power is its own family and its own graph.** The POWER-Z's watts go to
`syspower_`, never `power_`, for two independent reasons. They are a different
*quantity* — board total against one card's draw — and they are an order of
magnitude larger, so sharing the Power axis would flatten every card trace into
the bottom pixel row. Keeping them out of `power_` is also what keeps them out
of `power_for_device()`, which returns the max over that family and feeds the
engine its watts: board power landing there would inflate every fps/W and
mJ/frame figure by ~10x. The graph sits between **Current** and **Power**, so
the column reads rail voltage -> rail current -> resulting system watts -> what
each card draws inside that.

**The POWER-Z KM003C is read through the kernel, not libusb.** The in-tree
`powerz` hwmon driver binds the meter's vendor interface automatically, so
`PowerZProbe` is plain sysfs — no claim, nothing that could fight the meter's own
app, passive like every other probe here. It publishes VBUS, IBUS, derived power
and the meter's die temperature; `in1..in5` (CC1, CC2, D+, D-, internal VDD) are
deliberately not exposed, because a 1.6 V CC line sharing the voltage graph with
a 19.9 V bus squashes the trace that matters.

**Its current is negated, and that is a convention rather than a fault.** The
KM003C reports IBUS *negative while the sink draws*; negating makes a drawing
board read positive like every other current in the app. Do not confuse this
with the INA228 rails, where reversed leads were a real wiring bug fixed
per-rail in `ina228.conf`. Power is taken as `|V x I|`, matching the INA228
POWER register, which is unsigned by hardware — so it stays correct if the meter
is ever installed the other way round.

**What it measures is the whole board.** Verified 2026-09-07 by loading eight
CPU cores: 11.3 W idle -> 22.5 W and back, with VBUS sagging 19.86 -> 19.75 V.
That is the only real wattage available on the IQ-9075, and it is genuinely
useful — but it is not per-accelerator power, and the probe is named so that it
can never become a card's `power_for_device()` value.

**`Probes.{h,cpp}` is shared with `mb-powermon-gui`** and the two are now close.
The **frequency family was ported there 2026-09-15** — all four clock sources,
plus the Frequency graph — so the sibling has `freq_metrics_` and a
`Frequency (MHz)` section of its own. **`PMD2Probe` was ported there
2026-09-18** — the whole class, its two static name tables, the `<termios.h>`
include and the `discover()` registration in PowerZ's slot — so the sibling now
has the System Voltage/Current/Power sections populated on this host, which had
no POWER-Z. Its one edit was the header comment: that comment names
`power_for_device()`, which **cannot exist there** (it needs `Catalog.h`), so it
was reworded rather than carried over. `PowerZProbe`'s system V/I/W triplet is
ported too — an earlier version of this paragraph said it was not, and that was
already stale. Everything INA228 **has** been: energy/charge and the
die-temperature fold (2026-09-03), then `VBUS`, `CURRENT` with the per-rail
`invert` flag, the latched undervoltage check and `plausible_power()`
(2026-09-07 … 09-12). **Not ported: the RA6M5 probe and the airflow family
(2026-11-16)**, nor the `rails::` move of the config loader that came with them
— port those as one hunk set if the sibling ever needs the board.

**Three divergences bit during the frequency port, and each would have silently
no-op'd a `str.replace`** — they are the concrete form of "port hunk by hunk":
- the sibling's `Probes` class declares members **combined**
  (`std::vector<MetricInfo> temp_metrics_, power_metrics_;`) where this one has
  them on separate lines;
- its DeepX probe returned `std::vector<double>` from `read_temps()`, where this
  one parses temps *and* clocks from the same `dxrt-cli -s` line via a `Reading`
  struct — a rewrite, not a copy;
- its MemryX drain used `std::stod(line)`, which silently takes the watts and
  drops every clock now on that line. It needs the `istringstream` parse.

Frequency is also the one family that does **not** go through `emit()`/`fill()`:
it uses plain discovery order, because a clock always belongs to the card
reporting it and there is nothing to fold. That sidesteps the documented
signature difference (the sibling's helpers take no `order` argument).

**`ina228.conf` is not shared and the search orders differ.** Ours is
`$MB_INA228_CONFIG` → repo `config/` → `~/.config/mb-benchmark-gui/`; the
sibling's is `$MB_INA228_CONFIG` → `~/.config` only, with no repo copy. A
`config/ina228.conf` was created in the sibling once and was **inert** — it is
never consulted. Don't add one back.

### The RA6M5 probe board (2026-11-16)

A SparkFun Thing Plus RA6M5 running `firmware/mb-probe-ra6m5/` is the second
kind of INA228 carrier, **alongside** the FT232H — and the opposite design: the
FT232H is a dumb I2C bridge driven from the host, the RA6M5 is *smart*. Its
firmware owns the sensors, takes every INA228 conversion (~5 Hz) and every
FS3000 sample (8 Hz), and reports per-period mean/min/max as `key=value` text
lines over USB CDC. **`docs/ra6m5-probe.md` has the protocol, the one-time
toolchain setup and the first readings** — read it before touching either side.
`tools/flash-ra6m5.sh` builds and flashes with no button press.

Sensors today: INA228 (0x40), FS3000-1005 airflow (0x28), the EMC2101 fan
controller (0x4C) and a MAX31855K thermocouple on SPI (CS = D0). Of the EMC2101,
only the die temperature reaches the GUI. Both temperatures sit
on the Temperature graph as `RA6M5 EMC2101` and `RA6M5 MAX31855` (columns
`usb <port>_EMC2101` / `_MAX31855`), **named after the chip** because two
share one family on one probe and a bare `TEMP` would collide. An open
thermocouple is a gap, not a value. The fan (fw 0.4.0) is a `fan` family of
its own — `FAN_RPM` / `FAN_DUTY` columns, real units — **drawn on the Airflow
graph for now**, scaled onto that axis (rpm / 1000, duty / 10) at `push()`
only; separating it later is a MainWindow-only change (`af_metrics_` /
`af_src_`). **Telemetry -> Fan is the first control in that section that
ACTS on hardware**: every change sends `fan <percent>` and logs a note, and
it seeds itself from the board's reported duty rather than a default, so
launching the app never stops a running fan. The tach reads **only with the
breakout's `TACH` solder jumper bridged** (it ships open); see the doc. Two
boards exist (serials ending `A8CC…` and `3BA5…`), both mapped to Hailo. 0x36 on the bus is
the board's own MAX17048 fuel gauge and is ignored.

**Since firmware 0.6.0 the fan is driven by a SINGLE closed loop running ON
THE BOARD** (0.5.0 introduced it with three modes; 0.6.0 collapsed them).
Every input it needs is there — heatsink thermocouple, card watts, board air —
so it keeps cooling the card when this app exits, crashes or loses the port,
which is the whole argument for putting it there.
`firmware/mb-probe-ra6m5/Control.h` carries the design and the provenance of
every constant; `docs/ra6m5-probe.md` has the wire protocol.

**THE THREE MODES WERE ONE CONTROL LAW, and collapsing them is what 0.6.0 did.**
Reading 0.5.0 back against its own 225-minute four-setpoint run: in die mode

    e = die_est − target = (tc + rint·W) − target = tc − (target − rint·W)

and `(target − rint·W)` is exactly the heatsink target the feedforward already
computes. **The observer is a setpoint transformation, not a feedback
element**, so "regulate the die" and "regulate the heatsink" differed only in
whether that setpoint moved with load, and `host·die` vs `smart·die` differed
only in which constant landed in `target`. All three shared one error path —
which is why one defect produced three different failures in that run. Don't
reintroduce modes; the axes that are real are the setpoint, its source, and
whether it was defaulted.

**Power is mandatory, the thermocouple is substitutable — the reverse of
0.5.0.** `ctl.src` reports which of three sources the loop actually used:
**0 measured** (the thermocouple, closed loop with PI), **1 modelled** (a
fitted estimate of it, closed loop with PI), **2 none** (open loop, the
feedforward alone and no PI at all). The control law is identical for the first
two; only where `tc_` comes from changes — a soft sensor is a drop-in for one
input, not a second controller.

**ONE SELECTOR SINCE 0.7.0, and `ctl.tc` is now the REQUEST while `ctl.src`
stays the RESULT.** 0.6.0 had `ctl.tc` and `ctl.nn` as two *effective* bools,
which made four combinations for three meanings and, being effective, only
restated `ctl.src` — they told a reader nothing the one enum did not. Now:

| `ctl.tc` | normal | thermocouple lost |
|---|---|---|
| `off` | open loop, feedforward only | — |
| `real` | measured | the model if one exists, else open loop · `degraded` |
| `model` | modelled | — (it is not using the probe) |

**Degradation is automatic, not configured** — `real` → `model` → open loop was
0.6.0's `tc on, nn on`, and making it a choice only invited someone to switch
the fallback off. **`model` deliberately does not fall back to the probe**:
asking for the estimate while one is fitted is exactly what the experiment
does, and preferring the probe would make that arm unmeasurable. The pair
disagrees exactly when a request was refused or a fallback fired.
- **`ctl.src` kept its column name and changed its meaning** in 0.6.0 (it was
  die-vs-heatsink). A pre-0.6.0 log's values do not mean this. `kProtocol`
  went 1 → 2 to say so, and **2 → 3 in 0.7.0** when `ctl.nn` was removed and
  `ctl.tc` went from bool to enum.
- **`ctl.nn` is gone and the command is REFUSED**, with a pointer to
  `ctl.tc off|real|model`. `ctl.model_ready` replaces the host-side guessing it
  enabled — see the `nn_available` entry under **Known issues**.
- **THE MODEL IS FITTED (2026-11-24) and `ctl.model_ready` reports 1.** See
  **The soft sensor** below for the fit and its error budget. Two consequences
  for reading a log, both of which invalidate advice this file used to give:
  `ctl.tc model` is now **honoured**, so `ctl.src` reports **1 modelled** and
  `ctl.state` is a normal running state — a run asking for the model can no
  longer be identified by its getting `src = 2` and `state = 6`. And the three
  arms of `automation-thermal-hailo-tc.conf` are no longer two baselines plus a
  repeat: arm 3 now measures what it was written to measure.
- **The firmware hook is `model_estimate()`, not `nn_*` — and the fit
  vindicated the name.** The prediction was that the first fit should be a
  first-order thermal model with a learned time constant: three or four
  physically interpretable parameters, monotone in duty, which `invert_curve`
  provably is and a net is not. That is exactly what it turned out to be — one
  pole on the existing curve plus one capacity, 0.52-0.87 °C held out. Naming a
  hook after a technique has to be undone when the technique changes; naming it
  after the role does not.
- **Open loop reports `nan` for `ctl.in` and `ctl.err`**: nothing is measured,
  so quoting an error would invent one. `ctl.die_est` equals `ctl.target` by
  construction while unclamped, and diverges exactly when the fan saturates,
  which is the one case it is informative.

**MEASURED 2026-11-22/23, four setpoints over 225 minutes**, on 0.5.0. The run
data is `logs/mb-benchmark-20261122-221548.csv` — **keep it**: it is half of
the 47 282 thermocouple-labelled samples that exist, and the first where duty
came from a closed loop rather than a ladder. The corpus is **21 179** (the
2026-11-22 fan ladder, `docs/data/ra6m5-thermal-hailo8-20261122.csv`) +
**13 502** (this 0.5.0 run) + **12 601** (the 2026-11-23 three-arm run,
`docs/data/ra6m5-loop-tc-20261123.csv`) — and all 12 601 of the last one carry
a thermocouple reading even in the arms where it was not in the control path,
because the MAX31855 is always sampled and always logged. **Train and validate
a soft sensor on the LOADED windows**, where the plant repeats to ~1 %; the
fan-off brackets are a check that conditions returned, not a source of model
inputs. All of that advice was followed and it held — see **The soft sensor**.
Two more runs are curated since: `docs/data/ra6m5-softsensor-fit-20261123.csv`
(14 932, the fitting run) and `docs/data/ra6m5-loadprobe-20261123.csv` (4 267,
the eight-model load probe), bringing the corpus to **66 481**.
- **The loop works.** 70/85 °C die and 60 °C heatsink setpoints, plus the
  mode's own 95 °C, all held to **≤ 0.03 °C of standing error** over ten
  minutes each, settling in 6.5–11.8 min (the plant arriving, not the
  controller deciding — the fan moves inside one second). **0.00 % throttled,
  400 MHz and 1372.3 ± 0.47 fps across all 7 192 loaded samples**, hottest die
  94.88 °C. So the 95 °C bound holds under a controller, not just a ladder.
- **Every failure it found was ONE defect, and 0.6.0 fixes it with one
  constant.** The error during any approach is "model error" plus "the plant
  has not got there yet", and nothing separated them, so the PI demanded the
  fan compensate for the plant's own time constant. Both signs were measured:
  arming from a hand-set 100 % fan preseeded the integrator to **+103.4 duty
  points** and pinned the fan at its 73 % ceiling for **9.4 minutes** while the
  die was already 10.3 °C *below* setpoint (20.1 min to settle, against 6.5 for
  the same law at a different setpoint); and on a cold start `kp·err`
  subtracted **29.7 points** from a feedforward that had correctly asked for
  33.2 % at 4.33 W, flooring the fan for 40 s and overshooting by 3.5 °C.
  **The fix is `kTrim`: the PI may trim the feedforward by at most 15 duty
  points.** Stated that way rather than as more anti-windup, because the
  integrator was unwinding *correctly* from a value the loop could never use.
  **Do not file the first case as an overshoot** — the die never once rose
  above the setpoint in that step.
- **`ctl.state 3` used to mean two opposite things** and is now split into
  `kFloor` (3) and `kCeiling` (4). Every idle in that run reported "saturated"
  while sitting at the 9.5 % floor with a slack setpoint, which is the opposite
  of running out of cooling authority.

**MEASURED 2026-11-23, three arms over 210 minutes**, on 0.6.0 — the run that
tested the substitution itself. Data `docs/data/ra6m5-loop-tc-20261123.csv`,
report `docs/reports/ra6m5-loop/`, plan
`config/automation-thermal-hailo-tc.conf`. **One** setpoint (90 °C), one load,
three arms differing only in `fan.thermocouple`, each bracketed by 30 minutes
of fan-off idle (`fan.idle = 0`).

- **`kTrim` works and the 0.5.0 defect is gone.** Arm 1 armed from a fan held
  at 0 % with the card 41.8 °C below setpoint — the same shape that pinned
  0.5.0. The rearm preseeded the integrator to **+44.95**, which is exactly
  `−kp·err` at kp = 1, so the trim saw ~0 and the first commanded duty came out
  *above* the feedforward (12.7 % against 9.5) rather than below it. The
  integrator then unwound monotonically to zero over **450 s** (−6.0
  points/min). Over the whole 30 minutes the PI moved the feedforward by
  **−1.50 … +5.50 duty points — 37 % of its 15-point budget**. Overshoot
  **+0.280 °C** against 0.5.0's +3.5. `kCeiling` never fired; `kFloor` fired
  only on the arming second. **116 samples held an integrator above 15 and not
  one reached the fan** — the bound is `±kTrim − kp·err`, not `±kTrim`, so a
  large integrator against a large error is *correct* and is not windup.
- **Open loop matched closed loop, and the agreement is below the noise.**
  Settled die error (last 10 min, card's own sensor): **−0.318 ± 0.101 °C**
  closed against **−0.334 ± 0.127** open — a difference of **0.016 °C**, on the
  same two EMC2101 levels, 0.152 duty points apart, 8 s apart in time-to-reach,
  1372.29 vs 1372.28 fps, 0.00 % throttled in both. The open arm overshot
  *less* (+0.078 vs +0.280) because there is no integrator to unwind.
  **Do not quote 0.016 °C as the result** — see the repeatability bound below.
- **What losing the thermocouple costs is the ESTIMATE, not the temperature.**
  `die_est − die` over each settled window: closed **+0.343 ± 0.077**, |max|
  0.560, and it **never once** read the die cooler than it was (0 of 600).
  Open: **+0.221 ± 0.591**, |max| 1.402, under-reading on **303 of 600 — half
  the hold** spent believing there was more margin than there was, and
  undetectable from the board. (32.5 % over the whole arm, because the approach
  is dominated by `die_est` tracking the setpoint by construction; the settled
  half is the figure that means something.) The sd is **7.6× worse** because the open
  estimate is computed *from* the applied duty, so every 1.587 % EMC2101 step
  swings it; the closed one is anchored to a measurement and does not move.
  Open loop's smaller *mean* is two model errors partly cancelling — luck, not
  accuracy.
- **The closed-loop estimator's bias is ENTIRELY the R_int error**, which is
  the cleanest confirmation of the observer yet: at its held duty the plant
  measures 1.6387 °C/W, and `(1.709 − 1.6387) × 4.8764 W = +0.3428 °C` against
  a measured **+0.3430**, residual **+0.0002**. Nothing else contributes.
- **ARMS 2 AND 3 ARE A REPLICATE PAIR and they bound everything: 0.67 °C.**
  There is no model compiled in, so arm 3 ran open too. Any smaller difference
  in this run — including the 0.016 °C above — is **below the noise floor** and
  must not be reported as a measurement. It decomposes exactly, and mostly into
  mundane things: R_conv −0.089 °C/W × 4.86 W = −0.43 °C, rail power −16 mW ×
  13.25 °C/W = **−0.21 °C**, EMC2101 air −0.027 = −0.03 °C; sum −0.67 ✓. **A
  third of it is simply that the card drew 16 mW less** — at this total
  resistance, 16 mW is 0.21 °C of die.
- **The closing bracket NEVER SETTLED, and that is the run's verdict on
  itself.** Still falling 0.197 °C across its last 300 s, thirty minutes after
  the load came off, where the first three were flat to within a die LSB. So it
  is not a valid baseline comparison — and declining to read it as one is the
  finding: the session's drift had not finished when the run ended. **Spend the
  30 minutes anyway**; a bracket that fails to settle is the only way to learn
  that.
- **The 8.1 % drift is a FAN-OFF phenomenon ONLY — do not point it at the
  loop.** Natural-convection resistance at the four unforced baselines went
  22.650 → 22.515 → 22.189 → **20.815 °C/W**, monotone, while the loaded arms'
  own R_conv moved just **0.96 %** (11.7278 → 11.7041 → 11.6153). The regime
  the loop actually operates in was stable to ~1 % all afternoon. This was
  first written up as a drift affecting the run and that was an over-read.
- **The EMC2101 moved the WRONG WAY while everything else moved**, +0.272 °C
  across the four brackets. It is the fan controller's own die temperature —
  self-heated, 0.25 °C quantum, on the probe board rather than in the
  airstream — and the feedforward uses it as `air`. With a thermocouple the PI
  absorbs that; open loop has nothing to correct it with. **Closing the loop on
  an estimate does not fix this**: a PI drives `die_est → target`, so a bias
  *upstream* of the estimate leaves the real die off target by exactly that
  bias. A soft sensor corrects everything downstream of itself and nothing
  above it.
- **`ctl.state` 5 vs 6 is the ONLY column separating arm 2 from arm 3.** `src`,
  `tc`, `nn` and `auto` read identically in both (1799 samples each). Had that
  one column not worked, this run would have measured arm 2 three times with no
  way to say so — see the `kOpen` entry under **Known issues**.
- R_int today: **1.6387 / 1.6290 / 1.6297** at 37.7 / 37.6 / 36.5 % duty, i.e.
  a duty span of 1.18 points — one operating point, so this run adds nothing to
  the duty-dependence question below. Throughput: **1372.29 ± 0.42 fps** over
  5 394 loaded samples, 400 MHz flat, **0.00 % throttled**, hottest die
  **90.28 °C** with 4.72 °C of margin to the 95 °C bound.

**MEASURED 2026-11-23, the LOAD PROBE — eight models, 71 minutes, fan pinned at
73 %.** Log `logs/mb-benchmark-20261123-192601.csv`, plan
`config/automation-hailo-load-probe.conf`. This exists because **only ResNet-50
had ever run on this card** — every other model's power was an estimate from
GOP/frame × fps. It is the prerequisite for the soft sensor's W axis, since the
fan ladder characterises `Rconv(duty)` across 59 duty levels but at **one** load.

| model | GOP | fps | **rail W** | TOPS | of 26 | die | tc−air | Rconv |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| OSNet x1.0 | 1.96 | 214.6 | **1.116** | 0.42 | 1.6 % | 30.9 | 7.77 | 6.97 |
| SCRFD-500M | 1.47 | 1302.2 | **3.100** | 1.91 | 7.4 % | 48.5 | 20.76 | 6.70 |
| YOLOv8m | 78.94 | 67.2 | **3.218** | 5.30 | 20.4 % | 48.8 | 20.85 | 6.48 |
| ArcFace | 0.88 | 5195.2 | **3.907** | 4.57 | 17.6 % | 55.5 | 25.95 | 6.64 |
| SCRFD-2.5G | 6.86 | 1059.2 | **4.094** | 7.27 | 27.9 % | 57.2 | 27.24 | 6.65 |
| ResNet-50 | 8.174 | 1372.3 | **4.383** | 11.22 | 43.1 % | 60.3 | 29.64 | 6.76 |
| SCRFD-10G | 26.68 | 441.4 | **5.058** | 11.78 | 45.3 % | 64.9 | 33.16 | 6.56 |
| YOLOv8s | 28.60 | 491.6 | **7.712** | 14.06 | 54.1 % | 85.9 | 48.32 | 6.27 |
| ResNet-50 *(repeat)* | 8.174 | 1372.3 | **4.371** | 11.22 | 43.1 % | 59.5 | 28.97 | 6.63 |

- **THE W SPREAD IS 6.9× — 1.116 to 7.712 W.** The worry this run was launched
  to settle (that eight models would all land in the 4.2–4.9 W band the ladder
  already covered) is dead: models alone give a wide, usable load axis and the
  `fps` cap is not needed to create one. Rail power is read from the **last
  minute** of each tread, never the mean.
- **Five minutes is enough, and the naive check says otherwise — don't use it.**
  A least-squares slope over the last 60 s reads +1.8 °C/min on YOLOv8s and
  flags every tread as unsettled. That is wrong: an exponential fit gives
  τ = **56–82 s** for the heatsink (47–112 s for the die), so 299 s is
  **3.7–5.3 time constants** and the extrapolated shortfall is ≤ **0.91 °C** of
  heatsink and ≤ **0.2 %** of watts. **A decaying exponential still has slope
  while sitting within a degree of its asymptote** — fit the exponential, don't
  regress the tail.
- **Rconv at 73 % duty = 6.63 ± 0.18 °C/W (±2.7 %) across the whole 6.9× load
  range**, computed against each tread's own neighbouring fan-on idles so both
  session drift and EMC2101 self-heating cancel. **The ladder measured 6.746
  °C/W at the same duty a day earlier** — two independent sessions, 1.7 % apart.
  That is the strongest evidence yet that `tc = air + W·Rconv(duty)` is the
  right form.
- **But the linearity is NOT settled, and the honest answer is "the middle
  holds".** Over the seven treads between 3.1 and 5.1 W the through-origin form
  fits to **0.33 °C rms with exponent n = 1.009** — dead linear. Pooling all
  nine pulls it to n = 0.955 with a 1.89 °C residual at YOLOv8s, and pointwise
  Rconv falls −0.092 (°C/W) per W (R² = 0.71). **Drop either endpoint and the
  trend vanishes** (R² = 0.006, n = 1.009), and each endpoint has its own
  defect: OSNet's total rise is only 7.77 °C, so the ±0.25 °C air error is 3 %
  of it, and YOLOv8s is the one tread with a real extrapolation shortfall. **Two
  points cannot establish a curvature.** The fitting run has to put several
  loads across the full 1–8 W range, not rely on model identity to spread them.
- **The replicate pair bounds it: 0.27 % in watts, 0.67 °C in heatsink rise,
  2.0 % in Rconv.** ResNet-50 ran first and last, deliberately, in a scrambled
  power order so session drift could not alias onto load. The scrambling worked
  (power vs tread index is uncorrelated by construction) and the two treads
  agree to −0.0120 W. **Do not read the two treads' 1.10 °C die difference as
  drift** — tread 1 started from a 42.7 °C card (the plan-level fan is applied
  only while a benchmark is active, so the `start` pause ran at 0 %) and was
  *falling* at the end, where tread 9 started cold and was still rising. The
  **power** bracket is valid because power settles fast; the temperature bracket
  is not.
- **The EMC2101 is contaminated by the card, and this run bounds it at ≤ 0.25
  °C.** Raw, it rises 0.077 °C per watt of card power (R² = 0.44) — but most of
  that is the session's own 0.74 °C downward drift masquerading as a power
  effect. Differenced against the neighbouring idles it falls to **0.039 °C/W
  with R² = 0.27**, i.e. ≤ 0.25 °C over the full 6.6 W span, comparable to its
  own scatter. So it is bounded, not measured. It is still the feedforward's
  `air`, and it is still the bias a soft sensor cannot correct (see above).
- **YOLOv8s is the binding load: 86.4 °C die at the fan's ceiling**, 400 MHz
  flat, **0.00 % throttled**, 8.65 °C of margin to the 95 °C bound. Safe — but
  it pins the fan.
- **THE (W, duty) SPACE IS A TRIANGLE, NOT A RECTANGLE**, and this is what
  decides the fitting run's shape. Combining each load with the ladder's
  `Rconv(duty)` at the 95 °C bound, the lowest duty each load survives is:
  OSNet/SCRFD-500M/YOLOv8m **0 %**, ArcFace 9.5 %, SCRFD-2.5G 17.5 %,
  ResNet-50 **27 %**, SCRFD-10G 33.3 %, YOLOv8s **66.7 %**. So "hold a few
  fixed duties and step the load within each" is **not realisable across the
  full range** — the heavy loads exist only at high duty and the low duties only
  carry light loads. A fitting run must either accept a triangular design or use
  the `fps` cap to put a *chosen* wattage at a low duty, which is the one thing
  model-swapping cannot do.
- **Utilisation tracks model size, not model identity — the same pattern the
  Hexagon NSP shows.** 1.6 % of nameplate on a 1.96 GOP model against 54 % on
  28.6 GOP. YOLOv8s's **14.06 TOPS is the highest achieved figure this card has
  ever produced here**, above ResNet-50's 11.22, so the 43 % in the utilisation
  table is not this card's ceiling.
- **The fitting run this sized is `config/automation-hailo-soft-sensor-fit.conf`**
  (4 h 08 m, 24 treads, four duties x a shared cap ladder). Its header carries
  the triangle, the per-duty soak derived from tau scaling with Rconv, and the
  three questions it settles. **Read it before writing any other fixed-duty
  thermal plan** — the two design traps it encodes are that the heavy loads
  exist only at high duty, and that the ladder's own low-duty data is
  *throttled* data and so not a clean plant measurement.

**MEASURED 2026-11-23/24, THE SOFT-SENSOR FITTING RUN — 24 treads, 4 h 08 m,
four fixed duties.** Log `logs/mb-benchmark-20261123-205436.csv`, plan
`config/automation-hailo-soft-sensor-fit.conf`. All 24 treads ran; the duties
read back exactly, and `fan.idle = 73` held through the start pause, which
closed the load probe's one methodological defect and made the drift bracket
valid.

- **The headline is a negative result about the plan's own design**: the fps
  cap moved the rate and not the rail, so the intended 12-point matched-load
  grid became a **replicate** grid. See the `fps` entry above. The 21 % segment
  was capped-only precisely because the cap was supposed to make that duty
  feasible; it ran at ~4.8 W and threw three throttled treads.
- **`Rconv(duty)`, against SIMULTANEOUS air, from the exponential asymptotes:**

  | duty | Rconv (°C/W) | n | tau (s) | C = tau/R |
  | ---: | ---: | ---: | ---: | ---: |
  | 73.00 % | **6.682 ± 0.018** (0.27 %) | 6 | 70.3 | 10.53 |
  | 50.80 % | **9.760 ± 0.107** (1.10 %) | 5 | 102.4 | 10.49 |
  | 33.30 % | **12.384 ± 0.097** (0.78 %) | 5 | 159.6 | 12.89 |
  | 20.60 % | 15.813 | **1** | 176.0 | 11.13 |

  **ResNet-50 treads only** — pooling models here would fold the
  model-identity effect below into what is meant to be the plant's own spread.
  Three independent sessions now agree at 73 %: ladder 6.746, load probe
  6.63 ± 0.18, this run 6.682. These are the figures
  `tools/ra6m5-report-softsensor.py` prints; it is the reproducible
  computation, so prefer it over any number quoted from a scratch script. **FIT THE EXPONENTIAL, DO NOT REGRESS THE
  TAIL** — the load probe's hardest-won lesson, and it is why these are
  asymptotes and not tail means.
- **AIR MUST BE PRICED AT THE SAME DUTY AS THE TREAD.** The EMC2101 is cooled
  by the fan at **−0.0261 °C per duty point** (R² 0.846 against duty; R²
  **0.002** against card watts, so it is not contaminated by the card — it is
  in the airstream). This run's idles all sat at 73 % while its treads ran down
  to 20.6 %, so pricing `Rconv` against neighbouring idles folds a
  duty-dependent bias into the curve worth ~2 % at the bottom of the range. The
  first pass did exactly that and had to be redone. Good news for the model:
  **the board knows duty, so this bias is correctable** — better than the
  three-arm run's verdict that the EMC2101 bias is upstream of the estimate and
  irreducible.
- **`tc − air` is LINEAR IN W THROUGH THE ORIGIN, and the affine offset is
  retired.** The load probe's tempting `c = +1.59 °C` reproduced here at
  +1.62 °C and is an artifact: drop the two YOLO treads and it **collapses to
  +0.213 °C**, with through-origin R = 6.7039 at 0.136 °C rms. The decisive
  argument is that the deviation is **non-monotone in W** — YOLOv8m at 3.21 W
  reads −1.60 %, SCRFD-10G at 5.04 W reads +0.04 %, YOLOv8s at 7.71 W reads
  −3.43 %. Curvature cannot produce that pattern; model identity can.
- **Model identity is real, and it is the YOLO family.** SCRFD-10G matches
  ResNet-50 to **+0.04 %** at 73 % duty and **−0.03 %** at 50.8 % — two
  different models across a 16-19 % load difference, against a 0.55 % noise
  floor. The YOLO pair reads low (YOLOv8s −3.43 %, reproducing its load-probe
  offset in a second session; YOLOv8m −1.60 %). Likely spatial: the
  thermocouple reads one point between the fins, not the mean heatsink
  temperature.
- **Leakage reproduced independently: 17.89 mW/°C** (R² 0.869, n=16) against
  this file's 18.68 — 4.2 % apart.
- **The drift bracket is valid this time and 3x tighter than the load
  probe's.** T1 vs T24, 4 h 01 m apart: W −0.19 %, `tc − air` −0.216 °C,
  Rconv −0.55 %, tau 72 -> 69 s. That 0.55 % is the noise floor every other
  number here inherits.
- **The triangle was right.** ResNet-50 ran clean at 33.3 % (die 93.4 °C,
  1.6 °C margin) and throttled at 20.6 % (104 °C); the envelope predicted a
  floor of 27 %.

**THE SOFT SENSOR IS FITTED (2026-11-24).** `model_estimate()` in
`firmware/mb-probe-ra6m5/Control.h`, which carries the constants and the error
budget; `docs/reports/ra6m5-loop/` scores it per arm.

    tc_ss   = air + W * Rconv(duty)        the steady state
    tau     = Rconv(duty) * kHeatCapacity  the pole, from the same curve
    d tc/dt = (tc_ss - tc) / tau

**It adds exactly ONE parameter to what the feedforward already knew**:
`kHeatCapacity = 10.4 J/°C`, a lumped heatsink capacity, fitted jointly with
the curve. It is the **same table** `invert_curve()` inverts, deliberately —
one table, so the predictor and the observer cannot disagree about the plant.

- **Fitted on 26 094 samples** (the 11-22 ladder's unthrottled region plus the
  11-23 fitting run) to 0.51 °C rms, and **validated on 20 698 held out of the
  fit entirely**: 0.52 °C rms (the 11-22 four-setpoint run) and 0.87 °C (the
  11-23 three-arm run) on their loaded windows, bias +0.28 and −0.02.
- **THE DYNAMICS ARE NOT OPTIONAL — do not "simplify" this to the algebraic
  form.** The static version, `air + W*Rconv(duty)` evaluated fresh each step,
  which is exactly what the open-loop branch's own `die_est` computes, scores
  **rms 7.0-9.0 °C with excursions to 43 and 72 °C**. Same curve, one pole
  added: 0.52-0.87 °C. A memoryless estimate of a 70-165 s plant is wrong by
  the whole of whatever transient is in flight.
- **C IS A CONSTANT.** `tau = R*C` holds to **0.1 %** between 50.8 and 73 %
  duty. The apparent C = 12.8 J/°C at 33.3 % is an artifact of the per-tread
  `tau/R` arithmetic, not a plant property: letting C vary as `(R/R73)^a` made
  the held-out error strictly worse for every a > 0.
- **WHAT IT IS WORTH**, by replaying the arm that ran closed on the real probe
  and substituting the model: the loop commands a settled **37.29 % duty
  against the firmware's own 37.72 %** — 0.43 duty points, ~0.6 °C of die.
  That is the cost of losing the probe.
- **THE ERROR BUDGET IS TWO-SIDED, and the cold side is the one that matters.**
  Model identity is one-sided and safe: the curve is fitted to ResNet-50, which
  has the HIGHEST convective resistance of the four loads measured, so every
  other model reads hot (+0.30 SCRFD-10G, +0.43 YOLOv8m, +2.19 YOLOv8s) and
  over-cools. **Session drift reads cold** — up to **−2.34 °C**, on 15.5 % and
  60.0 % of the two held-out runs' loaded samples. The throttle boundary is
  hysteretic and stochastic between 98.9 and 104.2 °C, as little as 3.9 °C
  above `kAutoTarget`, so −2.3 °C is most of the margin. **Regulate on the
  model at 90 °C, not at `ctl.target auto`.** The real probe earned 95 °C over
  13 247 samples; the estimate has not.
- **13 nodes were tried and REJECTED.** The 9-node table's interpolation error
  is ±1.5 %, 0.72 % rms — worth 0.93 °C at 4.9 W — and four more nodes halve
  it. But held-out error did not improve (the 11-22 run 0.521 -> 0.498, the
  11-23 run 0.873 -> 0.876 with a worse bias), because session drift dominates
  interpolation. Don't re-raise it without a run that moves the drift term.
- **`ctl.die_est` should not be believed with the fan off.** The brackets score
  0.55-2.24 °C, the model's weakest ground: the duty-0 node is natural
  convection measured at **0.86 W only**, and natural convection is not linear
  in ΔT, so one resistance cannot serve both idle and load. It does not matter
  for control — the loop's floor is 9.5 % duty and any real load there throttles
  the card long before the model's error does.

**THE SAME REFIT CORRECTED THE FEEDFORWARD, and this invalidates the old
curve's three lowest nodes.** `curve_rconv()` had taken all nine nodes from the
fan ladder, and **the ladder's bottom treads are THROTTLED**: below 23.8 % duty
the card sits on its thermal limit, so 100 % of those samples are under 400 MHz
and the heatsink reading is pinned by the limit rather than set by the fan.
They measure the throttle, not the plant.

| duty | old | new | provenance of the new value |
| ---: | ---: | ---: | --- |
| 0 | 16.48 | **21.75** | natural convection, the 11-23 run's four fan-off brackets |
| 9.5 | 16.17 | **21.28** | two settled 9.5 % blocks; at its floor the fan turns 262 rpm and buys almost nothing over still air |
| 20.6 | 15.20 | **15.82** | the one unthrottled low-duty tread that exists, n=1 |
| 30.2-73 | — | ≤1.1 % change | the ladder's clean region, lag-corrected by the joint fit |

So the feedforward believed the fan was **half again as effective at its floor
as it is**. Nothing at or above 30.2 % moved by more than 1.1 %, so any run
held in the usual 32-61 % band is unaffected — but a pre-0.7.0 log of a
*light-load* step was using a bad curve. Above 73 % the curve is flat and
verified so: the ladder's 18 levels from 73 to 100 % read 6.73-6.81 with no
trend, agreeing with the tach.

**`ctl.rint` stays at 1.709 — the planned retune to 1.627 is REFUTED.** That
figure came from a hand-driven session and the plan said to adopt it if it
reproduced. It does not: at rest the four held setpoints measure **1.619–1.742
°C/W**, and 1.627 is below the whole range (the hand-run was reading a plant
still in motion, where the fast pole puts the die transiently nearer the
heatsink). 1.709 sits inside the band, which is why the estimator's error flips
sign with fan duty and **never exceeded 0.68 °C** over 2 400 steady-state
samples and 27 °C of die. Related and new: R_int is **not** the constant the
thermocouple report took it for — 1.619 at 32–41 % duty against 1.742 at
58–61 %, i.e. ~0.57 °C of heatsink reading, most likely the probe being cooled
by the airstream. Duty and heatsink temperature are confounded across those
four points, so that run cannot separate them; holding one setpoint at two
loads would. **The 2026-11-23 run confirms the low-duty end and still cannot
separate it**: 1.6290–1.6387 °C/W at 36.5–37.7 % duty, squarely consistent
with the 1.619 measured at 32–41 %, but all three arms held the same setpoint
at the same load so they span 1.18 duty points. The experiment that settles it
is still **one setpoint at two different loads**, and nothing has run it.

**The tach and the FS3000 are NOT loop inputs, and the header comment used to
claim otherwise.** `step()` takes only `Emc2101&, Max31855&, Ina228&`, and of
the EMC2101 it reads only `temp_int`. As *control* inputs they would add
nothing — duty→rpm repeats to **0.4–1.7 %** across visits hours apart, so there
is nothing for a cascade to regulate away.

**Confirmed a second time, independently, 2026-11-23 — and the "airflow as
disturbance feedforward" idea is DEAD.** Across 3.5 hours at load the whole
actuator chain agreed with itself: rpm per duty point 62.76 / 63.13 / 62.75
(+0.59 %, −0.03 %) and m/s per 1 000 rpm 0.8044 / 0.8044 / 0.8155 (−0.00 %,
+1.37 %). **At load the FS3000 carries no information duty does not already
carry**, so there is nothing for a feedforward term to tell the loop in the
regime the loop runs in. It showed a change only in the *fan-off* brackets
(median 0.005 → 0.010 m/s), which is 0.3 % of the 1.9 m/s it reads under load,
i.e. its noise floor — the one place its reading is least trustworthy. It is
tempting, after seeing the fan-off resistance move 8 %, to reach for the air
sensor as a control input; the load-side numbers say don't. Their value is fault detection:
discovering `max_duty` live instead of hard-coding 73.0 (the tach already shows
73 % and 100 % landing within 1 rpm), and catching a stalled or fouled fan.
The FS3000 would catch the one failure the tach structurally cannot — a blocked
inlet where the fan spins perfectly and no air moves. Neither is wired in yet.
Note the tach reads **272 ± 118 rpm (43 % spread)** at the 9.5 % floor, so any
rule has to be gated above it.

**What the board cannot see is the core clock and the frame rate** — those are
PCIe-side, host-only. So the loop holds a die temperature *believed* safe; it
cannot detect throttling and react. Closing that would need the host in the
loop, which is what running it on the board exists to avoid. The host still
logs `_CLK`, so any setpoint can be audited after the run.

Host side, in `Probes.cpp` — things that are deliberate:
- **One board is TWO `DeviceProbe`s sharing one `ra6m5::Link`.**
  `RA6M5InaProbe` mirrors `INA228Probe` exactly (name scheme, colour alias, the
  same six families and labels), so the existing fold makes it
  `<Card> INA228 <FAMILY>` and every consumer — CSV, plotter, Telemetry ->
  INA228 switch, `power_for_device()` — treats it as just another shunt, with
  no change. `RA6M5BoardProbe` ("RA6M5") carries what belongs to **no card**:
  airflow now, fan and thermocouple later. Folding those would mislabel them.
- **The fold names a family by its suffix alone**, so a folded probe can carry
  only ONE metric per family: a second (say `vbus_min`, or a thermocouple in
  the temperature family) would collide with `<Card> INA228 VBUS/TEMP`. That is
  why the min/max the firmware reports are not on the host yet, and why the
  thermocouple must go on the board probe. Fixing it means teaching `emit()` a
  per-metric suffix — do that deliberately, since it renames CSV columns.
- **Mapped in `ina228.conf` by USB serial number**, not port-path — the board
  has one (the RA6M5 unique id). The board's startup note prints the serial
  and, when unmapped, the exact line to add; that note is why the board probe
  is always added even with no airflow sensor.
- **`reset_acc` is sent at discovery**, matching the FT232H path's RSTACC
  pulse; otherwise energy starts at whatever the board integrated since it was
  flashed (249 J the first time it was tested).
- **The firmware is the only reader of `DIAG_ALRT`**, so the undervoltage note
  counts every flagged conversion rather than one sampled latch per second. The
  host counts `ina228.uv` once per D line (`seq`), because the host tick and
  the firmware period are not phase-locked and one line can be seen twice.
- **No reconnect.** The metric list is fixed at discovery; an unplugged or
  re-flashed board goes stale after 3 s and every value becomes a gap, with one
  transition note. Restart the app after flashing.
- Unmapped shunts number `INA228#<n>` across both carriers, from one counter.
- Two INA228s mapped to the **same** card (an FT232H and an RA6M5) would emit
  duplicate CSV column names. Not guarded; don't map both.

## API modes (Sync / Async)

**The mode is per card, chosen on that card's row — not once for the whole run.**
`BenchItem::api_mode` carries it (`enum class ApiMode` lives in `Catalog.h`, not
`Bench.h`, precisely so `BenchItem` can hold it); `BenchEngine::start()` takes no
mode argument and each `Worker` uses its own item's. A run may therefore mix
modes, which is legitimate but is **not** a like-for-like comparison — the status
line says "Mixed API modes" when enabled cards disagree.

The five SDKs are not symmetric. Verified 2026-08-04 against installed headers
**and** `nm -D` on the shipped libraries, not assumed:

| | Sync | Async |
| --- | --- | --- |
| Hailo | `InferVStreams::infer()` | `ConfiguredInferModel::run_async()` + `AsyncInferJob` |
| DeepX | `InferenceEngine::Run()` | `RunAsync()` → jobId → `Wait()` |
| MemryX | **`MxAcclMT::run()`** | `MxAccl::connect_stream()` |
| Axelera | `axr_run_model_instance()` | **1.8 only, and internal** — `axr_run_async_model_instance()` |
| Qualcomm | `QnnGraph_execute()` | **none exists** |

`accel_has_both_api_modes()` drives the UI: Hailo, DeepX, MemryX **and now
Axelera** get radios; **Qualcomm alone** gets a static line from
`accel_sole_api_mode_note()`.

**The starting mode is per card, from `accel_default_api_mode()`.** Async on
the four cards where it is the faster path; **Sync on Axelera**, because its
async executor refuses a multi-kernel artifact and so runs the batch-1 build at
~1/5 the rate (388 fps against 1967 for 4-core batched sync). A default that
quietly costs 5x is not a default. The switch is exhaustive and has no
`default:`, so `-Wswitch` catches a new card; `ControlPanel` calls
`refresh_depth_sensitivity()` once at construction so Axelera's depth spin
starts greyed and its double-buffering radios start live. The shipped
automation plans deliberately do not set `axelera.api`, so they inherit this.

**Axelera's radios are offered, and since 2026-09-20 its async mode actually
runs** (387.7 fps on ResNet-50 batch 1; it was the property separator, not the
SDK). Offering them was deliberate even while it appeared broken — the same principle as the Qualcomm
`Backend` control: the entry point genuinely exists in the SDK, the failure is
loud and names its own cause, and hiding it would make the UI disagree with the
runtime. `api_mode_note(Axelera, Async)` now says
`1.8 internal API · batch-1 build only · slower than sync`, because a mode that
works but cannot use the 4-core artifact would otherwise read as the faster
one. `api_mode_is_native()` still reports **Sync only**
for it: "native" there means *the vendor supports it*, which async is not yet. Neither switch has a
`default:`, so `-Wswitch` catches a newly added `Accel` — unlike `make_runner()`,
which does have one and will not warn.

- **MemryX has both modes natively — the SDK, not the class.** An earlier version
  of this file recorded "MemryX: none exists" for Sync and emulated it with a
  permit counter at depth 1. That was wrong: `MxAccl` has no blocking call, but
  **`MxAcclMT`** — same `MxAcclBase` parent, same constructor shape — provides
  `run(in, out, model_id, stream_id, timeout)`, which blocks until the frame
  completes. `bench_memryx.cpp` now has `MemryXSyncRunner` (MxAcclMT) alongside
  `MemryXAsyncRunner` (MxAccl streams), the way `bench_hailo.cpp` has always been
  split. Three things to know about it:
  - **`MxAcclMT` has no `start()`/`stop()`** — construct, `run()`, destruct. Do
    not go looking for the symmetric call; `MxAccl::stop()` is separately a
    **deprecated no-op** in 2.2.5 (it links and does nothing).
  - **The timeout must be finite.** `timeout = 0` blocks forever and the engine
    only tests `stop_flag` *between* frames, so an infinite wait on a wedged MX3
    would strand the worker. `kRunTimeoutMs` is 10 s and a timeout throws.
  - The SDK documents `run()` as a synchronous *facade* over an async internal
    pipeline, and the class serialises on a private mutex — so it is honest
    one-frame-at-a-time throughput, but not a clean single-frame latency probe.
  - **`local_mode` is taken now, and it buys latency, not throughput.**
    `MxAccl`/`MxAcclMT`/`MxAcclBase` all take `bool local_mode = false` (5th
    positional arg) and `mx_bench --help` advertises its default as "Local mode
    for peak FPS", which makes the manager daemon look like the obvious suspect
    for any MemryX shortfall. It is not: measured 2026-09-04 on ResNet-50 over
    30 000 frames, `mx_bench` reports **1796.10 fps local against 1796.11
    shared** — identical throughput; only latency differs (3.34 vs 3.69 ms).
    **This file previously said local mode was not worth taking. It is taken
    anyway**, because the latency is free and because a run that reaches the
    device the shortest way is the one to compare against `mx_bench`. The
    mechanism is `make_accl<>()` in `bench_memryx.cpp`:
    - local is tried first; `$MB_MEMRYX_SHARED=1` forces the old behaviour;
    - local needs **exclusive access**, so with `mxa-manager` holding the device
      the constructor throws `Error in client->try_local_lock for device id: 0`
      — our own MemryX telemetry helper is a second client on that card, so
      this happens routinely;
    - the fallback to shared is therefore expected, and it is **visible**: the
      string `shared (local refused: …)` goes into `describe()` and so into the
      CSV's `memryx_cfg`. A run that quietly changed how it reached the device
      is one nobody can read back later. Don't "tidy" that string away.

    It does **not** help the depth-8 wedge — both modes bottom out in the same
    `libmemx` / `/dev/memx0` path. **The MemryX shortfall was never the daemon —
    it was our permit depth.**
- **Measured after the split** (ResNet-50, this host): Hailo 298.1 / 1217.2,
  DeepX 384.0 / 1085.5, MemryX **259.9** / **1796.7** (sync / async). The MemryX sync
  figure is new — the old emulated one is not comparable and should not be quoted
  against it. **1796.7 is depth 8, and depth 8 is no longer the default** — it
  wedges the card (see the depth table below). The shipped default of 4
  measures ~1077 fps; quote whichever you like, but always with its depth.
- **Axelera had no async API through 1.7, and 1.8's is marked internal.**
  **Voyager 1.8 adds `axr_run_async_model_instance()`** — a real
  completion-callback path, the first this API has ever had
  (`AXR_API_VERSION` 3.1 → 3.2, symbols 40 → 44; the other three are
  `axr_{read,write,remove}_saved_config`). The header says
  *"INTERNAL USE ONLY ... experimental and subject to change or removal without
  notice - it is not yet part of the stable public ABI"*, and it appears in no
  release note, no `docs/`, no `examples/` and not in the C++ wrapper. So
  `bench_axelera.cpp` implements it behind `$MB_AXELERA_ASYNC`, **off by
  default**, compiled out entirely below 3.2 via `MB_AXELERA_HAVE_ASYNC`.

  **MEASURED 2026-09-20: it WORKS — 387.7 fps on ResNet-50 batch 1, through
  the app's own runner.** An earlier pass the same day concluded "every
  submission returns `AXR_ERROR_NOT_IMPLEMENTED`"; that was **our property
  string**, not the SDK (see **Instance properties** below — `;` meant
  `async_mode` never reached the runtime). With one property per line the
  runtime logs `AsyncExecutor initialised`, `io_buffer_depth=4, queue_depth=8`
  and four lanes retiring frames.

  **Its one hard limit is the artifact.** `AsyncExecutor::init()` refuses
  anything but a single-kernel model — `AsyncExecutor requires exactly one
  kernel at stage 0` — and an N-core build ships N of them
  (`resnet50-…-4core` has `kernel_function_0..3.elf`). It does not degrade, it
  **fails the load outright**, so `bench_axelera.cpp` forces the batch-1 build
  in Async and says `async needs the batch-1 build; ignoring N cores` in
  `describe()`. Async and the multi-core batched builds are therefore
  **mutually exclusive in 1.8.0**, which makes async the slower mode on this
  card: 387.7 fps against 1967 for 4-core sync.

  Two properties the vendor's own `AxInferenceNet` sets and we now set too:
  **`max_inflight`** (lanes, default 4) and **`max_pending`** (queue depth,
  default 8). They are only read when `async_mode` is set.

  What the library contains, from the 1.8.0 disassembly — kept because it is
  what made the diagnosis possible:
  - `axr::AsyncExecutor::run_async` is a **real implementation** (`T`), while
    `axr::Executor::run_async` is a **weak symbol** (`W`) — the base returning
    NOT_IMPLEMENTED. Landing on the base is the symptom of `async_mode` not
    having been applied, **not** proof that the C API cannot build an
    `AsyncExecutor`: `axr_load_model_instance` calls
    `axr::create_async_executor()` directly, gated on the parsed `async_mode`
    flag and on the model type being `default`.
  - **`async_mode` is a genuine property key**, new in 1.8 and absent from 1.7.
    It is *accepted* at load (an invalid key fails with `Unknown property key`,
    this one does not) and gated by `async_mode only supports model type
    'default'` — our models **are** type `default`, so that gate passes. It
    still does not switch the executor.
  - Dropping `double_buffer` (the `LockstepExecutor`) makes no difference
    either. That guess was tried and reverted.

  **The lesson is the diagnosis, not the verdict.** "The symbol is weak,
  therefore the feature is unimplemented" was wrong twice over: the strong
  implementation was in the same library, and the one thing never checked was
  whether our own property string had been parsed at all. When an SDK reports
  a feature missing, confirm the input reached it before concluding anything
  about the vendor.

  What follows was true of 1.7 and is kept because the reasoning still applies
  to the *stable* API: all 40 `axr_*` symbols in libaxruntime
  1.7.0 enumerated (`nm -D --defined-only`); exactly one is inference and it
  blocks. The undocumented export `axr_run_graph_model_instance` is not a second
  one — it is not asynchronous. The 233 `ze*` symbols are the statically linked
  oneAPI Level Zero loader — headers, pkg-config and CMake are all shipped, but it
  is model-agnostic, so driving inference through it means reimplementing
  `axr_load_model` against internal types. "Async" there sets the runtime's own
  `double_buffer=1` property, worth ~7-18%. **Don't label host-thread fan-out
  as "Async"** — the API has no async mode and the run column would claim one.
  That is a *naming* rule. It was once mis-applied here as a ban on ever using
  threads for throughput, and it is not one: Qualcomm does exactly that fan-out
  on a sync-only API and this file calls it honest. On Axelera it is a
  thread-safety problem, not a naming one — see **concurrent submission** below,
  where it is measured.
- **Qualcomm's "none exists" could not be re-verified here.** It was checked on
  the aarch64 IQ-9075; the QNN headers are absent on x86_64, where the backend is
  not even compiled. The shim contains no `executeAsync` reference anywhere,
  which is as far as an x86 host can confirm it.

- **Axelera concurrency is AIPU cores, not async and not host threads.** A model
  claims cores via `num_sub_devices` on `axr_device_connect()` — that is the
  card's real concurrency primitive, and it is what the vendor SDK and the
  sibling `envic_ai_aerex` pipeline both use (one connection per model, cores
  sized from the model's L2 footprint). The Frequency graph is the ground truth:
  a model on one core leaves `aicore1..3` parked at 50 MHz while `aicore0` runs
  at 800.

  **This was originally implemented backwards** (2026-08-04). The UI exposed
  "Streams" — host threads — and *derived* the core count from it
  (`avail / (nstages * streams)`), so asking for 4 streams asked for 1 core each
  across **four separate `axr_device_connect()` calls**. Every connect makes the
  runtime reload the card's firmware (`fwtrace: detached for firmware reload` in
  dmesg), so a 4-stream run fired three or four reloads in seconds; on a card
  still in bootloader that raced the 3.8 MB firmware ELF upload and took the
  PCIe link down. The control is now **AIPU cores (1-4)** — an honest name for
  what it delivers — and the ordering of the connects is what was actually
  wrong, not their number. **It defaulted to 2 while this paragraph was
  written, one step below the cliff; the fixed-batch path (2026-09-03) raised
  the default to 4**, which is what `ControlPanel.cpp` sets
  (`Adjustment::create(4, 1, 4, 1, 1)`) and what the invariant above states.
  Automation's `axelera.cores = 4` is therefore both legal and the default. Teardown must stop *and join* any instance
  threads before destroying an instance, and release `Stage::conn` only after
  every instance on it is gone.

  **The "one instance occupies exactly ONE AI core" rule was an artefact of a
  missing property, and is retired.** Measured 2026-08-04 on ResNet-50 (async),
  clocks sampled *mid-run*:

  | connections × instances | `num_sub_devices` | aicore0..3 during run | fps |
  | --- | --- | --- | --- |
  | 1 × 1 | 4 | `800 · 50 · 50 · 50` | 375.6 |
  | 1 × 4 | 4 | — (`Failed to wait for MSI`) | 201.7 |
  | 4 × 1 | 1 each | `800 · 800 · 800 · 800` | 694.9 |

  The first row is why we concluded a connection could not spread one instance
  across cores. **It was not passing `aipu_cores`.** `axr_device_connect()` says
  how many sub-devices the *connection* reserves; `aipu_cores`, an
  `axr_load_model_instance()` property, says how many the *instance* may use.
  Set only the first and three cores really do sit at 50 MHz. The SDK's own
  `examples/axruntime/axruntime_example.cpp` sets both:

  ```
  input_dmabuf=0;num_sub_devices=N;aipu_cores=N
  ```

  With both set — and the matching fixed-batch artifact — one connection and one
  instance drive all four cores. See the corrected table below. What follows is
  the superseded arrangement (`instances_ = cores_`)
  and gives every instance its own connection. "One sub-device each" holds for
  models whose constants fit on one core — every vendor prebuilt we ship — but
  is not universal; the next paragraph is what decides it.

  **`num_sub_devices` is sized from the model's L2 constants, not chosen.**
  Voyager's rule is ~1.5 MB of L2 per AIPU core, read from `model.json`'s
  `memory_pools` — nothing in libaxruntime reports it. `bench_axelera.cpp` has a
  minimal scanner for it (same spirit as Catalog's INI reader; a JSON library
  would not pay for three fields).

  **It is applied only when the model actually fits in the part's 6 MB.** The
  vendor prebuilts do not: ResNet-50 declares 7.97 MB of L2 and YOLOv8m 8.01 MB,
  and both load and run on a *single* sub-device — so those pools are paged and
  the arithmetic says nothing about them. Taking it literally would demand 4+
  cores each and collapse ResNet-50 from four instances to one, throwing away
  half its measured throughput. Our locally compiled ArcFace (3.55 MB → 3 cores)
  does fit and matches Voyager's own guidance exactly, and gets 3 sub-devices.
  So: **a big declared L2 means "paged", not "needs more cores"** — don't
  "fix" the runner by removing the fits-in-L2 gate.

  **Every instance loads its own copy of the DDR constants**, so N instances
  cost N × that: YOLOv8m carries 58 MB against ResNet-50's 20 MB, i.e. 234 MB at
  four instances versus 79 MB. `axrDeviceInfo::max_memory` is documented as
  always 0 in this runtime, so the budget cannot be queried and is **not guessed
  at** — the footprint is always reported in `describe()` (and so lands in the
  CSV's `axelera_cfg`), and `$MB_AXELERA_DDR_BUDGET_MB` enables an actual cap.

  **The prebuilt zip ships four compilations of every model — 1, 2, 3 and 4
  cores — and the N-core build is a genuine fixed-batch-N artifact.**
  `resnet50-imagenet-onnx-4core/model.json` declares input `[4,230,240,4]` and
  output `[4,1,1,1024]`: **one invocation retires four frames.** The compiler
  also moves constants from DDR into L2 as the count rises:

  | build | kernel_function*.c | L2 const | DDR const |
  | --- | --- | --- | --- |
  | 1-core | 1 | 7.6 MB | 20 MB |
  | 2-core | 2 | 16 MB | 12 MB |
  | 3-core | 3 | 23 MB | 4.3 MB |
  | 4-core | 4 | 28 MB | none |

  **This file used to say the multicore builds were worse for throughput. That
  was a counting error, now corrected.** The old comparison recorded the 2-core
  build at "317.6 fps" against two batch-1 instances at 574.0 — but 317.6 was
  *invocations* per second, and each retires 2 frames. It was 635 fps, and it
  won. `run_frame()` now returns the frames retired so the engine counts frames
  rather than calls.

  **Measured 2026-09-03, ResNet-50, one connection + one instance per build,
  depth 2** (`cores` selects the `<dir>-Ncore` artifact):

  | cores / batch | 1 | 2 | 3 | 4 |
  | --- | ---: | ---: | ---: | ---: |
  | invocations/s | 359 | 316 | 385 | 400 |
  | **fps** | **359.3** | **631.6** | **1153.9** | **1601.5** |
  | old multi-instance fps | 358.7 | 574.0 | 658.6 | 694.9 |

  **2.3x at four cores.** Note the invocation rate barely moves: a batch-4 call
  costs about what a batch-1 call costs (2.50 ms against 2.78 ms), because the
  four cores work the batch in parallel — which is what the artifact is *for*.
  That also makes the speedup slightly superlinear against one core (4.46x), as
  per-invocation overhead is amortised across four frames.

  **Only ResNet-50 has multicore builds on this host, and they were extracted by
  hand.** `strip = build/%m/%m/1` still fetches the 1-core build only, so a fresh
  checkout has no `-Ncore` siblings and every model falls back to batch 1 — with
  a visible `no N-core build; using batch 1` note, never a silent substitution.
  Fetching all four subtrees would need a new `Fetcher` mechanism; `strip` maps
  one subtree to one directory.

  **The vendor's 2054 fps on ResNet-50 remains unexplained**, and it is a real
  measurement — though see **Work per frame** for the scale of the gap: even
  2054 fps is only 7.8% of the Metis's rated 214 TOPS, so this is not a tuning
  shortfall: it comes from Voyager's `End2EndTracer`, which reports
  `1 / mean(inter-frame interval)` with **no multiplier** (unlike `AipuTracer` /
  `HostTracer`, which do multiply by core count — don't confuse the two, they
  live in different modules with the same class names). **Batching and the
  multi-core builds were previously listed here as ruled out. They were not —
  they were most of the gap**: the fixed-batch-4 artifact took us from 695 to
  1602 fps, i.e. from 34% of the vendor's figure to 78%. Still ruled out: a
  second API.

  **The dmabuf lead is now tested, and it is worth nothing.** This file listed
  `--enable-opencl` / `input_dmabuf` / `output_dmabuf` as "the untested
  difference" for a month. Measured 2026-09-14, ResNet-50, batch-4 artifact,
  four cores: `input_dmabuf=1` gives **1597.8 fps** against **1599.6** with
  plain host pointers — inside run-to-run noise. The buffers were real, not a
  shim (memfd → `F_SEAL_SHRINK` → `UDMABUF_CREATE`, a genuine dmabuf fd), so
  this measures the property and not a failed setup. **Do not re-raise it.**

  **RETIRED 2026-09-20 — ~1600 fps was double buffering being thrown away,
  not a ceiling.** With the property string fixed the same single-threaded path
  measures **1967 fps**, i.e. 96% of the vendor's 2054, with no MSI timeouts
  and one submitting thread. The arithmetic below is kept because the *method*
  is right and it is how the gap was framed; the numbers in it are pre-fix.

  **What was thought to remain is a Little's-law ceiling.** `frames_in_flight = throughput × latency`. Our
  runner: 1598.5 fps × 2.501 ms = **4.0 frames** — exactly the batch size,
  because `axr_run_model_instance()` blocks and there is no async API to
  overlap with. Voyager's own inference element: 2160.8 fps × 9.713 ms =
  **21.0 frames**. It is not faster per call; it has twenty-one frames resident
  where we have four.

  The obvious answer — a second instance — is closed by L2. The batch-4
  artifact is **28.34 MB of the part's ~32.47 MB**, and a second
  `axr_load_model_instance()` fails outright:
  `Not enough memory: free memory 4130304, request memory 28342784`. Verified
  end to end by running **the vendor pipeline's own artifact through our path**:
  1600.9 fps against our 1598.5. Same card, same ceiling. **The rest of the
  vendor's gap is the GStreamer pipeline around the runtime, not the runtime** —
  so stop looking for a property we are failing to set. (There are 40 `axr_*`
  symbols and one undocumented export, `axr_run_graph_model_instance`; it is not
  an async entry point.)

  **Concurrent submission into one instance: it works intermittently, and
  ~1600 fps is NOT the ceiling.** Little's law points at host threads — more
  callers, more frames resident — and extra submitting threads need **no L2 at
  all**, unlike a second instance. Measured on libaxruntime 1.7.0 via
  `$MB_AXELERA_SUBMIT_THREADS` (`bench_axelera.cpp`), ResNet-50 batch-4:

  | submit threads | fps |
  | --- | --- |
  | 1 | 1598.0, 1598.7, 1601.3, 1621.0, 1625.6 — tight |
  | 2 | **bimodal**: 1565–1643 most of the time — or **2037.3 / 2046.3 / 2051.7 / 2058.3** |
  | 4 | **run fails** |

  ```
  [ERROR][axeCommandQueueExecuteCommandLists]: All command lists must be
          closed before execution. Close command list 0 first.
          AXR_ERROR_RUNTIME_ERROR
  ```

  **The fast mode is real work, confirmed against the INA228** — this was
  checked precisely because a shared command list could plausibly return success
  without running, and the frame counter alone cannot tell. Idle is 2.34 W;
  subtract it and efficiency is constant across every run:

  | threads | fps | rail | dynamic | fps per dynamic W |
  | --- | ---: | ---: | ---: | ---: |
  | 1 | 1621.0 | 4.67 W | 2.33 W | 696 |
  | 2 (fast) | **2037.3** | **5.17 W** | **2.83 W** | **720** |
  | 2 (slow) | 1599.3 | 4.68 W | 2.34 W | 683 |
  | 2 (slow) | 1643.0 | 4.71 W | 2.37 W | 693 |

  683-720 across the board, within 5%. Fabricated completions cost no silicon,
  so the fast run's fps/W would be ~25% high; it is not. The card really did
  retire 2037 fps and drew 21% more dynamic power to do it. **That lands on the
  vendor's own 2054 fps**, which suggests the Voyager pipeline submits
  concurrently too, with whatever synchronisation makes it safe.

  **But it is a race, and the knob is clamped to 1 — off.** Two threads win
  maybe a third of the time, and the fast mode **disappears under any
  observation**: running the INA228 sampler alongside suppressed it, and so did
  a `npu-status.sh` call between runs.

  **Why two threads sometimes interleave cleanly is unknown, and the obvious
  theory is disproved.** `double_buffer` supplying a second command list was the
  candidate — the runtime's own log calls it "depth=2" — so it was tested with
  five interleaved on/off pairs. The single fast run came with double buffering
  **OFF**. Don't re-run this; it is settled:

  | | 2 submit threads, 5 runs each |
  | --- | --- |
  | double buffer ON | 1586.3 · 1600.3 · 1736.0 · 1567.3 · 1568.3 |
  | double buffer OFF | 1566.0 · 1565.0 · 1598.3 · 1598.7 · **2051.7** |

  **What decided the clamp is not throughput, it is `Failed to wait for MSI`.**
  Every 2-thread run logs one at teardown — 10 of 10, both `double_buffer`
  settings — and a 1-thread run logs **none**:

  | | fps | MSI timeouts |
  | --- | ---: | :---: |
  | 1 submit thread | 1599.3, 1600.0 | **0** |
  | 2 submit threads | all 10 runs | **1, every time** |

  That message is this card's documented precursor to a PCIe link drop and a
  power-cycle recovery (see **Known issues**). No run has actually dropped the
  link — 0 DMA errors throughout, card healthy after every sweep — but shipping
  a path that trips the precursor on every single run, to chase a gain that
  cannot be explained or reproduced on demand, is a bad trade. `4` threads fail
  outright with the command-list error above, which is the safe failure.

  So `MB_AXELERA_SUBMIT_THREADS` is clamped to 1 and cannot turn this on;
  raising it is a deliberate source edit next to the evidence.
  `describe()` still reports `· N submit threads` if it is ever raised, so a run
  cannot claim a configuration it did not get.

  **The finding that matters: ~1600 fps is not the card's ceiling.** It does
  ~2050 — the vendor's own published figure is 2054 — and single-threaded
  submission is simply the only *safe* way we currently have to drive it.

  For context on why more *instances* is not the answer: they fit only by
  dropping to smaller builds, and the batched builds are superlinear —
  1 x batch-4 (28.3 MB L2) measures 1602 fps against 4 x batch-1 (30.4 MB)
  at 695.

  The cold-card race is handled by *ordering*, not by avoiding connections: the
  **first** connect is made alone and allowed to complete (it is the one that
  uploads the 3.8 MB firmware ELF), and only then are the rest opened.
- **MemryX's clock knob is Python-only.** `set_mpu_frequency(dev, group, MHz)`
  exists in the `mxa` module and nowhere in `memx.h` or `MxAccl`, so
  `configure()` shells out to the venv interpreter once at load. ~4 s of Python
  startup, alongside model loading, never in the timed loop. The interpreter is
  `$MB_MEMRYX_PYTHON`, else the conventional
  `$HOME/mb-edgeai/memryx-env/bin/python`; finding neither makes the knob a
  silent no-op, deliberately — the Frequency graph then shows the clock did not
  move, which beats failing a run over a setting.
- **Qualcomm concurrency is NSPs first, engines-per-NSP second — and both are
  real.** QCS9075 has **two** Hexagon NSPs, each with its own 8 MB VTCM, and they
  are QNN **device ids**, not core ids. Measured 2026-08-04 on OSNet x1.0, burst,
  through the app's own headless driver:

  | | 1 NSP | 2 NSPs |
  | --- | ---: | ---: |
  | Sync (depth 1) | 1618 fps | **3546** (2.19x) |
  | Async depth 4 | 2313 (1.43x) | **4581** |
  | Async depth 8 | — | 4502 (saturated) |

  So NSPs are the large knob and async depth a smaller host-side overlap win that
  stacks on top of it. Two consequences for the design: the **`NSPs` control
  defaults to 2** (unlike Axelera's cores under the *old* multi-connection path
  there is no cliff here — nothing wedges), and
  **Async is honest here even though QNN has no async API** — it is extra engines
  in flight per NSP on host threads, and `api_mode_note()` says exactly that.
  Depth 8 regressing is why the shared default of 4 is right for this card too.

  **Selecting the second NSP takes a device *config*, not an id you pass to your
  own loader.** `QnnDevice_create` with a null config always instantiates
  hardware device 0. Asking for `core_id = 1` instead fails with "core type for
  device id 0 core id 1 not found in platform info" — and in both wrong forms
  **the model still loads and runs**, on the wrong NSP and without its
  performance mode. Nothing fails loudly. The shim handles this; don't rewrite it
  casually.

  **Enumerate NSPs from `qnn_backend_device_count()`, never from
  `/dev/fastrpc-*`.** The kernel exposes a node per FastRPC domain — this SoC has
  `cdsp`, `cdsp1`, `gdsp0`, `gdsp1` — whether or not the HTP backend can drive
  it, so counting nodes over-reports by 2x.
- **Qualcomm performance mode is not a knob you may leave unstated.** The HTP
  runs under DCVS and the spread is ~1.8x — measured on OSNet across two NSPs:
  burst 3381, `sustained_high_performance` 2818, balanced 2384, power_saver 1886.
  `describe()` therefore always carries the mode, and the default is an explicit
  `burst` rather than "leave the governor alone" (a short inference can otherwise
  finish before the governor ramps). Use `sustained_high_performance` for
  steady-state comparisons: burst thermally throttles on this passively-cooled
  board.
- **The Qualcomm `Backend` control cannot work with the shipped artifacts, and
  that is expected.** The shim can dlopen `libQnnHtp.so` / `libQnnGpu.so` /
  `libQnnCpu.so`, but a *context binary* is compiled for one backend. Measured
  here: the GPU rejects an HTP `.bin` with `GPU_ERROR_INVALID_VERSION` /
  "Context deserialization verification failure", and `libQnnCpu.so` reports **no
  devices at all**. The control is still offered — the failure is loud and names
  the real cause, and a GPU- or CPU-compiled artifact dropped into the models tree
  would then just work without a code change. Don't "fix" it by hiding the entries
  or by faking a fallback.
- **`run_frame()` must retire exactly one frame in both modes.** An async runner
  tops its pipeline up and *then* blocks for one completion, so the engine's
  frame counter needs no special case. Keep that contract.
- **Depth is per card, on that card's row** (`BenchItem::depth`, was a single
  global "Threads" control). Every backend has some form of frames-in-flight, but
  the mechanism and the useful range differ, so the range is per card:

  | card | what depth sets | range | gated on API mode? |
  | --- | --- | --- | --- |
  | Hailo | requested async queue depth, `min(requested, get_async_queue_size())` | 1-8 | yes — Sync forces 1 |
  | DeepX | outstanding `RunAsync` jobs | 1-8 | yes — Sync forces 1 |
  | MemryX | `connect_stream` permit depth | 1-8, **default 4** (was 8; see the wedge below) | yes — Sync uses `MxAcclMT::run()` |
  | Axelera | **both, and the API mode picks which**: `double_buffer` (Off/On radios) in Sync, async frames-in-flight (spin) in Async | Off/On · 1-8 | **yes**, since 1.8 |
  | Qualcomm | engines in flight per NSP, **one host thread each** | 1-8 | **no** |

  **Depth is frames-in-flight everywhere, and threads on Qualcomm alone.**
  Measured 2026-09-13 by counting `/proc/<pid>/task` across a run: Hailo **6**,
  DeepX **13**, MemryX **21** — *fixed*, the same count at depth 1 as at
  depth 8. Those SDKs size their own worker pools and depth only changes how
  many frames the app keeps outstanding inside them. Qualcomm is the one
  backend that creates threads from *the depth control* (`nsps × depth`). So
  **never describe depth as a thread count** in the UI, in `describe()`, or in
  the CSV — it is one on exactly one card out of five.

  **Two SDKs do expose a settable thread pool, and neither is worth a control.**
  An earlier version of this paragraph said Qualcomm was "the only place a
  separate Threads parameter would mean anything". That was wrong — it came
  from grepping the wrong headers, the same mistake this file already records
  twice for Hailo clocks:
  - **MemryX** — `MxAccl::set_num_workers(in, out, model_id)` and
    `MxAcclBase::set_parallel_fmap_convert(n, model_id)`. Both must be called
    **after** `connect_stream()` and **before** `start()` (`set_num_workers`
    documents that as a `@pre`). The default worker count is *the number of
    connected streams*, and we connect one.
  - **DeepX** — `NFH_INPUT_WORKER_THREADS` / `NFH_OUTPUT_WORKER_THREADS`, read
    from the **environment**, not a C++ setter (`dxrt/common.h:192`,
    `GetNfhInputWorkerThreads()`). Verified by calling the accessors: defaults
    **2 in / 4 out**, valid **1-64**, anything outside logs
    `Invalid NFH_INPUT_WORKER_THREADS value, using default=` and is ignored.
    Being an env var, it must be set on the **main thread before any worker
    exists** — `setenv` races `getenv` elsewhere, the same constraint that puts
    `prepare_backend_environment()` where it is.

  **Measured on MemryX 2026-09-14, and the answer is no.** ResNet-50, depth 4,
  local mode, against a baseline whose run-to-run spread is 0.13%
  (1151.2 / 1149.7 / 1150.4 fps):

  | setting | threads | fps | vs baseline |
  | --- | ---: | ---: | ---: |
  | default (workers = stream count) | 10 | **1171.1** | — |
  | `set_num_workers(2,2)` | 12 | 1150.3 | -1.8% |
  | `set_num_workers(4,4)` | 16 | 1132.7 | -3.3% |
  | `set_parallel_fmap_convert(4)` | 13 | 1094.0 | -6.6% |
  | `set_parallel_fmap_convert(2)` | 12 | 1070.7 | -8.6% |

  Both setters are real — the thread count moves exactly as asked — and both
  **cost** throughput. `MB_MEMRYX_WORKERS` / `MB_MEMRYX_FMAP` in
  `bench_memryx.cpp` keep the experiment reproducible; unset means the SDK
  default, so the shipped path is unchanged.

  **The reason is the one worth keeping: at depth 4 this app is nowhere near
  host-bound.** Per-thread CPU during a depth-4 run — busiest thread **19% of
  one core**, whole process **75% of one core**, on a 128-core host. There is
  no host-side constraint for extra threads to relieve, so they only add
  contention; the MemryX header says as much
  ("may even degrade performance due to increased CPU load"). It also settles
  the question **without risking a depth-8 wedge**: per-frame host work scales
  with rate, so at depth 8's 1796 fps the busiest thread lands near 29% of a
  core — still idle even if that estimate is off by 2x. The 1077 -> 1796 fps
  gap is **permits, not threads**, and a Threads control would have no
  demonstrated consumer. The plausible-looking lead — the DFP is float32-in, so
  every frame packs f32 -> bf16 — is dead: that conversion is the 19%.

  **DeepX's pool is untested**, and testing it needs no code at all, only the
  two env vars plus the same per-thread CPU check.

  **Axelera carries BOTH controls since Voyager 1.8, and they are alternatives
  rather than layers.** Double buffering is the Lockstep executor's one boolean
  and belongs to **Sync**; the depth spin is frames in flight through
  `axr_run_async_model_instance()` and belongs to **Async**.
  `refresh_depth_sensitivity()` greys whichever the current mode does not use,
  and `ControlPanel::depth()` returns the spin in Async and the boolean (as 1
  or 2) in Sync, so `BenchItem::depth` stays one field and the CSV reads like
  every other card. **Both are shown rather than swapped**, so the row says what
  the card can do in either mode instead of hiding half of it behind a radio —
  with **Depth first**, so every card's row reads `API -> Depth -> extras`
  regardless of which buffering control is live.
  The runner sets `depth_ = 1` when async is on, because double buffering is
  then the other mode's knob.

  Historically — **the Off/On radio pair replaced a 1-8 spin** (`axelera_dbuf_off_` /
  `axelera_dbuf_on_` in `ControlPanel.cpp`, **Off listed first and, since
  2026-09-20, the default**; `depth(Accel)` maps them to 1 and 2 for
  `BenchItem`). Three places must agree on that default: the radio, `depth_`
  in `bench_axelera.cpp` (used when `configure()` is never called), and the
  async-ineligible fallback, which drops to sync with double buffering **off**
  rather than into the faster untested configuration. Automation
  accepts `axelera.double_buffer` / `axelera.dbuf` as aliases for the same
  thing. Note the widget order trap: `set_active()` must come **after**
  `set_group()`, or joining the group clears the selection.

  The reason it was never a 1-8 range is below — verified 2026-08-04 against
  the shipped library, because "why not 1-8?" is an obvious question:
  - The runtime's own log string is `' has depth=<n>, overriding to depth=2 for
    double buffering.`, next to `axr::ze_utils::LockstepExecutor::
    get_pipeline_depth()`. So double buffering *is* depth 2, in its own words.
  - `depth` is **internal, not settable**. Passing `depth=4` to
    `axr_load_model_instance` fails with `Invalid properties: Unknown property
    key: depth` — the same error as a made-up key. It appears in the binary only
    as that log message and the executor's accessor.
  - `double_buffer` is effectively boolean: `double_buffer=4` measured 379.7 fps
    against `double_buffer=1`'s 379.6 — the same run, because any truthy value
    just turns it on.

  So a 1-8 range would have had 3..8 silently behave as 2, and a run could
  later be recorded as "Axelera at depth 8" when no such thing happened — which
  is why the control became a radio pair rather than merely being clamped.
  Measured ladder, one core: no property 362.2 fps, `double_buffer=1` 379.6. At
  two cores: 573.7 → 589.6.

  **Qualcomm still ignores the API mode here, and that is deliberate.** It shows
  no mode radios, so gating its depth on "Async" would leave it permanently at 1
  and silently drop the ~1.45x host pipelining it *does* have. It regressed
  exactly that way when the mode control first moved out of the panel-wide row.

  **Axelera used to be in that sentence and no longer is.** Now that it reports
  both modes, its two controls are *both* gated on the mode — but on opposite
  sides, so neither is ever unreachable: Sync keeps double buffering live and
  greys the spin, Async does the reverse.

  A card with both modes reports depth 1 in Sync regardless of where its spin
  button sits, because that is what Sync *is*, and its spin button greys out.

  **The default is per card, because the saturation point is.** Measured on
  ResNet-50: DeepX 1 → 402.2 fps, 4 → 1085.1, 8 → 1061.6 — depth pays until the
  device saturates and *regresses* after, so DeepX defaults to 4. Axelera
  1 → 573.7, 2 → 589.6 (2 is the API ceiling, now expressed as the On/Off
  double-buffering radio).

  **MemryX saturates only at 8, and defaulting it to 4 cost 40% of the card**
  (fixed 2026-09-04). Measured on the real runner: **1077.5 / 1597.1 / 1796.7 fps
  at depth 4 / 6 / 8**, where 8 matches `mx_bench` (1796.10) to four significant
  figures. Depth 16 and a completely unthrottled stream both measure 1795, so 8
  is the genuine saturation point and not merely the top of the UI range — the
  existing `max_depth = 8` was already right, only `def_depth` was wrong. The
  mechanism is plain once measured: `permits` caps frames in flight, while the
  SDK's own scheduler defaults to `input_queue_size = 16`, so a depth of 4 left
  the runtime's queue three-quarters empty. Defaults live in two places and both
  must agree — `kAsyncDepth` in `bench_memryx.cpp` (the runner's own, used when
  `configure()` is not called) and `def_depth` in `ControlPanel.cpp` (what the
  GUI and automation actually use, via `BenchItem::depth`).

  **That default has been reverted to 4, because depth 8 wedges the MX3** — and
  the 40%-of-the-card argument above was already made once and cost several
  power cycles, so **do not make it again without stability evidence.** Seven
  reproductions across **two hosts and two different cards** — five on the EPYC
  host (80, 80, 57, 33, 11 s) and two on the Strix Halo machine (64, 79 s) —
  every one at depth 8 / ~1796 fps. The collapse is abrupt: full rate one
  second, the chip unresponsive the next, no throttle and no decay first. The
  kernel says `admin timeout … chip 0` on EPYC and `fops_write: wait timeout
  1(s), retrying again` on Strix Halo; probably the same event logged by two
  driver versions, but that is unverified. What it is **not**:
  - **not thermal** — it died at 72 °C, while depth 4 ran to 86 °C and survived;
  - **not a frame count** — ~143 k frames at depth 8 against ~269 k at depth 4
    with no trouble;
  - **not a fixed duration** — 11 s to 80 s;
  - **not the `mxa-manager` daemon** — one of the runs was local mode, which the
    daemon never saw;
  - **not brown-out** — the INA228 read 3.074 V at the wedge, 74 mV above the
    3.003 V floor, and the latched undervoltage detector never tripped;
  - **not our runner — ON EPYC ONLY.** There `mx_bench -f 200000` wedges
    identically at ~57 s. (An earlier comparison that seemed to exonerate depth
    8 in `mx_bench` was invalid: `-f 30000` is 16.7 s, below the shortest wedge
    window then known.) **On Strix Halo the opposite holds**: measured
    2026-09-14, `mx_bench -f 200000` completed **3/3** at 1796.51 fps, peaking
    at **99 °C** — hotter than the app's 67–69 °C at the wedge — so there the
    discriminator *is* our depth-8 path. Never quote "not our runner" as a
    general result; it is host-specific.

  **This is an open cross-machine investigation — see
  `docs/memryx-mx3-wedge.md` before re-deriving any of it.** That doc holds the
  evidence, the ranked suspects and the capture script
  (`tools/mx3-capture-env.sh`, read-only, runs on both machines).

  **It was originally written as "EPYC wedges, Strix Halo does not". That
  premise is false** (corrected 2026-09-14 from the Strix Halo capture): the
  same workload wedges on both machines. Consequences, so they are not
  re-derived:
  - **The SDK version is NOT ruled out — that claim is retracted.** It read
    "Strix Halo runs the May stack and wedges anyway, so 2.2.4/2.2.5 did not
    introduce this". The premise is true; the conclusion does not follow.
    Measured 2026-09-14 after upgrading Strix Halo to EPYC's exact combination
    (`memx-accl` 2.2.5-1, `memx-drivers` 2.2.4-1, `mxa-manager` 2.2.5-1, module
    1.3.13.1, firmware unchanged): our depth-8 path survived **309 s against
    64–79 s on 2.2.2** — 4x longer and 30 °C hotter. It still wedged. The SDK
    is the only variable that has moved this failure at all.
  - **Not the PCIe width.** `x2` is `max_link_width` on both hosts, i.e. the
    module's native width, not a link-training fault.
  - **Not cooling.** Strix Halo idles 20 °C cooler (37–40 vs 57–59 °C) and
    still dies, at 67–69 °C — matching the article's card surviving 90 °C.
  - **Not "the 6.8 → 6.17 jump".** Strix Halo never ran 6.8: it was on
    **6.17.0-23/-29 while the runs were succeeding** in May, and EPYC is on
    6.17.0-40 while failing today. Any kernel boundary is inside or above
    6.17.0.
  - **Not degradation inferred from run order.** The "monotonically decreasing
    time-to-wedge" this file used to cite was an artefact of five samples;
    Strix Halo's two went 64 → 79 s, and pooled over seven there is no trend.

  **The kernel is out too, at least here** — measured 2026-09-14 before
  spending a reboot on the rollback: on the *current* 7.0.0-31, `mx_bench
  -f 200000` completed three times at 1796.51 fps (the May figure was 1796.54),
  climbing to 99 °C. So this host still runs the published workload perfectly,
  and there is nothing for a 6.17.0-29 rollback to fix. That also kills thermal
  outright: the surviving case is 30 °C **hotter** than the failing one.

  **The clock lead is dead, and so is the degradation reading — superseded
  2026-09-15.** On that date `mb-benchmark-gui` ran **all four cards at depth 8**
  on this EPYC host, after the MX3 had been left alone for a while, and the
  MemryX **completed**: 1812.5 fps peak, 358 s, no wedge. A depth-4 session ran
  464 s the same evening. It also throttled exactly as the vendor documents —
  600 → **300 MHz** at ~97 °C (per-chip, binary, 50 %), fps 1800 → 1020, power
  12.07 → 7.74 W — and then ran another 200 s at 100 °C.

  That kills two arguments this file used to make:
  - **Not thermal, and the clock was never the discriminator.** Every wedge
    happened at **55–77 °C**, i.e. 20–40 °C *below* the trip point. "We hold
    600 MHz into the wedge" is not suppressed backoff; 600 MHz is simply correct
    at 60 °C. There was nothing to suppress.
  - **Not a degrading module.** The same card that had wedged ten times then ran
    depth 8 to thermal limits. The "monotonically worse" trend (80 → 11 s, then
    14/10/15 s, `mx_bench` failing at `-f 10000`, depth 4 dying at 160 s) came
    entirely from runs taken minutes after a previous wedge.

  **The best-supported explanation is now recovery time: a card that has
  recently wedged fails fast and cold, and a rested one runs to its thermal
  limit.** A power cycle alone is evidently not enough. What the recovery
  actually is — thermal mass, firmware state, driver state, the `mxa-manager`
  session — and how long it takes is unmeasured.

  **Consequence for anyone testing this: never draw a trend from back-to-back
  runs.** Each wedge contaminates the next measurement; that invalidated most of
  a day's work. Rest the card, then measure.

  `kAsyncDepth` stays at **4** regardless — one good session is not a soak
  record. Raising it needs a rested card and repetition. The depth-6 and
  paced-depth-8 tests are still the right experiments, but they must be run on a
  rested card or they will measure recovery debt instead of permits or rate.
  `docs/memryx-mx3-wedge.md` has the full evidence; `bench_memryx.cpp` carries
  the wedge log beside `kAsyncDepth`.

  `describe()` reports it as `· depth N` on the four cards that have a depth.
  It used to say `· N in flight`; that is gone. **Axelera says
  `· double buffer` / `· no double buffer`**, which is not an oversight — it is
  the only card whose control is not a depth, and a `depth 2` in the CSV would
  claim a knob the runtime does not have.

- **The frame-rate cap was cosmetic on three backends until `c4e07e0`, and
  `src/bench_pacer.h` is the fix.** The cap only ever throttled runners whose
  producer *is* the engine's own `run_frame()` loop. Where the producer
  free-runs — MemryX's SDK threads calling `on_input`, Axelera's `stream_loop`,
  Qualcomm's per-engine `worker_loop` — the cap limited **counting, not
  submission**: the reported fps obeyed it while the card stayed at full tilt.
  The power graph is what proves it, and is the reason this matters at all:
  ResNet-50 on MemryX capped to 300 fps drew **8.24 W** against 8.18 W
  uncapped — i.e. a "300 fps" row in the CSV carried the watts of a 1796 fps
  run, so every fps/W and mJ/frame figure taken at a cap was wrong. Capped now
  measures **4.04 W**.

  `FramePacer` is one class shared by all three: `set_target(item.target_fps)`
  in `configure()`, `await()` at the top of the producer, `stop()` **first** in
  `shutdown()`. `BenchItem` gained `target_fps` and `Bench.cpp` fills it in
  `start()`. Four things in it are load-bearing:
  - **An unpaced runner takes no lock at all.** `await()` returns on
    `period_ <= 0` before touching the mutex, so the uncapped hot path is
    unchanged — the pacer must never cost throughput when it is not asked for.
  - **Axelera calls `await(batch_)`**, because one invocation retires four
    frames. Pacing per call would cap the card at a quarter of the target.
  - **A producer more than a second behind its schedule re-origins** rather than
    sprinting to catch up; otherwise a slow model load turns into a burst the
    moment it finishes.
  - **`stop()` before the condition variable is signalled**, or a producer
    parked in `wait_until` holds teardown for a whole period — on MemryX that is
    an SDK callback thread, which is exactly where a wedge gets manufactured.

  Hailo and DeepX need none of this: their producer is `run_frame()`, so the
  engine's existing cap already bites at submission. **Qualcomm's pacer is
  compile-verified only** — it has not been run on the IQ-9075.

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
- **Qualcomm** — the artifact is a **context binary** (`.bin`), never a `.dlc`.
  Measured on this part, loading a cached context is 14–27 ms against 0.6–2.6 s
  to finalize a graph from scratch, so a runner that accepted a `.dlc` would be
  timing the compiler. Inputs are quantized uint8 already, filled once at load;
  outputs are left in raw quantized device layout and **never dequantized** —
  measured on this SoC, converting a detection head's uint8 outputs to float32
  costs more than the inference did (1.487 ms output vs 1.074 ms execute).
- Input is a fixed xorshift pattern (`bench_input.h`) — plausible bytes,
  identical on every run and every card.
- **Compare within a mode, never across.** A sync figure and an async figure
  answer different questions; the README states this and the UI labels the run.

## Work per frame, and how much of each card that uses

Computed 2026-08-08 from the source ONNX in
`../envic_ai_python/ai_reference/models` with a node-walking MAC counter
(Conv/ConvTranspose/Gemm/MatMul; `onnx.shape_inference`, batch pinned to 1).
**GOP = 2 x MAC.** Reproduce it rather than trusting these if a model changes.

| model | input | GMAC/frame | **GOP/frame** | params |
| --- | --- | ---: | ---: | ---: |
| ArcFace MobileFaceNet | 112x112 | 0.438 | **0.88** | 2.04 M |
| SCRFD-500M | 640x640 | 0.734 | **1.47** | 0.63 M |
| OSNet x1.0 | 256x128 | 0.979 | **1.96** | 2.16 M |
| SCRFD-2.5G | 640x640 | 3.428 | **6.86** | 0.82 M |
| ResNet-50 | 224x224 | 4.087 | **8.17** | 23.6 M |
| SCRFD-10G | 640x640 | 13.341 | **26.68** | 4.23 M |
| YOLOv8s | 640x640 | 14.301 | **28.60** | 11.2 M |
| YOLOv8m | 640x640 | 39.468 | **78.94** | 25.9 M |

**The counter is `tools/model-ops.py`** — it was ad-hoc when this section was
first written; it is now in the repo, with `--check` reproducing every row above
(worst deviation 0.56%, which is rounding in the quoted values). Use it rather
than retyping numbers.

The counter is validated three ways, which is why these are worth quoting:
YOLOv8s/m reproduce Ultralytics' published 28.6 / 78.9 **exactly** (confirming
their "GFLOPs" are 2xMAC, a common factor-of-two trap), and a ResNet-50 backbone
comes out at 4.087 GMAC against the published 4.09. ResNet-50's own ONNX is not
in the tree; its row is measured from `ai_deepx/models/deepmar_resnet50-1.onnx`,
which is that backbone plus a small attribute head.

**That ResNet-50 row is 5.9% high for a plain ResNet-50, and the reason is worth
knowing.** The Qualcomm artifact is built from ONNX Model Zoo `resnet50-v1-12`,
which measures **3.858 GMAC / 7.72 GOP** — not 4.087 / 8.17. The gap is the
classic **v1 vs v1.5** distinction: v1 puts the stride on the 1x1 convolution,
v1.5 on the 3x3, and v1.5 is the more expensive. Verified structurally rather
than assumed — `resnet50-v1-12` has **six stride-2 1x1 convs and zero stride-2
3x3 convs**, so it is v1. `deepmar_resnet50-1.onnx` carries the v1.5-style cost
plus its attribute head.

Consequence: **Hailo's artifact is `resnet_v1_50.hef`**, i.e. v1 by name, so its
utilisation below is probably computed against a ~6% too-large GOP figure. The
other three cards' artifacts are compiled binaries whose topology cannot be
inspected here, so this is flagged rather than corrected — but if the M.2
utilisation numbers are ever re-derived, establish which variant each vendor
actually ships first.

**The SCRFD names understate what we run by exactly 4/3.** InsightFace quotes
those flops at VGA 640x480; our artifacts are 640x640. `13.341 x 0.75 = 10.006`
against the "10G" nameplate — an exact match, and the same ratio holds for the
other two. So **SCRFD-10G is a 26.7 GOP model here, heavier than YOLOv8s** — do
not read the model name as its cost.

### How much of that runs on the accelerator

**In this app: essentially all of it, by construction.** Everything under
**Measurement choices** exists to keep host work out of the timed loop, so the
frame rate is the accelerator. In a full pipeline the vendors differ in what
they leave to the host — Hailo and DeepX bake NMS/decode into the artifact,
MemryX's compiler crops a `_post` head off (which is why the fetcher
deliberately does not extract it), and Axelera does quantize/pad, unpad/dequant
and the whole DFL/sigmoid/NMS decode in host NumPy.

**But in *op* terms even the worst case is negligible.** YOLOv8m on Axelera:
the host touches 1.23 M input elements and 706 k output elements at a few ops
each, ~11 MOP against 78,937 MOP of model — **0.014%**, i.e. >99.98% of
operations are on the accelerator regardless of vendor.

**Op share and time share diverge by four orders of magnitude, and that is the
point.** On the same weak-host cascade, Axelera's host post-processing measured
**121 ms against 80 ms of model time** — 60% of wall clock for 0.014% of the
ops, because the NPU runs at ~1e13 ops/s and host NumPy at ~1e9. **Never reason
about where the time goes from an op count.**

**No DeepX CPU-fallback for anything we ship** — verified by screening every
`Conv` node in all eight models for dilated depthwise (DX-COM 2.2.0's known
fallback trigger): 17/60/20 depthwise convs in ArcFace/OSNet/SCRFD-500M, every
one at dilation 1, and none at all in YOLOv8 or SCRFD-2.5G/10G. So the one case
where real compute would leave the NPU does not arise here.

### Card utilisation

Derived, not measured: the ResNet-50 figures from **Host specifics** multiplied
by 8.174 GOP/frame, against each vendor's rated INT8 throughput.

| card | fps | achieved | rated | utilisation |
| --- | ---: | ---: | ---: | ---: |
| MemryX MX3 | 1797 | 14.7 TOPS | 14 @ 600 MHz | **105%** (see below) |
| Hailo-8 | 1369 | 11.2 TOPS | 26 | **43%** |
| DeepX M1 | 1087 | 8.9 TOPS | 25 | **36%** |
| Axelera Metis | 1967 | 16.1 TOPS | 214 | **7.5%** |

**The MemryX row exceeds its own nameplate, so at least one of its two inputs is
wrong — do not quote it as a utilisation figure.** 1796.7 fps is solid: it is the
measured rate of our own runner and it matches `mx_bench` (1796.10) to four
significant figures. The suspect input is the 8.174 GOP/frame, which is measured
from `deepmar_resnet50-1.onnx` and is already flagged as ~6% high for a plain
ResNet-50 (see the v1-vs-v1.5 note under **Work per frame**). Even at the v1
figure of 7.72 GOP it comes to 13.87 TOPS, or 99%. The likelier explanation is
the artifact: it is named `ResNet_50_MXA_Optimized_224_224_3_onnx`, and a
vendor-optimised graph need not cost what a stock ResNet-50 costs. Nothing here
can inspect a compiled `.dfp`'s topology, so this is flagged rather than
corrected. **Establish the artifact's real GOP before publishing a MemryX
efficiency number.**

Two cards land in a plausible 36-43% band. **Axelera delivers about 7.5% of its
nameplate** — 2.7% before the fixed-batch fix (2026-09-03), 6.1% after it, and
7.5% since the property-separator fix (2026-09-20), which is **96%** of the
vendor's own 2054 fps. Even the vendor figure is only 7.8%
of the datasheet.
That reframes the open "18% of vendor" question under **API modes**: the gap to
the *datasheet* is far larger than the gap to the vendor's own benchmark, so
whatever is being missed is not a small tuning matter.

### Card utilisation — the IQ-9075 (Hexagon NSP)

Rated **100 dense TOPS INT8** (200 sparse) for the dual-HTP subsystem — the
official Dragonwing figure, and it covers **both** NSPs, which is what our 2-NSP
measurements should be compared against. Measured with the app's own headless
driver, burst, async, 2 NSPs, depth 4; GOP from `tools/model-ops.py`.

Unlike the M.2 table above, this is every model rather than ResNet-50 alone —
and the spread is the finding:

| model | GOP/frame | fps | achieved | utilisation |
| --- | ---: | ---: | ---: | ---: |
| SCRFD-500M | 1.47 | 4325 | 6.4 TOPS | **6.4%** |
| ArcFace MobileFaceNet | 0.88 | 10112 | 8.9 TOPS | **8.9%** |
| OSNet x1.0 | 1.96 | 4576 | 9.0 TOPS | **9.0%** |
| ResNet-50 | 7.72 | 2178 | 16.8 TOPS | **16.8%** |
| SCRFD-2.5G | 6.86 | 3504 | 24.0 TOPS | **24.0%** |
| YOLOv8s | 28.60 | 966 | 27.6 TOPS | **27.6%** |
| YOLOv8m | 78.94 | 507 | 40.0 TOPS | **40.0%** |
| SCRFD-10G | 26.68 | 1869 | 49.9 TOPS | **49.9%** |

**Utilisation tracks model size, not model identity.** The three sub-2-GOP
models all land at 6-9% however different they are, and the two heaviest reach
40-50%. At 10112 fps ArcFace retires a frame every 99 us, so what is being
measured there is per-frame overhead — the FastRPC round trip and host
threading — not the HMX array. **Do not read 6.4% as "the NSP is bad at
SCRFD-500M";** read it as "SCRFD-500M is too small to occupy this part".

**The ResNet-50 row is the only like-for-like comparison with the M.2 table
above, and it is 16.8%** — below all three healthy M.2 cards. Same reason: at
7.72 GOP against 100 TOPS nameplate, saturating this part would need ~12950 fps.

**Peak observed is 49.9 TOPS on SCRFD-10G**, which is a hard measured lower
bound on the silicon regardless of what one believes about the nameplate. It is
also the best datasheet fraction anything in this project reaches on its
heaviest model.

**The two NSPs scale cleanly, so the shortfall is per-NSP, not contention.**
OSNet on one NSP is 2313 fps = 4.5 TOPS against that NSP's 50, i.e. **9.1%**,
against 9.0% for two NSPs at 4576 fps. Identical to within noise — adding the
second NSP buys a full second NSP's worth. Whatever caps a small model is
per-engine overhead, and it is not fixed by more silicon.

### The same Hailo-8 is 41% slower on this board than on the x86 host

Worth stating separately because it is a trap for cross-host comparison. The
IQ-9075 carries a Hailo-8 as well, running the same artifacts:

| ResNet-50 | fps | achieved | of 26 TOPS |
| --- | ---: | ---: | ---: |
| Hailo-8 on x86_64 EPYC | 1369 | 10.6 TOPS | 40.6% |
| Hailo-8 on IQ-9075 | 801 | 6.2 TOPS | 23.8% |

Same card, same HEF, **59% of the frame rate**. On OSNet it is far worse —
214 fps, 0.42 TOPS, **1.6%** of nameplate. So: **never compare a frame rate
across the two hosts**, and treat this board's Hailo numbers as a floor rather
than a measurement of the card. (Both figures here use 7.72 GOP for ResNet-50, per the v1 note above; the
M.2 table's 43% uses 8.17 and is not directly comparable to the 40.6% here.)

**OSNet's 214 fps is NOT host-bound — that explanation is REFUTED (2026-11-23).**
This file said the figure was low "because a 1.96 GOP model on a card this fast
is entirely host-bound, and the IQ-9075's Kryo cores are much weaker than the
EPYC's". The load probe ran the same `osnet_x1_0.hef` **on the 128-core EPYC**,
async depth 8, and measured **214.6 fps** — within 0.3 % of the aarch64 board's
214. A host-bound workload cannot be indifferent to a host that large.

The cross-host ratios in fact run the *other* way, and they are consistent with
per-frame host overhead rather than with host weakness:

| model | EPYC | IQ-9075 | ratio | host calls/s on EPYC |
| --- | ---: | ---: | ---: | ---: |
| ResNet-50 | 1372.3 | 801 | 58 % | 1372 |
| YOLOv8s | 491.6 | 359 | 73 % | 492 |
| OSNet x1.0 | 214.6 | 214 | **100 %** | 215 |

The *fastest* model is the most host-sensitive and the slowest is not sensitive
at all — exactly backwards from "small models are host-bound". So OSNet's 214 fps
is a property of the **artifact**, not of either host: it is 6.4× slower than
ResNet-50 while being 4.2× lighter, i.e. ~27× worse per op, and at 215 frames a
second there is almost no host work left to be bound by. The HEF is 6.76 MB
against ResNet-50's 17.98 MB. Unexplained, and worth a look if OSNet ever
matters — but **do not attribute it to the host**, and do not cite OSNet as
evidence about either machine's CPU.

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

**`prepare_backend_environment()` also fills in `ADSP_LIBRARY_PATH`** for
Qualcomm, via `qualcomm_prepare_environment()`. FastRPC loads the DSP-side skel
and does **not** consult `LD_LIBRARY_PATH`, only `ADSP_LIBRARY_PATH`; getting it
wrong is the classic Qualcomm bring-up failure and its worst form is silent — the
graph quietly falls back to CPU and just reads slow. On Ubuntu for Dragonwing the
loader finds `/usr/lib/dsp/<domain>` unaided (verified by running with the
variable unset), so this only fills the variable in when **nothing** has set it,
globbing `/usr/lib/dsp/*`, `/usr/lib/rfsa/adsp` and `/usr/share/qcom/*/*/*/dsp/cdsp*`
for a `libQnnHtp*Skel.so`. It stays silent if it finds none, letting the SDK's own
error stand. Note the separator is **`;`**, not `:`.

No test suite. **Everything except the widgets is GTK-free and testable
headlessly** — prefer this over driving the GUI, which belongs to the user:

- **Runners** — `Catalog.cpp Bench.cpp bench_runners.cpp bench_*.cpp`; a ~50-line
  main that calls `Catalog::discover()`, `make_runner(accel, mode)`, `load()`
  and loops `run_frame()` prints fps per card, per mode. On the IQ-9075 that is
  `g++ -std=c++17 -O2 -Isrc -I/usr/include/QNN -DMB_HAVE_QUALCOMM=1 …
  src/bench_qualcomm.cpp src/qnnruntime.cpp -ldl -lpthread` — the Qualcomm
  backend needs no library beyond `libdl`, since the shim dlopen's QNN.
- **Downloads** — `Catalog.cpp Fetcher.cpp`; drive `Fetcher::start(catalog)` and
  poll `snapshot()` to verify a new `[vendor:*]` source end to end.
- **Telemetry** — `Probes.cpp` alone.
- **Logging** — `Logger.cpp Probes.cpp Catalog.cpp`; discover, poll a few times,
  `write_row()`, `close()`. Set `MB_BENCH_LOG_DIR` to a scratch directory so the
  driver does not litter `logs/`. Worth asserting on the output rather than
  eyeballing it: `time` parses with Python's `datetime.fromisoformat`, a NaN
  reading is an **empty** field and not `0.000000`, and a message containing a
  comma and a quote survives Python's `csv` reader. Then run
  `python3 tools/csv-to-html-plot.py -i <file>` — it must plot with **no**
  warnings. `../mb-powermon/csv-to-html-plot.py` must still open it too, which
  is what caught `host` being written as column 0; it warns about far more than
  the benchmark columns, though, because it predates `_INA228`/`_CLK`/`_C<n>`
  and drops all four shunt rails and every clock.

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
immune because it samples continuously from before the run starts. It was hit
again 2026-09-13 while measuring the frame-rate cap — DeepX read 1525 fps
instead of 1284 — so the trap survives being documented; **discard it in the
driver, don't remember to.**

**Stamp `dt` at the top of the tick, before `poll()`.** The second half of the
same bug: `Probes::poll()` takes ~190 ms over four FTDI bridges, so a driver
that polls and *then* measures the interval divides by a `dt` 19% short and
inflates every rate by 1.19× — 357 fps reported for a 300 fps cap, which read
exactly like pacer overshoot and nearly got a correct pacer "fixed".
`MainWindow::on_tick` is right (it stamps `last_time_us_` first); a headless
driver has to do the same deliberately.

**Vendor tools available on this host** for cross-checking, and worth using —
they are the ground truth this app is validated against. **Run them from a
scratch directory, not the repo root:** `dxbenchmark` writes
`DXBENCHMARK_<timestamp>.{csv,html,json}` plus a ~15 MB `profiler.json` into the
current directory, which otherwise litters the working tree (`.gitignore` has a
backstop, but the files still land there).
`hailortcli benchmark <hef>`, `mx_bench -v -d <dfp> -f <frames>` (add `-s` for
shared mode — it measures the same throughput, see the `local_mode` note under
**API modes**, so the bare form is fine),
`dxbenchmark --dir <dir> --warmup 10 --time 30` (prints a profiler table rather
than an FPS line; divide the M1's core count by "NPU Core" average). See the
comparison table in the README.

### `tools/`

**`npu-status.sh` — read-only health check, the first thing to run when a card
misbehaves.** `./tools/npu-status.sh [all|hailo|deepx|memryx|axelera]`, no root,
touches nothing (it reads `/proc`, `/sys` and systemd state; the only device
access is Axelera's `--fwver`, a control-plane read). Exits 0 when everything is
healthy, 1 otherwise, so it works in a script.

**`npu-reset.sh` — the one that acts.** `sudo ./tools/npu-reset.sh
{all|hailo|deepx|memryx|axelera}`. It **sources** `npu-status.sh` for the checks
and helpers rather than keeping a second copy — the two must not drift, and the
first version of these scripts already had one copy fixed and one not. Keep them
installed together. Status is still `npu-reset.sh`'s default so a mistyped
argument cannot tear down drivers.

**MemryX recovery is staged, and that staging is load-bearing.** The default is
a `mxa-manager` restart only. `modprobe -r memx_cascade_plus_pcie` is behind
`--hard` because on this host it made things *worse*: the card fell to D3cold and
would not come back —

```
memx_pcie_ai_chip 0000:47:00.0: Unable to change power state from D3cold to D0, device inaccessible
memryx: memx_firmware_init: get hardware info from fw failed
probe with driver memx_pcie_ai_chip failed with error -1
```

— leaving the PCI device enumerated but **unbound, with no `/dev/memx0` at all**,
which is worse than the wedged-but-present state it started in. Recovery from
*that* is `echo 1 > /sys/bus/pci/devices/<bdf>/remove` + `echo 1 >
/sys/bus/pci/rescan` (the tool offers `--rescan`), or a power cycle. Do not
"simplify" the staging back into an unconditional module reload.

It encodes the recovery knowledge that was otherwise spread across this file:
- the MemryX module is **`memx_cascade_plus_pcie`**, not `memx`, and
  `mxa-manager` holds a reference — the daemon must stop before it will unload;
- DeepX's `dxrt_driver` depends on `dx_dma`, so it unloads first and loads last,
  around a `dxrt.service` restart;
- an Axelera reload **cannot** clear a wedged Metis, and the script says so
  instead of reporting success;
- `DVM_ALREADY_IN_USE` is two power clients, not a wedged Hailo driver.

Status also detects the two states that otherwise take a diagnosis: the MemryX
`65262000` (-274 °C) sentinel on every sensor, and an Axelera sitting in
bootloader *with* DMA errors this boot (⇒ wedged, power-off required) as opposed
to one merely cold-booted (⇒ normal, any model will load its firmware).

Use `/proc/modules`, not `lsmod`, for module checks — `/usr/sbin` is not on a
normal user's `PATH`, so `lsmod` silently reports every driver missing in the
unprivileged status view. That bug was in the first version of this script.

**`model-ops.py` — the op counter behind "Work per frame".** `tools/model-ops.py
<onnx>...` prints GMAC / GOP / params per model; `--check` re-derives every row
of that section's table from the reference ONNX set and reports the worst
deviation (0.56% today, all rounding in the quoted values). Needs a Python with
`onnx` — on the IQ-9075 that is `~/qaihub-venv/bin/python`, not the system one.
Use `--shape 640x640` for a graph with dynamic spatial dims (SCRFD ships them).
It counts Conv/ConvTranspose/Gemm/MatMul only, and **GOP = 2 x MAC** — that
factor is the single most common way these figures get quoted wrong.

**`ra6m5-report-{tc,loop,softsensor}.py` generate the studies in
`docs/reports/`.** Each
writes one `data.js` (a single `const` object) next to a hand-written
`index.html` that renders every table and chart from it — **nothing in a page
re-derives a number, and nothing is typed in**. Both print every scalar their
prose quotes, so the prose can be checked against the data rather than
remembered, and both regenerate **byte-identical** (assert that after any
change; it is the cheapest regression test these have). Conventions that are
load-bearing in both: columns resolved by **suffix**, because the BDF and USB
port path move across reboots; `die` is `max(TS0, TS1)`, never a mean; a
missing reading is JSON `null`, never 0; and every thermal resistance uses the
INA228 **rail** power (`_INA228_POWER`), never the card's own `_POW`, which
runs ~0.19 W low — the firmware's own estimate uses the rail, so it is the only
basis on which the estimator can be scored.

- **`ra6m5-report-softsensor.py` is the one to read for the FIT** —
  `docs/reports/ra6m5-soft-sensor/`, and it is deliberately a separate page
  from `ra6m5-loop/`. That one reports one run; this one reports the fit, which
  spans four (two to characterise, one to fit on, two held out) and whose whole
  point is that the data came from somewhere before the model did. It takes the
  logs as arguments, so re-running it on a later session is a command and not
  an edit, and it is the **reproducible computation** for every Rconv figure —
  prefer its output over any number from a scratch script.
- **`ra6m5-report-loop.py` reads the soft sensor's constants out of
  `Control.h` rather than carrying a copy** (2026-11-24), and raises if the
  header is refactored. The page quotes `Rconv`, the heat capacity and the time
  constants it implies, and a second copy here would be free to drift away from
  the firmware that actually runs — which is the kind of disagreement a report
  should catch, not create. Its model section scores the estimate on the
  three-arm run, which is **held out of the fit**, so those numbers measure the
  model and not its own residual.
- **`ra6m5-report-loop.py` was rewritten 2026-11-23 for 0.6.0** and the old
  four-setpoint shape would have broken on the three-arm run in four places,
  each worth knowing: the changed enums; `nan` in `_CTL_IN`/`_CTL_ERR` for the
  open arms, which makes every `max()` over an empty sequence throw; an
  `idles()` that looked for the loop still armed, finding nothing now that
  `fan.idle = 0` turns it off; and a setpoint-cost regression with no setpoints
  to fit. **Score open-loop arms against the CARD's sensor, not `_CTL_IN`** —
  it is the one instrument common to every arm.
- **The estimator must be scored on the SETTLED WINDOW ONLY.** In open loop
  `die_est` tracks `target` by construction while the fan is unclamped, so
  during the approach it reads ~90 °C while the die is still at 57 — pooling
  the ramp reports a +42 °C "error" that says nothing about the model. The
  0.5.0 script pooled whole steps safely only because every step was closed.
- **The fan-off brackets need their own, SHORTER window** (300 s, not the
  arms' 600). Every bracket but the first follows a hot soak, so 600 s reaches
  back into the cooling transient and moves the reading by more than the
  session drift the window exists to measure.
- **THERE IS NO JS RUNTIME ON THIS HOST** — no node, deno, bun, qjs, or any
  Python binding — so a report page's JavaScript cannot be executed outside a
  browser. Verify a generated page by **asserting against the data**: every
  `LOOP.*` path resolves in the JSON, every row key used in a `forEach` body
  exists in that list's elements, every element id referenced exists, every
  declared table gets filled, tags balance. Then screenshot it. Headless
  Firefox here is a **snap**, so it refuses a second instance while the user
  has one open — check `ps` for whose it is and **never kill theirs**.

**`csv-to-html-plot.py` is a fork of `../mb-powermon`'s, not a copy** — unlike
`GraphArea`/`util.h`, it is *not* kept identical and must not be `cp`'d in either
direction. It adds the metrics that postdate the sibling (`_INA228_POWER`,
`_INA228_TEMP`, `_INA228_ENERGY`, `_INA228_VBUS`, `_INA228_CURRENT`, `_CLK`,
`_C<n>`, `_SYS_VBUS`/`_SYS_CURRENT`/`_SYS_POWER`, and the Qualcomm NSP
`N<i>-<b>` zones), the six
benchmark series, and the run/message tables. **`_IBUS` and `N<i>-<b>` were
missing until 2026-09-07** — an unrecognised column is silently *skipped*, so all
six NSP thermal traces were being dropped from every plot of that board, with
only a warning to show for it. After adding a metric, re-run the plotter and
check the printed series counts, not just that it exited 0. Its chart order
mirrors the GUI's — Bus Voltage, Current, Power, Accumulated Energy, … — so a
plot reads the same way as the app that produced it. **It also still accepts a bare `_INA228`**, which is what
the folded shunt power column was called before 2026-09-03 — logs already on disk
carry that spelling and must keep plotting. `_INA228_CHARGE` is parsed and deliberately *not* charted
— coulombs cannot share the joules axis. Things in it that are load-bearing:
- **The `SYS_*` entries must precede the bare `SYS`, `VBUS` and `CURRENT`
  alternatives**, for exactly the reason the next bullet gives: with `SYS`
  first, `usb 3-1.4_SYS_VBUS` splits as device `usb 3-1.4_SYS` + metric `VBUS`
  and invents a phantom device.
- **`_METRIC_RE`'s alternation is ordered.** The device capture is non-greedy, so
  the first split that matches wins: with the bare `TEMP` listed before
  `INA228_TEMP`, the column `0000:47:00.0_INA228_TEMP` splits as device
  `0000:47:00.0_INA228` and invents a phantom device no card can match. The same trap applies to every family added since — `INA228_VBUS`
  and `INA228_CURRENT` must precede the bare `VBUS` / `CURRENT` alternatives.
- **`identify_devices()` has to be folding-aware.** It cross-checks each card's
  temperature-column count against the note's `<n> temp`, but a folded shunt
  gives the card one *more* temp column than its probe reported. `_card_temps()`
  counts only non-`ina228` suffixes, and `usb `-prefixed bridges are excluded
  from `temp_order` entirely. Without both, every card falls back to label
  inference and the colours can go wrong across the whole plot.
- **The accelerator colours are `util.h`'s `accent::` values, duplicated in
  Python.** "Colour means one card, everywhere" extends to the plots. Sensors
  within a card vary by *lightness*, never by hue — the upstream fallback hashes
  on (device, kind) and would scatter one card across the palette.
- **Seconds are the internal unit; minutes are a display conversion.** Every
  window, gate and boundary in the script reasons in seconds — the cold and
  sustained windows, the run boundaries, the phase detection — and `to_min()` is
  applied only where a number is rendered (axis, tooltip, run table, message
  log, and the hailortcli overlay markers, which are plotted as datasets and
  would otherwise collapse onto x≈0). Converting at the source would mean
  re-deriving every threshold, which is where a units bug would actually hurt.
- **A BDF does not identify a card**, since `Logger` strips the device name and
  BDFs move across reboots (DeepX and MemryX swapped between 2026-08-04 and
  2026-08-08 — the host-specifics table below records the older assignment).
  Identification reads the log's own startup note, which lists probes and sensor
  counts in `discover()` order; the fallback is label vocabulary, where the
  discriminator is that **MemryX reports power and DeepX does not**. That pair
  is easy to get backwards and doing so silently swaps two cards' colours.

`axelera-temps.sh` + `axelera-temps.service` are now only for the **headless**
case. In the GUI both gates clear themselves:

- **App firmware** — the runtime loads it on the first `axr_device_connect()`,
  i.e. the first Axelera benchmark. Nothing else may load it; see the
  firmware-load note below.
- **Collector level** — `AxeleraProbe::read_temps()` raises it to
  `inf:collector` once, on seeing firmware present but no `core_temps`. The flag
  resets when it next sees a version mismatch, so a reboot or a `modprobe -r
  metis` re-arms it. `$MB_AXELERA_NO_COLLECTOR=1` opts out.

  **NEVER while a benchmark is running** (2026-09-20). That write is an `axcmd`
  shell-out — a *second process opening the card* — and its trigger, "firmware
  present but no core_temps", fires the instant `axr_device_connect()` finishes
  uploading firmware. Observed: the write landed one second after the upload,
  `Link down` followed within that second, and the card fell back to
  bootloader with a DMA error — a power cycle. **The evidence is timing, not
  proof.** It was first read as "two firmware reloads inside that second =
  two openers", but a single clean connect also logs *two*
  `fwtrace: detached for firmware reload` lines (seen 2026-09-20 12:46 and
  2026-09-21 09:20, both healthy runs), so the double reload is normal and
  does not identify a second opener. The gate is kept because a second process
  opening the card mid-load is never desirable, not because it is proven to
  have caused that wedge. So
  `Probes::set_busy(bool)` is set from `MainWindow::on_tick` **before**
  `poll()`, from `engine_.active() || engine_.stopping()` (stopping counts:
  a worker can still be inside a vendor SDK call), and the probe skips the
  write with `collector not enabled while a benchmark is running`. It retries
  once the run ends — `collector_set_` stays false. The sibling has no engine
  and no collector write, so `set_busy()` is a no-op there by construction.

  **The tools are globbed NEWEST-first, and were not.** `glob()` returns
  sorted, so `.front()` picked `/opt/axelera/runtime-1.7.0-1/bin/axcmd` on a
  host where 1.8 sits beside it — a 1.7 tool against the 1.8 firmware the
  runtime had just loaded, which the app itself logged as
  `device firmware v1.8.0 vs tool v1.7.0` and we read past. `.back()` now, the
  same rule CMake already follows for `AXR_LIB`. **Ported to the sibling**;
  that half applies to both, the `set_busy()` half does not.

**This reverses an earlier decision, deliberately.** The probe used to stay
passive because the collector level is *global* device state — another client
sharing the card gets the busier log ring too. That reasoning is still true; it
lost to "temperatures never work after a boot", which was the actual outcome
every single boot. The code comment carries the trade so it does not read as a
forgotten rule.

Note what the probe still will **not** do: load firmware. That is the one thing
that must stay with the runtime.

Two things in that script are load-bearing:
- **Never pipe into `grep -q` under `set -o pipefail`.** grep exits at the first
  match, the producer takes SIGPIPE, and the pipeline reports failure — so a
  match reads as "no match". This silently skipped the firmware load. Capture to
  a variable, then match.
- **The firmware-load branch is gone, and must not come back.** The script used
  to run `axcmd --fwload` on a card it found in bootloader. Measured repeatedly
  on 2026-08-07: firmware uploaded that way leaves the card unable to run
  inference — the *second* frame fails with

  ```
  [ERROR][waitForQueueBinaryCompletion]: Wait kernel failed with return code -1433
  [ERROR][axeCommandQueueExecuteCommandLists]: Failed with error code: 1879048193
  ```

  `0x70000001` is `ZE_RESULT_ERROR_DEVICE_LOST`. There are **no DMA errors, no
  link drop and no page faults during the run** — the card looks healthy,
  `axcmd --fwver` answers, temps flow, and every model still refuses to run. A
  driver reload does not clear it (card RAM survives), only a power cycle.

  The runtime's own path is fine: `axr_device_connect()` loads firmware on a cold
  card and inference works. The evidence is one-sided — every boot where the
  script uploaded, inference failed; the one boot where the runtime loaded it,
  four consecutive runs passed (358.7 / 574.0 / 658.6 / 694.9 fps).

  **Correct order: run a model first, then run the script for temperatures.**
  The script now reports a bootloader card and exits 0 without touching it.

  This was diagnosed, then wrongly retracted, then re-confirmed. The retraction
  came from seeing a `fwtrace: detached for firmware reload` with no service
  installed and assuming the app's runtime had done it — the user had run the
  script by hand. **Do not re-litigate this from a single reload event in the
  journal; check who invoked it.**

**The unit runs `/usr/local/sbin/axelera-temps.sh`, not the repo copy, and that
matters here.** This checkout lives on `/media/abbeefepyc/Nauvoo` (`/dev/sda`),
which is **not in fstab** — `udisksd` mounts it *on behalf of uid 1000* when the
user logs into GNOME. Measured on one boot: boot 17:16:46, mount 17:17:30,
`multi-user.target` 17:18:00. So the repo path simply does not exist for the
first ~44 s of a boot and never exists on a headless one; an `ExecStart` into it
would race the login or fail outright. The script references nothing in the repo
(only `/opt/axelera` and `/dev/metis-*`), so it is installed out. Consequence:
**editing `tools/axelera-temps.sh` does not change what boots** — re-run the
`install` line in the unit's header.

### Desktop entry

`mb-benchmark-gui.desktop` uses `Icon=M_benchmarking`, *not* the `M_logo` that
`mb-powermon-gui` uses — otherwise the two apps are indistinguishable in the
launcher. `Categories` is `System;Monitor;` only: adding a second *main*
category (e.g. `Development`) makes the app appear twice in the menu, which
`desktop-file-validate` warns about. A launcher in `~/Desktop` needs mode 755
**and** `gio set … metadata::trusted true`, or GNOME's `ding` extension renders
it as a text file.

`Name` is **`mb-benchmark`** (`mb-powermon` in the sibling), deliberately short:
anything longer wraps to a second line under the desktop icon. It was
"NPU Benchmarking GUI", then "mb-benchmark-gui" — both wrapped. Keep the
descriptive wording in `GenericName`/`Comment`, which is what feeds the tooltip
and menu search, and leave `Name` terse. Editing a `~/Desktop` copy in place
rewrites the file, so re-apply **both** mode 755 and `metadata::trusted` after
any such change.

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

**The MX3 wedges at async depth 8, and only a power cycle clears it.** Eight
reproductions, 11 s to **309 s** at ~1796 fps, **on two hosts and two different
cards**, including one through the vendor's own `mx_bench`; ruled out frame
count, duration, the `mxa-manager` daemon, rail brown-out, PCIe width and
cooling. **Not** ruled out: the SDK version — 2.2.5 stretched survival from
64–79 s to 309 s here without fixing it. The default is back at 4. Full evidence and the "do not
re-raise the throughput argument" note are in the depth table under
**API modes**; `bench_memryx.cpp` carries the same log beside `kAsyncDepth`.
Recovery is a power cycle — a `mxa-manager` restart is not enough once the
driver reports `admin timeout … chip 0`, and the app then hits the pre-`main()`
hang below on its next launch.

**The app hangs before `main()` when the MX3 is left wedged.** Symptom: clicking
the desktop icon appears to do nothing — no window, no error, and GNOME's
startup spinner just gives up. The process *is* there; it is blocked in
`memx_fops_open` with **one thread, 0% CPU, and only fds 0/1/2 open** — no X11
socket, no config file, no device fd. That fd list is the tell: it never reached
GTK, so this is not a GUI or a config problem. `libmemx.so` / `libmx_accl.so`
are linked in, and an ELF constructor opens `/dev/memx0` at load time, *before*
any of our code runs. **Nothing in the app can guard against it** — every launch
of the binary hangs identically, icon or terminal.

Trigger: killing an instance while it holds an MX3 session. `mxa-manager` does
not reclaim the session; it stays up but spins (~24% of a core, versus idle),
and every later `open()` blocks forever. Clear it with:

```bash
pkill -x mb-benchmark && sudo systemctl restart mxa-manager
```

and if a launch still hangs, the driver rather than the daemon is wedged:
`sudo modprobe -r memx && sudo modprobe memx`.

Diagnose it in this order — `ls -l /proc/<pid>/fd` first (three fds ⇒ pre-main),
then `cat /proc/<pid>/task/*/wchan`. **Do not confuse this with the Stop-button
freeze below**: that one was a GUI-thread join, has many threads and a mapped
window, and is fixed. I conflated the two once already and debugged the wrong
thing. Unfixed at the app level; the honest fix would be reaping the MX3 session
on exit so a hard kill can't poison the next launch, which needs a look at what
`MxAccl` teardown actually offers.

**Axelera: claiming all 4 AIPU cores wedges the card — under the OLD
multi-connection implementation.** Everything in this entry was measured against
the arrangement that opened one `axr_device_connect()` per core and ran four
instances. The fixed-batch path that replaced it (2026-09-03) makes **one**
connection and **one** instance, so the contention and the repeated firmware
reloads that produced these symptoms do not arise; four cores at batch 4 has run
clean here. Two caveats before treating it as solved: the batched path has only
minutes of soak time, not hours, and `$MB_AXELERA_MULTI_INSTANCE=1` reinstates
the old arrangement exactly, including this failure mode.

The original entry, kept because the symptoms are worth recognising:
Reproduced 2026-08-04 by stepping the AIPU-cores control 1 → 2 → 3 → 4 in one
session: 1, 2 and 3 all ran; 4 failed immediately with

```
[libaxldev.c:142] Failed to wait for MSI 0..3: Connection timed out
[ERROR][waitForQueueBinaryCompletion]: Wait kernel failed with return code -1
kernel: axl 0000:..: IRQ MSI timeout (0 30) (1 30) (2 30) (3 30)
kernel: axl 0000:..: Link down / Link up
```

— all four MSI vectors at once, then the PCIe link drops and the card falls back
to bootloader, after which every firmware upload fails until a **full power-off**
(a driver reload does not clear it). 3 is the highest value observed stable;
the shipped default is **2**, one step below the cliff.

A 4-core run *can* succeed briefly — a 20 s headless run measured 694.9 fps with
all four cores confirmed at 800 MHz — so this is not "4 never works", it is "4 is
not survivable". **Do not raise the default back to 4 on the strength of one
good run.**

**It is also model-dependent, and the failure is at *load*, not under load.**
Measured 2026-08-08 through an `--automation` sweep at `axelera.cores = 4`:

| step | outcome | rail | clocks | maxT |
| --- | --- | ---: | --- | ---: |
| ResNet-50, 2 min | **fine**, ~700 fps | 5.2–5.4 W | 4 × 800 MHz | 47 °C |
| YOLOv8m, next load | dead in ~2 s | **2.3 → 2.0 W** | unreadable | — |

ResNet-50 sustained all four cores for two minutes; the very next Axelera model
load died with the MSI timeouts above. During the *working* run the vendor
library also emitted `AxeleraDmaBuf.cpp: DMA transfer failed: Device or resource
busy` / `wait_for_dma called but no dma transfer is in progress` — four
instances contending on the DMA path, and the earliest warning sign there is.

**The power-budget hypothesis is dead — do not re-raise it.** This file used to
say a `Link down` at four cores is what a power limit looks like, and to watch
the INA228 on the Axelera rail across 3- and 4-core runs. That test has now been
run, from the app's own CSV: the rail **neither collapsed nor spiked**. It sat at
~2.3 W through the failure, fell to 2.0 W, and settled at 2.41 W — idle level.
Peak draw under a *healthy* 4-core run was 5.4 W. The card dies while loading a
model at idle power, so this is not an M.2 slot power budget and not thermal
(47 °C). Note also that YOLOv8m was the **second** Axelera load session of that
process, which points at the separate one-session-per-power-cycle entry below
rather than at core count alone.

**A DEVICE INIT WHILE ANOTHER CARD IS ACTIVE kills the Axelera — in either
direction.** That is the sharpest statement the evidence supports, and it is
narrower than the "concurrency" wording this entry carried until 2026-10-31
12:13. The outcome is sometimes a wedged card and sometimes a dead host; the
*trigger* is the same in both.

**The run that pinned it down** — stagger 5 s, order **Axelera first**:

```
12:13:19  Axelera connects, loads firmware (alone)   2 x IO_PAGE_FAULT
12:13:20-23  Axelera 1627 fps, 6.35 W, VBUS 3.140 V  healthy, full rate
12:13:24  Link down / Link up                        <- DeepX's worker reaches
12:13:25  Axelera 0 fps, 1.88 W; MSI timeout (12 1)     make_runner() ~12:13:24.6
12:13:54  MSI timeout (0 30) -> bootloader + 1 DMA error, WEDGED
```

Axelera was **running at full rate**, not loading, and died in the same second
as *DeepX's* device initialisation. The other three then ran another minute and
the host survived. So it is **not** "Axelera cannot start while another card
runs" — that framing was too narrow, and the reverse order disproves it.

Every occurrence of the *host-death* variant leaves nothing behind: no kernel
oops, no `Link down`, no MSI timeout, no systemd shutdown, and the journal
simply stops mid-log. 2026-10-31 10:58 the whole PC jammed hard enough to drop
the remote connection.

| date | configuration | double buffer | outcome |
| --- | --- | --- | --- |
| 2026-08-08 x2 | `--automation`, cores = 4 | off (inert then) | host down |
| 2026-09-20 | GUI, 4 cards, cores = 4 | **on** | host down |
| 2026-10-31 10:45 | GUI, 4 cards, no stagger | **off** | Axelera never loaded — `Link down`, MSI timeout, card **wedged** |
| 2026-10-31 10:58 | GUI, **DeepX + Axelera**, staggered 5 s | **off** | Axelera loaded (471 fps, 6.43 W), then **host froze** |
| 2026-10-31 12:13 | GUI, **Axelera first**, staggered 5 s | **off** | Axelera **running at 1627 fps**, died at **DeepX's** init; host survived |
| **2026-11-02 13:21** | GUI, **Axelera first**, staggered 5 s, **`amd_iommu=off`** | **off** | identical, and **captured over netconsole**: Axelera at **1710 fps**, `Link down` **79 ms** after DeepX's `load()`; card wedged, host survived |

What that table kills:
- **Not the IOMMU** — the 11-02 run reproduced it exactly under
  `amd_iommu=off`, with 0 `IO_PAGE_FAULT`s for the boot.
- **Not double buffering** — off in both 10-31 failures.
- **Not core count or total power.** The 10:58 freeze was *two* cards at ~33 W;
  four cards at ~45 W have run for minutes.
- **Not the 3.3 V supply.** The PMD2 (ElmorLabs) measures the PSU rail at
  **3.291 V idle and 3.278 V at 17.6 W — 13 mV of droop** — while the card's
  own INA228 rail moves **115 mV** (3.229 -> 3.114 V) over the same range. The
  supply is stiff and nowhere near its limit; all the droop is downstream in
  the board's M.2 distribution (~49 mOhm, consistent with the September
  source-impedance measurement). **Stop treating this as a brown-out.**
- **Not "Axelera cannot launch while another card runs"** — at 10:58 it
  launched fine with DeepX already at 1072 fps, and at 12:13 it was the
  *running* card that died at someone else's init.
- **Not load, duration or rate.** 12:13 died four seconds in, at full rate,
  having just held 3.140 V — the same rail voltage as a healthy solo run.

**The IOMMU is RULED OUT — tested 2026-11-02, and it is not the cause.** It was
the leading suspect: the trigger is *device initialisation*, i.e. DMA mapping
setup; it fires in either direction; and Axelera's own init on 10-31 logged
**two** `IO_PAGE_FAULT`s where one per boot is the norm. The test ran under
`amd_iommu=off` with `iommu_groups: 0` confirmed **before** the run, and the
failure reproduced **identically**. `IO_PAGE_FAULT` for that boot was **0**, so
the two page faults were not a clue either. **Do not re-run this test** —
`~/iommu-test/` holds the runbook and scripts if the reasoning needs checking.

A solo Axelera control under `amd_iommu=off` measured **1666 fps mean (excl.
first sample) / 1710 max** against the IOMMU-on baseline's 1614 / 1662: +3.2%,
within noise, with no `swiotlb buffer is full`. So the test boot was not a
degraded one and the null result is trustworthy.

**What the capture bought — the first real evidence in this investigation.**
netconsole (printk over UDP to another machine; *not* a serial console, which
breaks RealVNC here) plus `kernel.printk = 7 4 1 7`, because `quiet` leaves
`console_loglevel` at 4 and a console receives only levels 0-3 — **`Link down`
would have been dropped**. App CSV and kernel log interleaved:

```
13:20:57.445  app:  stagger start: 5 s apart, Axelera -> DeepX
13:20:58-02   app:  Axelera 1451 / 1670 / 1666 / 1666 / 1710 fps   healthy
13:21:02.445  app:  DeepX leaves Waiting -> make_runner() -> load()  (exactly 5.000 s)
13:21:02.524  axl:  Link down                                       (+79 ms)
13:21:02.612  axl:  Link up
13:21:03.201  app:  deepx_cfg -> "1x224x224x3 uint8 - depth 4"      DeepX load() done
13:21:04      app:  Axelera 0 fps — never again
13:21:07.852  axl:  vmsi configured + fwtrace: detached for firmware reload
13:21:09.196  axl:  IRQ MSI timeout (12 1), then once a second indefinitely
13:21:10.273  app:  firmware v1.3.2+bl1 vs tool v1.8.0 -> BOOTLOADER = wedged
13:21:33.826  app:  Axelera: run failed: AXR_ERROR_RUNTIME_ERROR
13:21:33 on   app:  DeepX 1080 fps for another 2.5 minutes, untouched
```

Three further eliminations from that run:
- **The two cards do not share a PCIe hierarchy.** `axl` sits under root port
  `0000:80:01.1`, DeepX under `0000:c0:01.1` — **different root complexes**. No
  shared switch, so no shared bus reset or link retrain can be the mechanism.
  Verify with `basename $(readlink -f /sys/bus/pci/devices/0000:<bdf>/..)`
  rather than assuming; BDFs move.
- **DeepX's init writes NOTHING to the kernel log.** The
  `dxrt0: dxrt_recovery_device` lines are from **boot**, not the run — do not
  mistake them for the trigger. Whatever happens at 13:21:02.4 is entirely
  userspace, inside the DXRT runtime.
- **The Axelera is the only casualty.** DeepX ran 2.5 more minutes at full
  rate, so this is not a shared-resource collapse.

**The next lead: the axl driver logs `All AER errors masked` at probe.** It
masks the PCIe error reporting that would say *why* the link dropped — a
completion timeout and a receiver error look identical from outside. Unmasking
AER on that device, or enabling it on the root port, is cheap and is the
experiment to run next. The driver also prints one `aer_cor_status: 0x00002000`
at probe — bit 13, Advisory Non-Fatal, **correctable**, once per boot on four
consecutive boots including clean ones. Boot-time constant, like the `PCI PERR`
SEL entries; not a lead.

**The capture facility now exists and works** — netconsole, `console_loglevel`
7, kdump armed, `hardlockup_panic` / `panic_on_oops` / `panic=20`;
`~/iommu-test/README.md` is the runbook and the 2026-11-02 reproduction above is
the first event it caught. What follows describes the state before that.

**The real obstacle WAS that there is no evidence.** This host had **no capture
facility at all** (checked 2026-10-31: no kdump, no `crashkernel=`, no
`console=` so IPMI SOL shows nothing, `hardlockup_panic` / `panic_on_oops` /
`panic` all 0 — a detected lockup just sits there). Each freeze therefore costs
a power cycle and yields "it stopped". **`docs/host-freeze-capture.md` has the
commands**: serial console out to the BMC's SOL so a trace survives a total
freeze, fatal-and-rebooting lockup detection, kdump, and the one untested
mechanism that fits concurrency — `amd_iommu=off`, since two cards DMA-ing
through the IOMMU is the only candidate that explains "only ever with Axelera
plus another card". Apply the capture *before* the next reproduction.

The SEL holds a `CPU_THERMTRIP` 86 s after the 09-20 journal ends, but that is
the hard power-off that *followed* the crash, not its cause; see the THERMTRIP
entry below.

The original entry:
Reproduced 2026-08-08 on the same `--automation` plan, twice from a cold power
cycle: the machine died — display lost, no shutdown, nothing written by the
kernel or by systemd — while `cores = 1` completed the identical eight-step plan
end to end. That is a strong correlation and **not** a mechanism; treat it as a
reason to keep the default at 2 rather than as an explanation.

What was ruled out, so it is not re-derived:
- **Not CPU thermal, not power.** The BMC's SEL has no entry at either crash —
  no `CPU_THERMTRIP`, no power event, nothing between the boot's own startup
  burst and the next boot's clock sync. (`sudo ipmitool sel elist`; install
  `ipmitool` first, it is not on this host by default.)
- **Not the driver's DMA cleanup.** `axl …: Previous async DMA transfer not
  cleaned up (state=4)` floods at teardown of a multi-instance run and looks
  damning, but it tracks Axelera *activity*, not crashes: two boots logged ~14k
  of them and shut down cleanly, and the *first* crash logged none. Same for
  `Failed to wait for MSI` and `Link down` — present only on boots that
  survived.
- **`IO_PAGE_FAULT` on `axl` is routine.** Exactly one per boot, on every boot,
  including cleanly shut down ones. It fires when the runtime loads firmware.

**Do not trust the CSV's or the journal's last timestamp as the crash time.**
Both crashes were first read as "died before YOLOv8m loaded", from the last row
in the log — which was wrong. Line buffering only reaches the page cache, and
journald fsyncs on an interval, so both tails predate a host crash by an unknown
margin. `Logger` now fsyncs every row for exactly this reason; the journal still
does not, so prefer the CSV.

**The SEL's `CPU_THERMTRIP` entries track HARD power-offs, not heat — the
"chronic thermal problem" this file used to describe is probably not one.**
Correlated 2026-09-21 against how each boot ended (clean = the journal reached
a shutdown target; hard = it stops mid-log):

| boot ended | how | THERMTRIP |
| --- | --- | --- |
| 09-20 09:53 / 10:38 / 11:22 | clean | none |
| **09-20 12:12** | **hard** (the 3rd "host crash") | **12:13:55** |
| 09-20 12:36 | clean | none |
| **09-21 09:06** | **hard** | **09:09:30** |
| 09-21 09:34 | clean, then remote power-off at 09:39 | none |

**2 of 2 hard offs, 0 of 5 clean.** Both trips land 1.5-3 min *after* the
journal's last line — a real trip would end the journal at the trip — and BMC
temperatures were normal each time. The 09:34 row is a deliberate test: a clean
OS shutdown, then power cut from the already-off machine, logs nothing. So the
entry needs power cut from a **running (or hung) CPU**.

**Two readings remain, and the evidence favours the first:** the BMC logs a
spurious THERMTRIP whenever a live CPU loses power; or a real trip killed the
machine. The decisive test is a remote power-off of a *running, healthy* OS
with no shutdown first — a THERMTRIP then proves it spurious. Not run yet (it
costs an unclean shutdown).

Consequences: **a THERMTRIP next to a crash is not the crash's cause** — the
09-20 12:12 one is the power-off that followed it, so that crash is still
unexplained; and the five trips recorded 2026-07-14 … 08-06 were probably the
same artifact. Idle temperatures are fine (CPU 40 °C, Card Side 46 °C); only
two chassis fans report, at 500 and 800 RPM, which is worth knowing but is no
longer evidence of anything.

**`PCI PERR` fires ~9 times at every POST**, clean boots included, before any
driver loads — a boot-time constant, never seen at a wedge. One decoded
(`ipmitool sel get 0x78`, event data `a4 c1 00`) points at bus `0xc1` dev 0
fn 0, which was **DeepX** that boot. Decode a few more before concluding it is
always the same slot; BDFs move across reboots.

**The Metis wedges after a load session and only a power cycle clears it.**
Symptom, from *any* client — this app, `axcmd`, or the vendor's own Voyager
pipeline:

```
[libaxldev_linux.c:812] USR_DMA_XFER failed: Connection timed out
[libaxldev.c:435] Failed to write ELF to device memory: Connection timed out
Failed to load runtime stage0: .../start_axelera_runtime_stage0.bin
Failed to initialise device: metis-0:<bdf>:0
kernel: axl 0000:..: DMA WR CH0 status -110      (-110 = ETIMEDOUT)
```

`axcmd --fwver` then reports `v1.3.2+bl1-stage0`. The card answers control-plane
commands (version, MSI, link training) but times out on the bulk DMA that
uploads firmware, so it can never leave bootloader.

Observed pattern (2026-08-04): after a power cycle the card allows **one**
successful load session; once that session ends — especially on an unclean exit
like Ctrl-C or a killed process — every later attempt fails as above.
**`modprobe -r metis && modprobe metis` does not clear it** (verified); neither
does a warm reboot. Only a full power-off.

**Do not diagnose this as an app bug.** Three wrong turns were taken before the
vendor pipeline was tried and failed identically:
1. Blamed `axcmd --fwload` from `tools/axelera-temps.sh`. The per-boot journal
   shows the upload path was already failing ~6 hours before that script first
   ran.
2. Blamed a `vmsi not available` / PCIe-BDF correlation. It fails at both `c2`
   and `c6`, including the address it last worked at.
3. Concluded "card-level fault, contact Axelera" — then the customer's Voyager
   cascade ran a 4-model pipeline on it minutes later.

The one cheap discriminator is **run the vendor pipeline**. If it fails too, it
is not our code. `journalctl -b -N -k | grep -c "detached for firmware reload"`
against DMA-error counts per boot is the fastest way to see the history.

Separately and genuinely ours: the old per-stream `axr_device_connect()` fired
several firmware reloads per run and would have accelerated the wedging. Fixed —
see **API modes**.

### Fixed — recorded so they are recognised, not re-debugged

| Symptom | Real cause | Guard |
|---|---|---|
| **Whole window freezes on "Stop benchmark"** (and once at startup, blocked in `memx_fops_open`) | `BenchEngine::stop()` joined the worker threads **on the GUI thread**. `stop_flag` is only tested *between* frames, so a worker blocked inside a vendor SDK call never reaches the check and the join never returns. Confirmed from `/proc/<pid>/task/*/wchan`: main thread in `futex_do_wait` (the join), one worker in `hailo_nnc_ioctl`, another in `do_msgrcv` (DXRT), total CPU delta 0 — a true deadlock. The startup freeze logged earlier as "probably stale device state" was the same bug with MemryX blocking; **that guess was wrong** | `stop()` signals and returns immediately; a detached reaper owns the workers (via `shared_ptr`, so they outlive the engine) and joins them. `stopping()` gates the Start button and drives a "Stopping — waiting for the device to release…" status. A card that never returns now leaks one thread instead of hanging the app. **Never join a worker from the GUI thread** |
| INA228 bridges missing after a reboot, fine after unplug/replug | A udev rule whose filename sorts **before** `50-udev-default.rules` is overwritten by it — `MODE` is a plain assignment, last wins — so `11-ftdi.rules`' `0666` loses to the default `0664` and the nodes come up `root:root` unopenable. The devices are tagged `seat` but **not** `uaccess`, so logind grants no ACL either | Name it `99-ftdi.rules`, then `udevadm control --reload` + `udevadm trigger --action=add --subsystem-match=usb` (no replug needed). Diagnose with `udevadm test /sys/bus/usb/devices/<port>`, which prints every `MODE` assignment and its source rule |
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
| Every open-loop step logged `ctl.state = 6 degraded`; `kOpen` (5) unreachable | The NaN check on the inferred estimate sat **after** the if/else instead of inside the `kInferred` branch — and `pv_tc` is NaN in the `kNone` branch too, so `if (isnan(pv_tc)) src = kNone` fired unconditionally. Nothing in the code read wrong. **Caught on hardware (bench test T6), not in review**, and critical: `ctl.state` 5-vs-6 is the *only* channel separating a deliberate open-loop arm from one that asked for the learned estimate and did not get it. Had it shipped, the 2026-11-23 run would have measured the same arm three times with no way to say so | Test moved inside the `kInferred` branch. **Bench-test every state a plan distinguishes arms by**, and distrust a provenance channel until a command has forced each of its values |
| The GUI's `NN` box ungreyed itself precisely when the board had NO model, and the whole Fan control row started fully live | Two bugs, one screenshot. (a) `fan_ctl_nn_available_ = s.nn \|\| s.state == 6` — with the estimate requested and no model the firmware reports `nn = 0, state = 6`, so that test was exactly inverted; and `state 6` never meant "a model exists" anyway, being equally what a lost thermocouple looks like. (b) the sensitivity pass lived inside `emit_fan_control()`, which only runs from a widget signal or from automation, so nothing greyed anything until the row was touched | (a) the board publishes `ctl.model_ready` (fw ≥ 0.7.0) and the host reads it instead of inferring an unobservable. (b) `refresh_fan_ctl_sensitivity()` split out and called from the constructor, beside `refresh_depth_sensitivity()` — **it must not emit**, or launching the app would send a fan command and stomp a running loop. **A control's initial sensitivity needs its own call at construction**; coupling it to the emit path means it only ever runs too late |

## Host specifics

**There are two development hosts, and most of this file describes the first
one.** Check `uname -m` before trusting a "this machine" claim below.

### The x86_64 four-card host

All four cards are present and **all four** inference SDKs build: Hailo-8, MemryX
MX3, DeepX M1 and Axelera Metis.

**Do not look up a BDF here — they move.** One boot had Hailo `0000:01:00.0`,
MemryX `0000:47:00.0`, DeepX `0000:c1:00.0`, Axelera `0000:c2:00.0`; on
2026-08-08 it was Hailo `01`, **DeepX `47`, MemryX `c1`**, Axelera `81` — the
middle pair exactly reversed. Those addresses appear in this file only as
verbatim quotes from the boot that produced them (the D3cold dmesg under
**Known issues** is one), never as constants. Resolve a card at the time you
need it:

```bash
for d in /sys/bus/pci/devices/*/; do
  drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)
  case "$drv" in hailo|dx_dma_pcie|memx_pcie_ai_chip|axl)
    echo "$(basename "$d")  $drv";; esac
done
```

`dx_dma_pcie` is DeepX and `memx_pcie_ai_chip` is MemryX — the pair most easily
confused. Two independent cross-checks, both visible in any log: **MemryX
reports a power reading and DeepX does not**, and their clocks differ
unmistakably — DeepX sits at **1000 MHz fixed**, MemryX is DVFS and tops out at
its configured **600**.

**PCIe BDFs are not stable across reboots here** — the Axelera card was observed
at `c6:00.0` and later `c2:00.0`, which also renames its device node
(`/dev/metis-0:c6:0` → `/dev/metis-0:c2:0`). Never hardcode either: discover the
node with a glob (`/dev/metis-*`), as `AxeleraProbe` and `tools/axelera-temps.sh`
both do, and find BDFs by PCI vendor id.

**MemryX is not telemetry-only** — an earlier version of this file said so, and
it was wrong. `mb-powermon-gui`'s note that the MemryX *power-telemetry* classes
are Python-only is true and unrelated; the **MxAccl inference runtime does ship a
linkable C++ library** (`memx-accl`, headers `/usr/include/memx/accl/`). Link
`libmx_accl`, **not** `libmemx` — the latter is the driver library and has none
of the `MxAccl` symbols.

**All four cards are instrumented with INA228 shunts** (`config/ina228.conf`:
`1-1.1` Hailo, `1-1.2` Axelera, `1-1.4.1` DeepX, `1-1.4.2` MemryX — the
four-part paths are bridges behind a second hub tier), so every card has real
fps/W and mJ/frame. This was not always true: DeepX and MemryX have no on-die
power sensor, and older notes describing DeepX as "no power" predate the third
and fourth shunts. Running `mb-powermon`/`mb-powermon-gui` at the same time
fights over the Hailo power DVM (`DVM_ALREADY_IN_USE`).

Rough numbers at max speed, for sanity-checking a change. Multiply by the
GOP/frame figures in **Work per frame** to turn any of these into achieved TOPS:

| | Hailo sync | Hailo async | DeepX sync | DeepX async | Axelera sync | Axelera async |
| --- | --- | --- | --- | --- | --- | --- |
| YOLOv8s | ~123 | ~490 | ~32 | ~148 | ~131 | ~154 |
| YOLOv8m | ~61 | ~67 | | | | |
| ResNet-50 | ~297 | ~1369 | ~393 | ~1087 | ~1967 | ~388 |

**Async is not a uniform multiplier.** YOLOv8m gains ~10% on Hailo where YOLOv8s
gains 4× — the medium model is NPU-compute-bound, so there is little transfer
latency left to hide. A small async gain is a property of the workload, not a
sign the async path is broken.

Axelera ResNet-50, re-measured 2026-08-04 through a direct libaxruntime probe
(one connection and one sub-device per instance): **358.7 / 574.0 / 658.6 /
694.9 fps**. Superseded 2026-09-03 by the fixed-batch path — **359 / 632 / 1154 /
1602** — and again 2026-09-20 by the property-separator fix, which lifts the
4-core figure to **1967**; the 1-core async mode measures **388**. See
**API modes**. The old figures counted a batched invocation as one
frame where the artifact was batched at all at 1 / 2 / 3 / 4 cores. The older 437 / 671 / 805 figures predate the
cores rework and were taken differently; prefer these. Anything quoting a
one-core Axelera number is measuring a quarter of the card.

The People Tracking pipeline roughly halves the YOLOv8s figures. Note DeepX
*leads* on ResNet-50 sync while trailing badly on YOLOv8s sync — its weak
YOLOv8s number is a mode artifact (4.6× in async), not a general handicap, so
don't "fix" it by special-casing the DeepX runner.

### The aarch64 Qualcomm host — Dragonwing IQ-9075 EVK

`uname -m` = `aarch64`, kernel `6.8.0-1080-qcom`, `/proc/device-tree/model` =
"Qualcomm Technologies, Inc. Addons IQ 9075 EVK" (QCS9075 / SA8775P, **HTP
v73**, `socModel` 77, **2 NSPs**, 8 MB VTCM each). Two accelerators here, not
four:

- **Hexagon NSP ×2**, on-SoC. QAIRT **2.46.0** is apt-installed from
  `ubuntu-qcom-iot/qcom-ppa`: headers in `/usr/include/QNN`, 44 × `libQnn*.so`
  in `/usr/lib`, and the V73 skels in **`/usr/lib/dsp/cdsp{,1}`** — *not*
  `/usr/lib/rfsa/adsp`, which the vendor docs name and which does not exist on
  this image. `MB_HAVE_QUALCOMM` lights up from the headers alone.
- **Hailo-8** at `0001:01:00.0` (`/dev/hailo0`), libhailort **4.24.0** in
  `/usr/lib` with headers in `/usr/include` — so `MB_HAVE_HAILO` builds here
  too and the board gives a genuine two-backend comparison. Note the paths:
  CMake's `/usr/local/*` search hits nothing, `/usr/*` is what matters.

DeepX / MemryX / Axelera are absent, and **`libftdi1` is not installed**, so
there are no INA228 rails. The SoC still exposes no power sensor of its own —
see the Qualcomm entry under telemetry, and **do not add an estimate to fill the
graph** — but a **ChargerLAB POWER-Z KM003C** is now inline on the board's USB-C
supply and `PowerZProbe` reads it, so this host does have real watts. They are
**whole-board** watts (CPU + GPU + NSP + DRAM + peripherals), not NPU watts, so
the per-card efficiency figures are still 0 by design: the probe's device name
matches no `accel_name()`, which is what keeps `power_for_device()` returning
NaN for Qualcomm rather than handing a card a number ~10x its real draw.

The user is in the `fastrpc` group already. If a fresh account is not,
`/dev/fastrpc-*` can be `stat()`ed but not opened — existence checks pass and
the DSP still refuses — so probe with `access(node, R_OK|W_OK)` and fix with
`usermod -aG fastrpc`.

Measured 2026-08-04 through the app's own headless driver, **burst, async,
2 NSPs, depth 4** (context binaries from `~/envic_models/local/ctx`):

| | fps | load |
| --- | ---: | ---: |
| ArcFace MobileFaceNet (`mbf`) | 10112 | 365 ms |
| OSNet x1.0 | 4576 | 375 ms |
| SCRFD-500M | 4325 | 402 ms |
| SCRFD-2.5G | 3504 | 378 ms |
| Face Recognition · SCRFD-500M | 2865 | 458 ms |
| Face Recognition · SCRFD-2.5G | 2471 | 470 ms |
| ResNet-50 | 2178 | 511 ms |
| SCRFD-10G | 1869 | 416 ms |
| Face Recognition · SCRFD-10G | 1527 | 476 ms |
| YOLOv8s (split) | 966 | 465 ms |
| People Tracking · YOLOv8s | 789 | 556 ms |
| YOLOv8m (split) | 507 | 529 ms |
| People Tracking · YOLOv8m | 454 | 631 ms |

Sanity checks these numbers pass, worth re-checking after any runner change: a
pipeline is slower than its slowest stage, SCRFD scales the right way with model
size (10G < 2.5G < 500M), and YOLOv8s is ~2x YOLOv8m.

**All 8 models and all 5 pipelines now run on this card.** For contrast on the
same board, Hailo-8 async depth 4: ResNet-50 801, YOLOv8s 359, OSNet 214 fps.

**Qualcomm artifacts are build products of this board, not downloads.** They are
context binaries locked to HTP v73, produced by `qnn-context-binary-generator`
via `~/envic_models/run_local.sh` (or `gen_ctx_bench.sh`) after AI Hub compiles
the `.dlc` — hence `fetch = none` and `qualcomm.source = local` on every entry.
The aarch64 packages ship **no converter and no quantizer** (`qairt-tools` is
runtime-only), so ONNX → `.dlc` cannot happen on this board; that step is AI Hub
or an x86 host. `qai_hub` 0.53.0 lives in `~/qaihub-venv`, not the system python.

**Quantize inside the compile job, not as a separate AI Hub job** — measured
2026-08-07 while adding ResNet-50, and it cost four round trips to find:

- `submit_quantize_job()` on ResNet-50 produced a QDQ ONNX with **168 tensors
  marked EXTERNAL that also carry inline `float_data`**. AI Hub's *own* compiler
  rejects it ("stored externally and should not have data field.float_data"), so
  the two-step path cannot complete at all for this model.
- The two copies are **not duplicates** — for `resnetv17_conv0_weight` the
  external copy has std 0.116047 against the inline copy's 0.116064, i.e. the
  inline data is a stale pre-quantization version. The ONNX spec makes external
  authoritative when `data_location` is EXTERNAL, so a repair must clear
  `float_data`, not `raw_data`.
- Repairing it lets the compile succeed, but the resulting DLC **will not
  compose on HTP**: `in[1].shape[0] and in[1].shape[1] must be [1,1]` /
  `Failed to validate op resnetv17_conv0_fwd`. That is a *shape* complaint, so
  it cannot be caused by which weight copy the repair kept.
- Not our toolchain either: **AI Hub's own profile job, on a real IQ-9075**,
  fails identically with `QnnModel_composeGraphsFromDlc: MODEL_GRAPH_ERROR`.
  Identical with and without `--force_channel_last_input`, so not layout.
- **The fix is `--quantize_full_type int8` on `submit_compile_job()`** with
  `calibration_data=`, skipping `submit_quantize_job()` entirely. Note the
  option takes `int8`, **not** `w8a8` (valid: int8, int16, float16, w8a16,
  w4a8, w4a16). The other five models predate this and used the two-step path
  successfully — it is not universally broken, but when a DLC will not compose,
  try this before suspecting the model.

**A compile job reporting SUCCESS is not evidence the artifact loads.** Both the
broken ResNet-50 DLCs compiled cleanly and only failed at `qnn-context-binary-
generator`. `gen_ctx_bench.sh` composes on the board as part of the build for
exactly this reason; keep any new recipe doing the same.

### Driving the GUI

**Ask before launching or killing the app.** It runs on the user's own display;
they will be using it. `pkill`-ing it out from under them mid-test has happened
and is not acceptable. Prefer the headless drivers below — they need no display
and no GUI instance, only the device to be free.
