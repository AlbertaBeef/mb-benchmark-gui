#include "Bench.h"

#include <chrono>
#include <cmath>
#include <exception>

namespace {
using clk = std::chrono::steady_clock;
}

// A worker owns one device for the duration of a run. `frames` is the only
// thing the GUI thread reads on the hot path.
struct BenchEngine::Worker {
    BenchItem item;
    double target_fps = 0.0;
    ApiMode mode = ApiMode::Sync;

    std::atomic<std::uint64_t> frames{0};
    std::atomic<int> state{static_cast<int>(State::Loading)};
    std::atomic<bool> stop_flag{false};

    std::mutex info_mu;   // guards error/detail, written once by the worker
    std::string error;
    std::string detail;

    std::thread th;
    std::uint64_t last_frames = 0;  // GUI-thread only, for the rate delta

    void set_failed(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(info_mu);
            error = msg;
        }
        state.store(static_cast<int>(State::Failed), std::memory_order_release);
    }

    void run() {
        std::string err;
        std::unique_ptr<BenchRunner> runner = make_runner(item.accel, mode, &err);
        if (!runner) { set_failed(err.empty() ? "backend unavailable" : err); return; }

        try {
            runner->load(item.members);
        } catch (const std::exception& e) {
            set_failed(e.what());
            return;
        } catch (...) {
            set_failed("unknown error while loading");
            return;
        }

        {
            std::lock_guard<std::mutex> lk(info_mu);
            detail = runner->describe();
        }
        state.store(static_cast<int>(State::Running), std::memory_order_release);

        // Pacing uses absolute deadlines off a fixed origin so a target rate
        // doesn't drift. If the device can't keep up and we fall far behind,
        // the origin is rebased — otherwise a temporary stall would be repaid
        // as a burst of un-paced frames once it clears.
        const double period = target_fps > 0.0 ? 1.0 / target_fps : 0.0;
        auto origin = clk::now();
        std::uint64_t n = 0, since_origin = 0;

        while (!stop_flag.load(std::memory_order_relaxed)) {
            try {
                runner->run_frame();
            } catch (const std::exception& e) {
                set_failed(e.what());
                return;
            } catch (...) {
                set_failed("unknown error during inference");
                return;
            }
            ++n;
            ++since_origin;
            frames.store(n, std::memory_order_relaxed);

            if (period > 0.0) {
                const auto deadline =
                    origin + std::chrono::duration_cast<clk::duration>(
                                 std::chrono::duration<double>(since_origin * period));
                const auto now = clk::now();
                if (now < deadline) {
                    std::this_thread::sleep_until(deadline);
                } else if (now - deadline > std::chrono::seconds(1)) {
                    origin = now;          // can't keep up — stop accumulating debt
                    since_origin = 0;
                }
            }
        }
        state.store(static_cast<int>(State::Stopped), std::memory_order_release);
    }
};

BenchEngine::BenchEngine() = default;
BenchEngine::~BenchEngine() { stop(); }

void BenchEngine::start(const std::vector<BenchItem>& items, double target_fps,
                        ApiMode mode) {
    stop();
    if (items.empty()) return;

    target_fps_ = target_fps;
    mode_ = mode;
    workers_.clear();
    series_.clear();
    workers_.reserve(items.size());
    series_.reserve(items.size());

    for (const auto& it : items) {
        auto w = std::make_unique<Worker>();
        w->item = it;
        w->target_fps = target_fps;
        w->mode = mode;

        Series s;
        s.label = it.label;
        s.accel = it.accel;
        s.state = State::Loading;
        series_.push_back(std::move(s));

        Worker* raw = w.get();
        workers_.push_back(std::move(w));
        // Started last so the Worker is fully constructed and owned first.
        raw->th = std::thread([raw] { raw->run(); });
    }
    active_ = true;
}

void BenchEngine::stop() {
    for (auto& w : workers_) w->stop_flag.store(true, std::memory_order_relaxed);
    for (auto& w : workers_) {
        if (w->th.joinable()) w->th.join();
    }
    workers_.clear();
    // Series are kept so the UI can still show why a run failed, but their
    // live numbers drop to zero: an idle card does zero frames per second.
    for (auto& s : series_) {
        if (s.state != State::Failed) s.state = State::Stopped;
        s.fps = 0.0;
        s.fps_per_w = 0.0;
        // Zero this too: with the traces never reset, a retained value would
        // keep drawing as a flat line and read as an ongoing measurement.
        s.joules_per_frame = 0.0;
    }
    active_ = false;
}

void BenchEngine::sample(double dt_s, const double watts[kAccelCount]) {
    if (dt_s <= 0.0) dt_s = 1.0;

    for (size_t i = 0; i < workers_.size() && i < series_.size(); ++i) {
        Worker& w = *workers_[i];
        Series& s = series_[i];

        s.state = static_cast<State>(w.state.load(std::memory_order_acquire));
        if (s.state == State::Failed || s.detail.empty()) {
            std::lock_guard<std::mutex> lk(w.info_mu);
            if (s.state == State::Failed) s.error = w.error;
            if (s.detail.empty()) s.detail = w.detail;
        }

        const std::uint64_t now = w.frames.load(std::memory_order_relaxed);
        const std::uint64_t delta = now - w.last_frames;
        w.last_frames = now;
        s.frames = now;
        s.fps = (s.state == State::Running) ? static_cast<double>(delta) / dt_s : 0.0;

        // Both efficiency views are raw per-sample figures, and both need the
        // watts of the card that produced the frames — a card with no power
        // sensor gets neither. They are exact reciprocals of one another;
        // having both is a units choice, not two different measurements.
        const double p = watts[accel_index(s.accel)];
        const bool have_power = !std::isnan(p) && p > 0.0;
        s.fps_per_w = have_power ? s.fps / p : 0.0;
        s.joules_per_frame = (have_power && s.fps > 0.0) ? p / s.fps : 0.0;
    }
}
