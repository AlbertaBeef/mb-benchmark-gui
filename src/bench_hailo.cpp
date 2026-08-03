// Hailo-8 benchmark runner (HailoRT C++ API).
//
// One VDevice per run with the ROUND_ROBIN scheduler, so a multi-stage pipeline
// can keep several network groups configured at once and HailoRT multiplexes
// them internally — no per-frame activate/deactivate, which would otherwise
// dominate the measurement.
//
// Output vstreams are created with HAILO_FORMAT_TYPE_AUTO (the device's native
// format) rather than FLOAT32. FLOAT32 would make HailoRT dequantize every
// output on the host inside infer(), so the number would include this CPU's
// dequantization cost rather than the card's throughput.
#include <hailo/hailort.hpp>
#include <hailo/infer_model.hpp>

#include <unistd.h>

#include <cstdlib>
#include <new>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Bench.h"
#include "bench_input.h"

namespace {

using namespace hailort;

// HailoRT DMAs directly out of the buffers we hand it. Two consequences, both
// stated in infer_model.hpp and neither optional:
//   * an unaligned address forces a bounce copy — the runtime warns
//     "read_async() was provided an unaligned buffer ... performance degradation";
//   * an *output* buffer whose allocation is not a whole number of pages risks
//     "memory corruption ... at the end of the last page".
// So allocate page-aligned and round the allocation up to a page, while still
// showing the SDK a view of the logical frame size.
class PageBuffer {
public:
    PageBuffer() = default;
    explicit PageBuffer(size_t size) : size_(size) {
        const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        const size_t alloc = ((size + page - 1) / page) * page;
        void* p = nullptr;
        if (alloc == 0 || ::posix_memalign(&p, page, alloc) != 0 || !p) {
            throw std::bad_alloc();
        }
        data_ = static_cast<std::uint8_t*>(p);
    }
    ~PageBuffer() { std::free(data_); }

    PageBuffer(const PageBuffer&) = delete;
    PageBuffer& operator=(const PageBuffer&) = delete;
    PageBuffer(PageBuffer&& o) noexcept : data_(o.data_), size_(o.size_) {
        o.data_ = nullptr;
        o.size_ = 0;
    }
    PageBuffer& operator=(PageBuffer&& o) noexcept {
        if (this != &o) {
            std::free(data_);
            data_ = o.data_;
            size_ = o.size_;
            o.data_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    std::uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    std::uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

void check(hailo_status s, const char* what) {
    if (s != HAILO_SUCCESS) {
        throw std::runtime_error(std::string("Hailo ") + what +
                                 " failed (status=" + std::to_string(s) + ")");
    }
}

template <typename T>
T unwrap(Expected<T>&& e, const char* what) {
    if (!e) {
        throw std::runtime_error(std::string("Hailo ") + what +
                                 " failed (status=" + std::to_string(e.status()) + ")");
    }
    return e.release();
}

class HailoSyncRunner : public BenchRunner {
public:
    void load(const std::vector<BenchMember>& members) override {
        hailo_vdevice_params_t params{};
        check(hailo_init_vdevice_params(&params), "init vdevice params");
        params.scheduling_algorithm = HAILO_SCHEDULING_ALGORITHM_ROUND_ROBIN;
        device_ = unwrap(VDevice::create(params), "VDevice::create");

        for (const auto& m : members) {
            auto st = std::make_unique<Stage>();
            st->reps = m.reps > 0 ? m.reps : 1;
            st->hef = std::make_shared<Hef>(unwrap(Hef::create(m.path), "Hef::create"));

            auto groups = unwrap(device_->configure(*st->hef), "VDevice::configure");
            if (groups.empty()) {
                throw std::runtime_error("Hailo: no network groups in " + m.path);
            }
            st->group = groups[0];

            auto in_params = unwrap(
                st->group->make_input_vstream_params(
                    false, HAILO_FORMAT_TYPE_UINT8, HAILO_DEFAULT_VSTREAM_TIMEOUT_MS,
                    HAILO_DEFAULT_VSTREAM_QUEUE_SIZE),
                "make_input_vstream_params");
            auto out_params = unwrap(
                st->group->make_output_vstream_params(
                    false, HAILO_FORMAT_TYPE_AUTO, HAILO_DEFAULT_VSTREAM_TIMEOUT_MS,
                    HAILO_DEFAULT_VSTREAM_QUEUE_SIZE),
                "make_output_vstream_params");

            st->pipeline = std::make_unique<InferVStreams>(
                unwrap(InferVStreams::create(*st->group, in_params, out_params),
                       "InferVStreams::create"));

            for (auto& ref : st->pipeline->get_input_vstreams()) {
                InputVStream& vs = ref.get();
                Buf b;
                b.name = vs.name();
                b.bytes = PageBuffer(vs.get_frame_size());
                fill_pattern(b.bytes.data(), b.bytes.size());
                st->inputs.push_back(std::move(b));
            }
            for (auto& ref : st->pipeline->get_output_vstreams()) {
                OutputVStream& vs = ref.get();
                Buf b;
                b.name = vs.name();
                b.bytes = PageBuffer(vs.get_frame_size());
                st->outputs.push_back(std::move(b));
            }

            if (describe_.empty()) {
                auto infos = st->hef->get_input_vstream_infos();
                if (infos && !infos->empty()) {
                    const auto& s = infos->front().shape;
                    describe_ = std::to_string(s.height) + "x" +
                                std::to_string(s.width) + "x" +
                                std::to_string(s.features) + " uint8";
                }
            }
            stages_.push_back(std::move(st));
        }

        // Built after every stage exists so the views point at final storage.
        for (auto& st : stages_) {
            for (auto& b : st->inputs)
                st->in_views.emplace(b.name, MemoryView(b.bytes.data(), b.bytes.size()));
            for (auto& b : st->outputs)
                st->out_views.emplace(b.name, MemoryView(b.bytes.data(), b.bytes.size()));
        }
    }

    void run_frame() override {
        for (auto& st : stages_) {
            for (int r = 0; r < st->reps; ++r) {
                check(st->pipeline->infer(st->in_views, st->out_views, 1), "infer");
            }
        }
    }

    std::string describe() const override { return describe_; }

private:
    struct Buf {
        std::string name;
        PageBuffer bytes;
    };
    struct Stage {
        std::shared_ptr<Hef> hef;
        std::shared_ptr<ConfiguredNetworkGroup> group;
        std::unique_ptr<InferVStreams> pipeline;
        std::vector<Buf> inputs, outputs;
        std::map<std::string, MemoryView> in_views, out_views;
        int reps = 1;
    };

    std::unique_ptr<VDevice> device_;
    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
};

// Async path: HailoRT's InferModel / ConfiguredInferModel::run_async. Each
// stage keeps a pool of Bindings in flight; run_frame() tops the pipeline back
// up and then blocks until one frame retires, so one call still means one
// completed frame and the engine's counter needs no special case.
class HailoAsyncRunner : public BenchRunner {
public:
    // HailoRT invokes completion callbacks on its own worker threads, and those
    // callbacks touch Stage state. Detached jobs can therefore still be in
    // flight when the run ends, so every stage must be drained *and* have its
    // ConfiguredInferModel torn down before the state the callback writes to
    // goes away. Without this the callback lands on a freed `free_slots`
    // vector — a use-after-free that ASAN catches and that shows up in the
    // wild as "corrupted double-linked list", intermittently and only on
    // models slow enough to still have work outstanding at teardown.
    ~HailoAsyncRunner() override {
        for (auto& st : stages_) st->drain();
        stages_.clear();
    }

    void load(const std::vector<BenchMember>& members) override {
        hailo_vdevice_params_t params{};
        check(hailo_init_vdevice_params(&params), "init vdevice params");
        params.scheduling_algorithm = HAILO_SCHEDULING_ALGORITHM_ROUND_ROBIN;
        device_ = unwrap(VDevice::create(params), "VDevice::create");

        for (const auto& m : members) {
            auto st = std::make_unique<Stage>();
            st->reps = m.reps > 0 ? m.reps : 1;

            // create_infer_model already hands back a shared_ptr; InferModel is
            // abstract, so it must be held by pointer.
            st->model = unwrap(device_->create_infer_model(m.path),
                               "create_infer_model");
            // Device-native output format, same reasoning as the sync path: a
            // FLOAT32 request would dequantize on this CPU inside the call.
            for (const auto& name : st->model->get_output_names()) {
                st->model->output(name)->set_format_type(HAILO_FORMAT_TYPE_AUTO);
            }
            for (const auto& name : st->model->get_input_names()) {
                st->model->input(name)->set_format_type(HAILO_FORMAT_TYPE_UINT8);
            }

            st->configured = std::make_unique<ConfiguredInferModel>(
                unwrap(st->model->configure(), "InferModel::configure"));

            size_t depth = 4;
            if (auto q = st->configured->get_async_queue_size()) {
                depth = std::max<size_t>(1, std::min<size_t>(*q, 8));
            }
            st->depth = depth;

            for (size_t i = 0; i < depth; ++i) {
                auto slot = std::make_unique<Slot>();
                slot->bindings = std::make_unique<ConfiguredInferModel::Bindings>(
                    unwrap(st->configured->create_bindings(), "create_bindings"));
                for (const auto& name : st->model->get_input_names()) {
                    Buf b;
                    b.name = name;
                    b.bytes = PageBuffer(st->model->input(name)->get_frame_size());
                    fill_pattern(b.bytes.data(), b.bytes.size());
                    slot->bufs.push_back(std::move(b));
                    auto& back = slot->bufs.back();
                    check(slot->bindings->input(name)->set_buffer(
                              MemoryView(back.bytes.data(), back.bytes.size())),
                          "bind input");
                }
                for (const auto& name : st->model->get_output_names()) {
                    Buf b;
                    b.name = name;
                    b.bytes = PageBuffer(st->model->output(name)->get_frame_size());
                    slot->bufs.push_back(std::move(b));
                    auto& back = slot->bufs.back();
                    check(slot->bindings->output(name)->set_buffer(
                              MemoryView(back.bytes.data(), back.bytes.size())),
                          "bind output");
                }
                st->free_slots.push_back(slot.get());
                st->slots.push_back(std::move(slot));
            }

            if (describe_.empty()) {
                const auto& names = st->model->get_input_names();
                if (!names.empty()) {
                    const auto shape = st->model->input(names.front())->shape();
                    describe_ = std::to_string(shape.height) + "x" +
                                std::to_string(shape.width) + "x" +
                                std::to_string(shape.features) + " uint8 · " +
                                std::to_string(depth) + " in flight";
                }
            }
            stages_.push_back(std::move(st));
        }
    }

    void run_frame() override {
        for (auto& st : stages_) {
            for (int r = 0; r < st->reps; ++r) {
                fill_pipeline(*st);
                wait_one(*st);
            }
        }
    }

    std::string describe() const override { return describe_; }

private:
    struct Buf {
        std::string name;
        PageBuffer bytes;
    };
    struct Slot {
        std::unique_ptr<ConfiguredInferModel::Bindings> bindings;
        std::deque<Buf> bufs;  // stable addresses: bound once, reused per job
    };
    struct Stage {
        std::shared_ptr<InferModel> model;
        std::unique_ptr<ConfiguredInferModel> configured;
        std::vector<std::unique_ptr<Slot>> slots;
        std::vector<Slot*> free_slots;   // guarded by mu
        size_t depth = 1;
        size_t in_flight = 0;            // guarded by mu
        size_t completed = 0;            // guarded by mu
        std::mutex mu;
        std::condition_variable cv;
        int reps = 1;

        // Wait for every outstanding job to retire, then drop the configured
        // model so no further callback can fire, all before ~Stage lets the
        // slot bookkeeping die. Bounded: a wedged device must not hang exit.
        void drain() {
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::seconds(5),
                            [this] { return in_flight == 0; });
            }
            configured.reset();
        }
    };

    void fill_pipeline(Stage& st) {
        while (true) {
            Slot* slot = nullptr;
            {
                std::unique_lock<std::mutex> lk(st.mu);
                if (st.in_flight >= st.depth || st.free_slots.empty()) break;
                slot = st.free_slots.back();
                st.free_slots.pop_back();
                ++st.in_flight;
            }
            // Backpressure from the SDK itself, outside the lock.
            check(st.configured->wait_for_async_ready(std::chrono::milliseconds(10000)),
                  "wait_for_async_ready");
            auto job = st.configured->run_async(
                *slot->bindings, [&st, slot](const AsyncInferCompletionInfo&) {
                    std::lock_guard<std::mutex> lk(st.mu);
                    st.free_slots.push_back(slot);
                    --st.in_flight;
                    ++st.completed;
                    // notify_all, not notify_one: run_frame() and drain() can
                    // both be waiting on this condvar.
                    st.cv.notify_all();
                });
            if (!job) {
                std::lock_guard<std::mutex> lk(st.mu);
                st.free_slots.push_back(slot);
                --st.in_flight;
                throw std::runtime_error("Hailo run_async failed (status=" +
                                         std::to_string(job.status()) + ")");
            }
            job->detach();
        }
    }

    void wait_one(Stage& st) {
        std::unique_lock<std::mutex> lk(st.mu);
        if (!st.cv.wait_for(lk, std::chrono::seconds(10),
                            [&st] { return st.completed > 0; })) {
            throw std::runtime_error("Hailo async inference timed out");
        }
        --st.completed;
    }

    std::unique_ptr<VDevice> device_;
    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
};

}  // namespace

std::unique_ptr<BenchRunner> make_hailo_runner(ApiMode mode) {
    if (mode == ApiMode::Async) return std::make_unique<HailoAsyncRunner>();
    return std::make_unique<HailoSyncRunner>();
}
