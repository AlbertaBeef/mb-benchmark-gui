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
- **`Probes.{h,cpp}` has diverged** — 1259 lines here against 1107 there. The
  deliberate edits are the INA228 config-path search (see **Config**) and the
  added `Probes::power_for_device()`, which needs `Catalog.h` and cannot compile
  in the sibling. **Never `cp` this one**; port hunk by hunk. Doing otherwise
  breaks that build, which has already happened once.

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

- **`qnnruntime.{h,cpp}`** — **vendored, not written here.** A flat C-ABI shim
  over the QNN C API, copied from
  `../envic_ai_aerex/envic/src/components/accelerator/qnn/`. The `.cpp` is kept
  **byte-identical** to that copy so fixes can move either way with `cp`; `cmp`
  it after touching anything. The `.h` differs *only* in its header comment
  (the original describes a Vala caller). QNN is never linked — the shim
  `dlopen`s a backend library and asks it for an interface struct, which is why
  CMake needs only headers and `libdl`.
- **A card the build has no backend for is hidden completely.** `accel_present()`
  in `Catalog.h` is the single question every UI site asks: no Inference
  checkbox, no settings tab, no Graphs filter checkbox, no legend entry, no
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
  - **`accel_present()` is build-time only** (`accel_compiled_in()`). It
    deliberately does *not* detect a compiled-in card whose hardware is
    unplugged: that needs each vendor's enumeration API on the GUI thread at
    startup, and a false negative would hide a working card — worse than showing
    one that then reports a clear load error. Extend there, not at the call
    sites.
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
  (Hailo/MemryX/DeepX/Axelera/Qualcomm, order fixes every graph's series order)
  plus an INI reader for `config/models.conf`. **Append to that enum, never
  insert** — `accel_index()`/`accel_at()` are raw casts, so a card's position is
  what indexes `watts[]`, `Catalog::vendors_[]` and every graph series.
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
  (`Hailo, MemryX, DeepX, Axelera`) fixes the series order on the three
  benchmark graphs, the checkbox row and the card tabs. The **Power /
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
- **Graph span is 10 minutes** (`kSpanSeconds = 600` → `kHistory = 601` samples
  at 1 Hz). `GraphArea` derives its time-label step from the span — the largest
  of {10,15,30,60,120,300,600} s that still leaves ~5+ divisions — so a 60 s
  window keeps 10 s marks while 600 s gets 2 min marks instead of sixty
  gridlines. Deriving it from the span rather than hardcoding it is what keeps
  `GraphArea.cpp` byte-identical to `mb-powermon-gui`'s copy — both apps now pass
  600, but the widget doesn't care. Beware: `draw()` already has a `step` for the
  trace x-spacing, hence `label_step`.
- **The axis top is a baseline plus a policy.** Percent, `fixed_max` (the 100 °C
  temperature axis) and auto-with-a-`min_axis_max`-floor each supply a *baseline*
  top; `GraphArea::RangeMode` decides what the data does to it, and the **Graphs
  → Range** control drives all six graphs at once (mixing modes between graphs
  would have them answering different questions simultaneously):
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
- **The Graphs / Accelerators filter hides series and legend entries; it does
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
  the other two" has to be expressible. `graph_accel_mask()` returns a bit per
  card in `Accel` order. Unticking everything legitimately blanks the graphs;
  that is left alone rather than silently re-showing all, which would make the
  control lie about its own state.

  Mapping differs by graph family, and both mappings already exist: the three
  telemetry graphs carry one series per `MetricInfo` and are matched on
  `device_name` (so a mapped INA228 folded onto a card is kept with it), while
  the three benchmark graphs are one series per card in `Accel` order. A metric
  belonging to no card at all — an unmapped INA228, the board ambient sensor —
  has no bit to consult and stays visible.

  **The legend entry is hidden with the trace**, not left running. The handles
  for that are collected at build time — `AccelSection::cell[]` for the
  benchmark graphs, `LegendRow{device, widgets}` for the telemetry ones, where a
  row is the device-name label plus its aggregate label plus its per-metric
  cells. They have to be captured during construction because the telemetry
  legend is a `Gtk::Grid` laid out by device, with nothing to walk back to
  afterwards.

  **Qualcomm is deliberately one of those unmatched metrics, and this is the one
  place a fifth card is not seamless.** `accel_name(Accel::Qualcomm)` is the
  stable string `"Qualcomm"` — it labels a checkbox, a settings tab and a graph
  series, so it cannot vary by board — while `QualcommIQProbe` names its thermal
  rows after the device tree (`IQ9075 N0-0` …), because a temperature row wants
  to say *which board*. They therefore never string-match, so unticking Qualcomm
  in Graphs/Accelerators hides its benchmark series but leaves its six NSP
  thermal traces on the Temperature graph. That was the accepted cost of a
  stable name; the alternatives were renaming the probe (which would change what
  the shared `mb-powermon-gui` shows on this board) or making `accel_name()`
  read the device tree. `power_for_device("Qualcomm")` returning NaN is likewise
  *correct*, not a bug — this board has no power sensor at all.
- **Axelera AIPU cores defaults to 2 — deliberately not 4.** One core leaves the
  others at 50 MHz, which is not a representative Metis figure, so the default
  wants to be high; but claiming *all four* wedges this card (all four MSI
  vectors time out, the PCIe link drops, power-off to recover — see Known
  issues). 3 is the highest value observed stable, and the shipped default sits
  one below it so a fresh checkout cannot cost someone a power cycle. Raising it
  is a deliberate act. This control replaced an earlier "Streams" knob — see the
  API-modes section for why that was wrong.
- **Graph order is Power, Temperature, Frequency, Frame Rate, Efficiency,
  Energy.** The three telemetry graphs lead because they are live with no run in
  progress; Frequency sits under Temperature because a clock sagging as a die
  heats is the whole reason to plot it. Frequency and Energy are collapsed by
  default.
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
- **The three benchmark legends are one row of up to four cards** (wrapping at
  four via `i % 4, i / 4`) and carry no explanatory text. Failures and loading
  state therefore go to the status line in `on_tick` — do not let a card sit
  silently at 0, which is how a broken model looks exactly like a slow one.

## Telemetry families (Probes)

`Probes` publishes **three** flat metric families — temperature, power and
frequency — each with an aligned value vector refreshed by `poll()`. Power alone
uses `power_device_order()` and the INA228 folding; temperature and frequency
follow plain discovery order, because a clock always belongs to the card
reporting it and there is nothing to fold.

**All four** cards report a core clock, each from a different place:

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

**`Probes.{h,cpp}` is shared with `mb-powermon-gui`** — the frequency family is a
new addition here and has *not* been ported there yet.

## API modes (Sync / Async)

**The mode is per card, chosen in that card's tab — not once for the whole run.**
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
| Axelera | `axr_run_model_instance()` | **none exists** |
| Qualcomm | `QnnGraph_execute()` | **none exists** |

`accel_has_both_api_modes()` drives the UI: Hailo, DeepX and MemryX get radios;
Axelera and Qualcomm get a static line from `accel_sole_api_mode_note()` instead,
so the UI never offers a mode the runtime does not have. Neither switch has a
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
- **Measured after the split** (ResNet-50, this host): Hailo 298.1 / 1217.2,
  DeepX 384.0 / 1085.5, MemryX **259.9** / 1104.4 (sync / async). The MemryX sync
  figure is new — the old emulated one is not comparable and should not be quoted
  against it.
- **Axelera really has no async API.** All 41 `axr_*` symbols enumerated; exactly
  one is inference and it blocks. The 233 `ze*` symbols are the statically linked
  oneAPI Level Zero loader — headers, pkg-config and CMake are all shipped, but it
  is model-agnostic, so driving inference through it means reimplementing
  `axr_load_model` against internal types. "Async" there sets the runtime's own
  `double_buffer=1` property, worth ~7-18%.
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
  PCIe link down. The control is now **AIPU cores (1-4, default 2)** — an honest
  name for what it delivers — and the ordering of the connects is what was
  actually wrong, not their number. Teardown must stop *and join* any instance
  threads before destroying an instance, and release `Stage::conn` only after
  every instance on it is gone.

  **A model instance occupies exactly ONE AI core, whatever `num_sub_devices`
  the connection asked for.** Measured 2026-08-04 on ResNet-50 (async), clocks
  sampled *mid-run* rather than after:

  | connections × instances | `num_sub_devices` | aicore0..3 during run | fps |
  | --- | --- | --- | --- |
  | 1 × 1 | 4 | `800 · 50 · 50 · 50` | 375.6 |
  | 1 × 4 | 4 | — (`Failed to wait for MSI`) | 201.7 |
  | **4 × 1** | **1 each** | **`800 · 800 · 800 · 800`** | **694.9** |

  So asking one connection for four cores buys nothing — the other three stay
  parked at 50 MHz — and stacking four instances on one connection makes them
  collide on the command queue. **N cores busy means N connections, one instance
  and one sub-device each.** That is why the runner sets `instances_ = cores_`
  and gives every instance its own connection.

  **The prebuilt zip ships four compilations of every model — 1, 2, 3 and 4
  cores — and taking the 1-core one is correct.** `config/models.conf` has
  `strip = build/%m/%m/1`, which looked like an oversight and is not. The
  compiler moves constants from DDR into L2 as the core count rises:

  | build | kernel_function*.c | L2 const | DDR const |
  | --- | --- | --- | --- |
  | 1-core | 1 | 7.6 MB | 20 MB |
  | 2-core | 2 | 16 MB | 12 MB |
  | 3-core | 3 | 23 MB | 4.3 MB |
  | 4-core | 4 | 28 MB | none |

  That looks like it should be faster, and for *latency* it is — one frame is
  spread across N cores. For **throughput it is worse**, measured 2026-08-04 on
  ResNet-50 with both configurations occupying exactly two cores (confirmed by
  sampling `axcmd --clock-all-actual` mid-run, `aicore0+1` at 800 MHz in both):

  | | fps |
  | --- | ---: |
  | 2-core build, `num_sub_devices=2`, 1 instance | 317.6 |
  | **1-core build, `num_sub_devices=1`, 2 instances** | **574.0** |

  1.81x in favour of N independent instances. The 2-core build on two cores is
  even slower than the 1-core build on *one* (358.7). Don't "fix" the strip glob.

  **Scaling of the shipped arrangement** (1-core build, one connection and one
  sub-device per instance), same session:

  | instances / cores busy | 1 | 2 | 3 | 4 |
  | --- | ---: | ---: | ---: | ---: |
  | fps | 358.7 | 574.0 | 658.6 | 694.9 |

  Strongly sublinear — 1.94x for 4x the cores, saturating around three. That is
  a property of the card, not of this runner.

  **The vendor's 2054 fps on ResNet-50 remains unexplained**, and it is a real
  measurement: it comes from Voyager's `End2EndTracer`, which reports
  `1 / mean(inter-frame interval)` with **no multiplier** (unlike `AipuTracer` /
  `HostTracer`, which do multiply by core count — don't confuse the two, they
  live in different modules with the same class names). Ruled out so far: a
  second API, batching (both builds are batch 1), and the multi-core builds. The
  untested difference is `--enable-opencl` with real video streams, where the
  pipeline may feed the NPU through **dmabuf** — `input_dmabuf` / `output_dmabuf`
  are real instance properties we do not use.

  The cold-card race is handled by *ordering*, not by avoiding connections: the
  **first** connect is made alone and allowed to complete (it is the one that
  uploads the 3.8 MB firmware ELF), and only then are the rest opened.
- **MemryX's clock knob is Python-only.** `set_mpu_frequency(dev, group, MHz)`
  exists in the `mxa` module and nowhere in `memx.h` or `MxAccl`, so
  `configure()` shells out to the venv interpreter once at load. ~4 s of Python
  startup, alongside model loading, never in the timed loop.
- **Axelera really has no async API.** 41 `axr_*` entry points, exactly one is
  inference, and it blocks. The `zeCommandQueue*` symbols in `libaxruntime.so`
  are Level Zero internals (the runtime is built on oneAPI L0), not public API.
  "Async" there sets the runtime's own `double_buffer=1` property — worth only
  ~7–18%. Don't "improve" this by fanning out host threads; that would be
  fabricating a number the API does not offer.
- **MemryX really has no blocking call.** Its whole public surface is ctor,
  `connect_stream`, `start`, `wait`, `stop`, `set_num_workers`,
  `get_num_streams`. Sync is emulated by permitting one frame in flight.
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
  defaults to 2** (unlike Axelera's cores there is no cliff — nothing wedges), and
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
- **Depth is per card, in that card's tab** (`BenchItem::depth`, was a single
  global "Threads" control). Every backend has some form of frames-in-flight, but
  the mechanism and the useful range differ, so the range is per card:

  | card | what depth sets | range | gated on API mode? |
  | --- | --- | --- | --- |
  | Hailo | requested async queue depth, `min(requested, get_async_queue_size())` | 1-8 | yes — Sync forces 1 |
  | DeepX | outstanding `RunAsync` jobs | 1-8 | yes — Sync forces 1 |
  | MemryX | `connect_stream` permit depth | 1-8 | yes — Sync uses `MxAcclMT::run()` |
  | Axelera | `double_buffer` — a **boolean**, so 2 is as deep as it goes | 1-2 | **no** |
  | Qualcomm | engines in flight per NSP, one host thread each | 1-8 | **no** |

  **Axelera's 1-2 is not an arbitrary cap — 2 is the ceiling the API has.**
  Verified 2026-08-04 against the shipped library, because "why not 1-8?" is an
  obvious question:
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

  So a 1-8 range would have 3..8 silently behave as 2, and a run could later be
  recorded as "Axelera at depth 8" when no such thing happened. Measured ladder,
  one core: no property 362.2 fps, `double_buffer=1` 379.6. At two cores:
  573.7 → 589.6.

  **The two sync-only cards deliberately ignore the API mode here.** Axelera and
  Qualcomm show no mode radios (they have no async API), so gating depth on
  "Async" would leave it permanently at 1 and silently drop the concurrency each
  one *does* have — Axelera's double buffering and Qualcomm's measured ~1.45x
  host pipelining. Both regressed exactly that way when the mode control first
  moved into the tabs; depth is now independent for them.

  A card with both modes reports depth 1 in Sync regardless of where its spin
  button sits, because that is what Sync *is*, and its spin button greys out.

  Measured on ResNet-50: DeepX 1 → 402.2 fps, 4 → 1085.1, 8 → 1061.6 — depth pays
  until the device saturates and nothing after, which is why 4 is the default.
  Axelera 1 → 573.7, 2 → 589.6.

  `describe()` reports it uniformly as `· depth N`. It used to say
  `· N in flight`, and Axelera said `· double-buffered`; both are gone.

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

`axelera-temps.sh` + `axelera-temps.service` are now only for the **headless**
case. In the GUI both gates clear themselves:

- **App firmware** — the runtime loads it on the first `axr_device_connect()`,
  i.e. the first Axelera benchmark. Nothing else may load it; see the
  firmware-load note below.
- **Collector level** — `AxeleraProbe::read_temps()` raises it to
  `inf:collector` once, on seeing firmware present but no `core_temps`. The flag
  resets when it next sees a version mismatch, so a reboot or a `modprobe -r
  metis` re-arms it. `$MB_AXELERA_NO_COLLECTOR=1` opts out.

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

**Axelera: claiming all 4 AIPU cores wedges the card; 1-3 are stable.**
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

Worth testing before believing any software explanation: `Link down` under
four cores at 800 MHz is what a **power** limit looks like, and this host has an
INA228 on the Axelera rail (`config/ina228.conf`, `1-1.2`). Watch that trace in
`mb-powermon-gui` across a 3-core and a 4-core run — if the rail collapses at the
link drop, this is an M.2 slot power-budget problem, not a driver bug.

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

## Host specifics

**There are two development hosts, and most of this file describes the first
one.** Check `uname -m` before trusting a "this machine" claim below.

### The x86_64 four-card host

All four cards are present and **all four** inference SDKs build: Hailo-8
`0000:01:00.0`, MemryX MX3 `0000:47:00.0`, DeepX M1 `0000:c1:00.0`, Axelera Metis
`0000:c2:00.0`.

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

Axelera ResNet-50, re-measured 2026-08-04 through a direct libaxruntime probe
(one connection and one sub-device per instance): **358.7 / 574.0 / 658.6 /
694.9 fps** at 1 / 2 / 3 / 4 cores. The older 437 / 671 / 805 figures predate the
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
there are no INA228 rails — every efficiency and energy figure on this host is
legitimately 0. See the Qualcomm entry under telemetry for why no power number
exists here at all; **do not add an estimate to fill the graph.**

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
