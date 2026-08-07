// MemryX MX3 benchmark runner (MxAccl / MxAcclMT C++ runtimes).
//
// BOTH modes are native here, but they live on two different classes — which is
// why an earlier version of this file claimed MemryX had no blocking call and
// emulated Sync with a permit counter. It is the *class* that lacks one, not
// the SDK (verified against /usr/include/memx/accl/, memx-accl 2.2.5):
//
//   Async — MxAccl::connect_stream(in_cb, out_cb) + start(). The runtime drives
//           the callbacks from its own worker threads; depth is the Threads
//           control, applied as a permit counter in the input callback (parking
//           a worker is the intended way to throttle; returning false would
//           tear the stream down).
//   Sync  — MxAcclMT::run(in, out, model, stream, timeout). Blocks until the
//           frame completes. No start()/stop() lifecycle — construct, run,
//           destruct — and it serialises internally on a private mutex, which
//           is exactly the one-frame-at-a-time semantics we want.
//
// Two caveats the SDK documents about MxAcclMT::run(): it is a synchronous
// *facade* over an async internal pipeline, so it is not a clean single-frame
// latency probe; and MxAccl::stop() is a deprecated no-op in 2.2.5 (it links
// and does nothing), so teardown must not depend on it.
//
// MemryX takes float input, unlike the uint8/int8 of the other cards — that is
// the SDK's contract, and the buffers are filled once at load, not per frame.
#include "memx/accl/MxAccl.h"
#include "memx/accl/MxAcclMT.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "Bench.h"
#include "bench_input.h"

namespace {

// Same search order as Probes' telemetry helper: an explicit override, then the
// conventional venv location.
std::string find_memryx_python() {
    if (const char* e = std::getenv("MB_MEMRYX_PYTHON")) {
        if (::access(e, X_OK) == 0) return e;
    }
    if (const char* home = std::getenv("HOME")) {
        const std::string p = std::string(home) + "/mb-edgeai/memryx-env/bin/python";
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

// set_mpu_frequency lives only in the Python `mxa` module — neither memx.h nor
// MxAccl exposes it — so this is a one-shot interpreter call at load time rather
// than an API call. ~4 s of Python startup, once per run, alongside model
// loading; never in the timed loop. Shared by both runners: the clock is a
// property of the card, nothing to do with which API drives it.
void apply_mpu_clock(int freq_mhz) {
    if (freq_mhz <= 0) return;
    const std::string py = find_memryx_python();
    if (py.empty()) return;
    const std::string script =
        "import sys\n"
        "from memryx import mxa\n"
        "f=int(sys.argv[1])\n"
        "n=int(mxa.get_total_chip_count(0))\n"
        "[mxa.set_mpu_frequency(0,g,f) for g in range(n)]\n";
    const std::string cmd = "timeout 30 " + py + " -c '" + script + "' " +
                            std::to_string(freq_mhz) + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) { /* best effort — the clock stays where
        it was, and the Frequency graph shows that plainly rather than us
        pretending it changed */ }
}

class MemryXAsyncRunner : public BenchRunner {
public:
    MemryXAsyncRunner() : depth_(kAsyncDepth) {}

    ~MemryXAsyncRunner() override { shutdown(); }

    void configure(const BenchItem& item) override {
        if (item.depth >= 1) depth_ = static_cast<size_t>(item.depth);
        apply_mpu_clock(item.freq_mhz);
    }

    void load(const std::vector<BenchMember>& members) override {
        for (const auto& m : members) {
            auto st = std::make_unique<Stage>(depth_);
            st->reps = m.reps > 0 ? m.reps : 1;
            st->accl = std::make_unique<MX::Runtime::MxAccl>(m.path);

            // One stream, driven by our two callbacks.
            st->accl->connect_stream(
                [st = st.get()](std::vector<const MX::Types::FeatureMap*> maps,
                                int) { return st->on_input(maps); },
                [st = st.get()](std::vector<const MX::Types::FeatureMap*> maps,
                                int) { return st->on_output(maps); },
                /*stream_id=*/0);
            st->accl->start();

            if (describe_.empty()) describe_ = "depth " + std::to_string(depth_);
            stages_.push_back(std::move(st));
        }
        // Geometry is only known once the runtime has handed us a FeatureMap,
        // so the first frame fills in the description.
    }

    void run_frame() override {
        for (auto& st : stages_) {
            for (int r = 0; r < st->reps; ++r) st->wait_one();
        }
        if (!shape_done_ && !stages_.empty()) {
            const std::string s = stages_.front()->shape_text();
            if (!s.empty()) {
                describe_ = s + " float32 · " + "depth " + std::to_string(depth_);
                shape_done_ = true;
            }
        }
    }

    std::string describe() const override { return describe_; }

private:
    static constexpr size_t kAsyncDepth = 4;

    struct Stage {
        explicit Stage(size_t depth) : permits(depth) {}

        std::unique_ptr<MX::Runtime::MxAccl> accl;
        std::vector<std::vector<float>> in_bufs;  // one per input map
        std::string shape;
        int reps = 1;

        std::mutex mu;
        std::condition_variable cv;
        size_t permits = 1;    // submissions still allowed in flight
        size_t completed = 0;  // frames retired but not yet consumed
        bool stopping = false;

        // Called by an MxAccl worker when the runtime wants a frame.
        bool on_input(const std::vector<const MX::Types::FeatureMap*>& maps) {
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [this] { return permits > 0 || stopping; });
                if (stopping) return false;  // tears the stream down
                --permits;
                if (in_bufs.empty()) {
                    in_bufs.resize(maps.size());
                    for (size_t i = 0; i < maps.size(); ++i) {
                        size_t n = 1;
                        for (auto d : maps[i]->shape()) n *= static_cast<size_t>(d);
                        in_bufs[i].resize(n ? n : 1);
                        fill_pattern(in_bufs[i].data(), in_bufs[i].size() * sizeof(float));
                    }
                    shape = shape_of(*maps[0]);
                }
            }
            for (size_t i = 0; i < maps.size() && i < in_bufs.size(); ++i) {
                maps[i]->set_data(in_bufs[i].data());
            }
            return true;
        }

        // Called by an MxAccl worker when a frame is done. Outputs are left
        // alone — copying them out would be host post-processing, which none of
        // the other backends do in the timed loop either.
        bool on_output(const std::vector<const MX::Types::FeatureMap*>&) {
            std::lock_guard<std::mutex> lk(mu);
            ++permits;
            ++completed;
            cv.notify_all();
            return true;
        }

        // Block until one frame has retired.
        void wait_one() {
            std::unique_lock<std::mutex> lk(mu);
            if (!cv.wait_for(lk, std::chrono::seconds(10),
                             [this] { return completed > 0 || stopping; })) {
                throw std::runtime_error("MemryX inference timed out");
            }
            if (stopping) return;
            --completed;
        }

        void begin_stop() {
            {
                std::lock_guard<std::mutex> lk(mu);
                stopping = true;
            }
            cv.notify_all();
        }

        std::string shape_text() {
            std::lock_guard<std::mutex> lk(mu);
            return shape;
        }

        static std::string shape_of(const MX::Types::FeatureMap& fm) {
            std::string s;
            for (auto d : fm.shape()) {
                if (!s.empty()) s += "x";
                s += std::to_string(d);
            }
            return s;
        }
    };

    void shutdown() {
        // Release any worker parked in on_input before asking MxAccl to stop,
        // otherwise stop() waits on a thread that is waiting on us.
        for (auto& st : stages_) st->begin_stop();
        for (auto& st : stages_) {
            if (st->accl) st->accl->stop();
        }
        stages_.clear();
    }

    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
    bool shape_done_ = false;
    size_t depth_ = 1;
};

// ---------------------------------------------------------------------------
// Sync — MxAcclMT::run() blocks until the frame completes. Same shape as the
// other cards' blocking path, so no permit counter and no callbacks.
// ---------------------------------------------------------------------------
class MemryXSyncRunner : public BenchRunner {
public:
    ~MemryXSyncRunner() override { stages_.clear(); }

    void configure(const BenchItem& item) override {
        apply_mpu_clock(item.freq_mhz);
    }

    void load(const std::vector<BenchMember>& members) override {
        for (const auto& m : members) {
            auto st = std::make_unique<Stage>();
            st->reps = m.reps > 0 ? m.reps : 1;
            // No start()/stop() on this class — construct, run, destruct.
            st->accl = std::make_unique<MX::Runtime::MxAcclMT>(m.path);

            const MX::Types::MxModelInfo info = st->accl->get_model_info(0);
            st->in_bufs.resize(static_cast<size_t>(info.num_in_featuremaps));
            st->out_bufs.resize(static_cast<size_t>(info.num_out_featuremaps));
            for (size_t i = 0; i < st->in_bufs.size(); ++i) {
                const size_t n = i < info.in_featuremap_sizes.size()
                                     ? info.in_featuremap_sizes[i] : 1;
                st->in_bufs[i].resize(n ? n : 1);
                // Filled once, here — re-filling per frame would time our own
                // memset rather than the accelerator.
                fill_pattern(st->in_bufs[i].data(),
                             st->in_bufs[i].size() * sizeof(float));
                st->in_ptrs.push_back(st->in_bufs[i].data());
            }
            for (size_t i = 0; i < st->out_bufs.size(); ++i) {
                const size_t n = i < info.out_featuremap_sizes.size()
                                     ? info.out_featuremap_sizes[i] : 1;
                st->out_bufs[i].resize(n ? n : 1);
                st->out_ptrs.push_back(st->out_bufs[i].data());
            }

            if (describe_.empty() && !info.in_raw_shapes.empty()) {
                std::string shape;
                for (auto d : info.in_raw_shapes[0]) {
                    if (!shape.empty()) shape += "x";
                    shape += std::to_string(d);
                }
                describe_ = shape + " float32 · depth 1";
            }
            stages_.push_back(std::move(st));
        }
    }

    void run_frame() override {
        for (auto& st : stages_) {
            for (int r = 0; r < st->reps; ++r) {
                // A finite timeout, deliberately: 0 means block forever, and
                // the engine only tests its stop flag *between* frames, so an
                // infinite wait on a wedged MX3 would strand the worker.
                if (!st->accl->run(st->in_ptrs, st->out_ptrs, /*model_id=*/0,
                                   /*stream_id=*/0, kRunTimeoutMs)) {
                    throw std::runtime_error(
                        "MemryX: MxAcclMT::run() timed out after " +
                        std::to_string(kRunTimeoutMs / 1000) + "s");
                }
            }
        }
    }

    std::string describe() const override { return describe_; }

private:
    static constexpr std::int32_t kRunTimeoutMs = 10000;

    struct Stage {
        std::unique_ptr<MX::Runtime::MxAcclMT> accl;
        std::vector<std::vector<float>> in_bufs, out_bufs;
        std::vector<float*> in_ptrs, out_ptrs;
        int reps = 1;
    };

    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
};

}  // namespace

std::unique_ptr<BenchRunner> make_memryx_runner(ApiMode mode) {
    if (mode == ApiMode::Sync) return std::make_unique<MemryXSyncRunner>();
    return std::make_unique<MemryXAsyncRunner>();
}
