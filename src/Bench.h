// The benchmark engine: one worker thread per card, each looping inference on
// its own device as fast as it can (or paced to a target frame rate).
//
// Deliberately kept off the GUI thread and lock-free on the hot path: a worker
// only ever bumps a free-running atomic frame counter, and the GUI derives the
// frame rate from that counter's delta on its 1 Hz tick.
//
// The independence has to hold in BOTH directions, and originally it did not.
// No worker waits on the UI — but `stop()` used to join the workers, so a card
// wedged inside a vendor SDK call froze the whole window on "Stop benchmark".
// `stop()` now signals and returns; a detached reaper does the joining.
//
// No GTK dependency.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Catalog.h"

// Which of a vendor's two inference APIs to drive.
//
// The four SDKs are not symmetric here, and the UI must not pretend otherwise:
// Hailo and DeepX ship both a blocking call and a real async/job API; Axelera
// exposes only a blocking `axr_run_model_instance` (its sole concurrency knob
// is the runtime's own `double_buffer`); MemryX exposes only the streaming
// callback API and has no blocking call at all. So in each mode exactly one
// backend is emulated, and `api_mode_note()` says which.
enum class ApiMode { Sync, Async };

const char* api_mode_name(ApiMode m);
// True when the backend implements `m` with a native SDK API.
bool api_mode_is_native(Accel a, ApiMode m);
// Short qualifier for the legend when a mode is not native, else "".
const char* api_mode_note(Accel a, ApiMode m);

// One backend's inference loop, minus the threading. Implemented per SDK in
// bench_<backend>.cpp and compiled out when that SDK is absent.
//
// Lifetime note: a runner is created, used and destroyed entirely on its own
// worker thread — several of these SDKs keep thread-affine state.
class BenchRunner {
public:
    virtual ~BenchRunner() = default;

    // Per-card settings from the accelerator tabs (Axelera streams, MemryX
    // clock). Applied before load(); default is "leave the device alone".
    virtual void configure(const BenchItem&) {}

    // Open the device and load every stage. Throws std::runtime_error on
    // failure (message goes to the UI verbatim).
    virtual void load(const std::vector<BenchMember>& members) = 0;

    // Retire exactly one benchmark frame: every stage, `reps` times each, in
    // order. An async runner keeps its pipeline full and blocks until one
    // frame *completes*, so one call still means one frame either way and the
    // engine's counter stays honest in both modes.
    virtual void run_frame() = 0;

    // Short description of what got loaded (input geometry), for the status
    // line — e.g. "640x640x3 uint8".
    virtual std::string describe() const { return {}; }
};

// Returns nullptr when the backend was not compiled in; *err says why.
std::unique_ptr<BenchRunner> make_runner(Accel a, ApiMode mode, std::string* err);

// Host-side environment some backends need before any worker starts (today:
// putting Axelera's RISC-V toolchain on PATH). Call once from the main thread —
// it uses setenv(), which must not race with the workers' getenv().
void prepare_backend_environment();

class BenchEngine {
public:
    enum class State { Idle, Loading, Running, Failed, Stopped };

    // One running (or failed) benchmark: a subject on a card.
    struct Series {
        std::string label;  // subject name, e.g. "YOLOv8s"
        Accel accel = Accel::Hailo;
        State state = State::Idle;
        std::string error;    // set when state == Failed
        std::string detail;   // runner->describe(), once loaded

        double fps = 0.0;               // most recent sampled rate
        std::uint64_t frames = 0;       // frames since start
        double fps_per_w = 0.0;  // instantaneous fps / watts
        // Raw per-sample energy cost: this second's watts / this second's fps.
        // SI joules; the UI shows millijoules, the readable scale for these
        // parts. Unlike every other series on screen, LOWER IS BETTER.
        double joules_per_frame = 0.0;
    };

    // Both defined in Bench.cpp: Worker is incomplete here, so the vector of
    // unique_ptr<Worker> can only be constructed/destroyed where it is defined.
    BenchEngine();
    ~BenchEngine();

    // Start one worker per item. target_fps <= 0 means "run flat out".
    void start(const std::vector<BenchItem>& items, double target_fps, ApiMode mode);
    // Signal every worker to stop. Returns **immediately** — the join happens
    // on a detached reaper thread. A worker only notices `stop_flag` between
    // frames, so joining here would freeze the caller until the device came
    // back, which for a wedged card is never. Safe to call when idle.
    void stop();

    bool active() const { return active_; }
    // Workers signalled but not yet joined. Start must stay disabled until this
    // is false, or a new run would race the old one for the device.
    bool stopping() const { return pending_stops_->load() > 0; }
    double target_fps() const { return target_fps_; }
    ApiMode mode() const { return mode_; }

    // Refresh the per-series numbers. `dt_s` is the elapsed wall time since the
    // previous call; `watts[i]` is the power drawn by accelerator i right now
    // (NaN when that card reports none), used for the efficiency figures.
    void sample(double dt_s, const double watts[kAccelCount]);

    const std::vector<Series>& series() const { return series_; }

private:
    struct Worker;
    std::vector<std::unique_ptr<Worker>> workers_;
    // Shared with the reaper thread so the count outlives this object if a
    // worker never comes back.
    std::shared_ptr<std::atomic<int>> pending_stops_ =
        std::make_shared<std::atomic<int>>(0);
    std::vector<Series> series_;
    bool active_ = false;
    double target_fps_ = 0.0;
    ApiMode mode_ = ApiMode::Sync;
};
