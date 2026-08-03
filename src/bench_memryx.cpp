// MemryX MX3 benchmark runner (MxAccl C++ runtime).
//
// MemryX is the mirror image of Axelera: its SDK exposes *only* a streaming
// API. `connect_stream(in_cb, out_cb, ...)` hands the runtime two callbacks and
// `start()` drives them from its own worker threads — there is no blocking
// single-frame call anywhere in the public surface. So:
//
//   Async  — native. Up to kAsyncDepth frames outstanding.
//   Sync   — emulated by allowing exactly one frame in flight, which is the
//            closest honest equivalent of the other cards' blocking call.
//
// Backpressure is applied by making the input callback wait for a permit; that
// parks one of MxAccl's worker threads, which is the intended way to throttle a
// stream (returning false instead would tear it down).
//
// MemryX takes float input, unlike the uint8/int8 of the other three — that is
// the SDK's contract, and the conversion is done once at load, not per frame.
#include "memx/accl/MxAccl.h"

#include <atomic>
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

class MemryXBenchRunner : public BenchRunner {
public:
    explicit MemryXBenchRunner(ApiMode mode)
        : depth_(mode == ApiMode::Sync ? 1 : kAsyncDepth), mode_(mode) {}

    ~MemryXBenchRunner() override { shutdown(); }

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

            if (describe_.empty()) describe_ = std::to_string(depth_) + " in flight";
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
                describe_ = s + " float32 · " + std::to_string(depth_) + " in flight";
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
    ApiMode mode_ = ApiMode::Sync;
};

}  // namespace

std::unique_ptr<BenchRunner> make_memryx_runner(ApiMode mode) {
    return std::make_unique<MemryXBenchRunner>(mode);
}
