// DeepX M1 benchmark runner (DXRT C++ API).
//
// DeepX bakes preprocessing and output dequantization into the .dxnn at compile
// time, so a frame is just: hand the engine a uint8 NHWC buffer, let it run.
// Run() returns TensorPtrs that the engine owns until the next call, so nothing
// is copied out here — the benchmark measures the device, not a memcpy.
#include "dxrt/dxrt_api.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Bench.h"
#include "bench_input.h"

namespace {

class DeepXBenchRunner : public BenchRunner {
public:
    explicit DeepXBenchRunner(ApiMode mode) : mode_(mode) {}

    void configure(const BenchItem& item) override {
        if (item.depth >= 1) async_depth_ = static_cast<size_t>(item.depth);
    }

    void load(const std::vector<BenchMember>& members) override {
        for (const auto& m : members) {
            auto st = std::make_unique<Stage>();
            st->reps = m.reps > 0 ? m.reps : 1;
            st->engine = std::make_unique<dxrt::InferenceEngine>(m.path);

            const std::uint64_t bytes = st->engine->GetInputSize();
            if (bytes == 0) {
                throw std::runtime_error("DeepX: zero-sized input for " + m.path);
            }
            st->input.resize(static_cast<std::size_t>(bytes));
            fill_pattern(st->input.data(), st->input.size());

            if (describe_.empty()) {
                // GetInputs() returns Tensors by value and Tensor::type() is
                // non-const, so this binding must not be const.
                auto ins = st->engine->GetInputs();
                if (!ins.empty()) {
                    std::string shape;
                    for (auto d : ins.front().shape()) {
                        if (!shape.empty()) shape += "x";
                        shape += std::to_string(d);
                    }
                    describe_ = shape + " uint8";
                    if (mode_ == ApiMode::Async)
                        describe_ += " · depth " + std::to_string(async_depth_);
                }
            }
            stages_.push_back(std::move(st));
        }
    }

    void run_frame() override {
        for (auto& st : stages_) {
            for (int r = 0; r < st->reps; ++r) {
                if (mode_ == ApiMode::Sync) {
                    (void)st->engine->Run(st->input.data());
                } else {
                    // DXRT's native job queue. Top the pipeline up, then retire
                    // the oldest job — one call still means one finished frame.
                    while (st->jobs.size() < async_depth_) {
                        const int id = st->engine->RunAsync(st->input.data());
                        if (id < 0) throw std::runtime_error("DeepX: RunAsync failed");
                        st->jobs.push_back(id);
                    }
                    (void)st->engine->Wait(st->jobs.front());
                    st->jobs.pop_front();
                }
            }
        }
    }

    std::string describe() const override { return describe_; }

private:
    struct Stage {
        std::unique_ptr<dxrt::InferenceEngine> engine;
        std::vector<std::uint8_t> input;
        std::deque<int> jobs;  // async only: job ids awaiting Wait()
        int reps = 1;
    };

    // Enough outstanding jobs to keep the NPU fed without turning the figure
    // into a latency-hiding contest; matches a realistic pipeline depth.
    static constexpr size_t kDefaultAsyncDepth = 4;
    size_t async_depth_ = kDefaultAsyncDepth;  // set from the Depth control
    ApiMode mode_ = ApiMode::Sync;

    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
};

}  // namespace

std::unique_ptr<BenchRunner> make_deepx_runner(ApiMode mode) {
    return std::make_unique<DeepXBenchRunner>(mode);
}
