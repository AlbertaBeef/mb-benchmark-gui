// Qualcomm Hexagon NSP benchmark runner (QAIRT / QNN "AI Engine Direct").
//
// Unlike the four M.2 cards, this NPU is integrated in the SoC, so there is no
// PCIe device to open and no vendor daemon to talk to — the runtime is reached
// by dlopen'ing a backend library and asking it for an interface struct. All of
// that lives behind `qnnruntime.h`, a flat C ABI shim vendored from the
// `envic_ai_aerex` project (see CLAUDE.md for the sync rule).
//
// MODEL FORMAT: a *context binary* (.bin) from qnn-context-binary-generator,
// not a .dlc. Measured on this part, loading a cached context takes 14-27 ms
// against 0.6-2.6 s to finalize a graph from scratch, so anything that
// finalizes per run would be timing the compiler, not the NPU. A context binary
// is locked to the HTP architecture it was built for (v73 here); a binary from
// another SoC generation fails to deserialize.
//
// CONCURRENCY. QCS9075 exposes **two** NSPs, each with its own 8 MB VTCM, as
// QNN device ids 0 and 1. That is this part's real concurrency primitive and it
// is what the `NSPs` control drives — one engine chain per NSP, one host thread
// each, structurally identical to bench_axelera.cpp's AIPU cores. Selecting the
// second NSP takes a *device id*, not a core id: asking for core_id 1 fails
// with "core type for device id 0 core id 1 not found in platform info" while
// still loading and running, silently, on NSP 0.
//
// Measured here on OSNet x1.0, burst, 5 s runs:
//
//   1 engine  · 1 NSP    1556 fps      1 engine  × 2 NSPs   3571 fps  (2.30x)
//   2 engines · 1 NSP    2047 fps      4 engines × 1 NSP    2317 fps  (1.45x)
//
// So both knobs buy real throughput, and they are independent: NSPs is the
// large one, extra engines per NSP a smaller host-side overlap win. That second
// effect is what Async mode uses — see the ApiMode note below.
//
// API MODES. QNN's graph execute is blocking and the shim exposes no submit /
// poll pair, so like Axelera this backend has no async API. Sync runs one frame
// in flight per NSP; Async keeps `threads` engines in flight per NSP on host
// threads. That is a genuine gain (1.45x at depth 4 above) but it is host-side
// pipelining, not a vendor async API, and `api_mode_note()` says so.
//
// PERFORMANCE MODE is not a default worth accepting. The HTP runs under DCVS,
// and the spread is large — measured on OSNet across two NSPs:
//   burst 3381 · sustained_high_performance 2818 · balanced 2384 · power_saver 1886
// A Qualcomm frame rate without a stated mode is not comparable to anything,
// so describe() always carries it.
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Bench.h"
#include "bench_input.h"
#include "qnnruntime.h"

namespace fs = std::filesystem;

namespace {

// The SoC's compute units, as backend libraries. Only HTP can load the .bin
// artifacts the catalog ships — see qualcomm_backend_note() and ControlPanel.
constexpr const char* kDefaultBackend = "libQnnHtp.so";

const char* dtype_name(int t) {
    switch (t) {
        case QNN_SHIM_DTYPE_INT8: return "int8";
        case QNN_SHIM_DTYPE_UINT8: return "uint8";
        case QNN_SHIM_DTYPE_INT16: return "int16";
        case QNN_SHIM_DTYPE_UINT16: return "uint16";
        case QNN_SHIM_DTYPE_INT32: return "int32";
        case QNN_SHIM_DTYPE_UINT32: return "uint32";
        case QNN_SHIM_DTYPE_FLOAT16: return "float16";
        case QNN_SHIM_DTYPE_FLOAT32: return "float32";
        default: break;
    }
    return "?";
}

class QualcommBenchRunner : public BenchRunner {
public:
    explicit QualcommBenchRunner(ApiMode mode) : mode_(mode) {}
    ~QualcommBenchRunner() override { shutdown(); }

    void configure(const BenchItem& item) override {
        if (item.nsps >= 1) nsps_ = item.nsps;
        if (!item.perf_mode.empty()) perf_mode_ = item.perf_mode;
        if (!item.qnn_backend.empty()) backend_ = item.qnn_backend;
        // Async depth is engines in flight per NSP; Sync is one by definition.
        // Engines in flight per NSP. NOT gated on the API mode: QNN has no async
        // graph-execute, so this card shows no mode radio and depth would
        // otherwise be stuck at 1 — losing the measured ~1.45x it is worth.
        depth_ = std::max(1, item.depth);
    }

    void load(const std::vector<BenchMember>& members) override {
        if (!qnn_backend_available(backend_.c_str())) {
            throw std::runtime_error("Qualcomm: backend " + backend_ +
                                     " unavailable: " + qnn_last_error());
        }
        // Never ask for more NSPs than the part reports. QNN is the authority
        // here, not /dev/fastrpc-*: the kernel exposes a node per FastRPC
        // domain (cdsp, cdsp1, gdsp0, gdsp1 on this SoC) whether or not the
        // backend can drive it, so counting nodes over-reports by 2x.
        const int have = qnn_backend_device_count(backend_.c_str());
        if (have <= 0) {
            throw std::runtime_error("Qualcomm: " + backend_ +
                                     " reports no devices: " + qnn_last_error());
        }
        nsps_ = std::clamp(nsps_, 1, have);
        depth_ = std::clamp(depth_, 1, 8);

        // One worker per (NSP, depth slot); each owns a full chain of engines so
        // a multi-stage pipeline runs end to end on one NSP rather than
        // ping-ponging tensors between them.
        for (int n = 0; n < nsps_; ++n) {
            for (int d = 0; d < depth_; ++d) {
                auto w = std::make_unique<Worker>();
                w->device_id = static_cast<std::uint32_t>(n);
                for (const auto& m : members) {
                    w->stages.push_back(make_stage(m, w->device_id));
                }
                workers_.push_back(std::move(w));
            }
        }
        if (workers_.empty() || workers_.front()->stages.empty()) {
            throw std::runtime_error("Qualcomm: nothing to run");
        }

        describe_ = build_describe();

        // >1 worker: free-running threads retiring frames into a shared
        // counter. A single worker stays inline — no threads, no locks.
        if (workers_.size() > 1) {
            for (size_t i = 0; i < workers_.size(); ++i) {
                threads_.emplace_back([this, i] { worker_loop(i); });
            }
        }
    }

    void run_frame() override {
        if (workers_.size() == 1) {
            run_one(0);
            return;
        }
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::seconds(10),
                          [this] { return completed_ > 0 || !error_.empty(); })) {
            throw std::runtime_error("Qualcomm: inference timed out");
        }
        if (!error_.empty()) throw std::runtime_error(error_);
        --completed_;
    }

    std::string describe() const override { return describe_; }

private:
    // One graph on one NSP, with its I/O buffers bound once at load. The shim's
    // execute is not re-entrant for a single engine, so an engine belongs to
    // exactly one worker thread.
    struct Stage {
        QnnEngine* engine = nullptr;
        std::vector<std::vector<std::uint8_t>> in_bufs, out_bufs;
        std::vector<void*> in_ptrs, out_ptrs;
        std::vector<std::uint64_t> in_sizes, out_sizes;
        int reps = 1;

        ~Stage() {
            if (engine) qnn_engine_free(engine);
        }
    };
    struct Worker {
        std::uint32_t device_id = 0;
        std::vector<std::unique_ptr<Stage>> stages;
    };

    std::unique_ptr<Stage> make_stage(const BenchMember& m, std::uint32_t device_id) {
        auto st = std::make_unique<Stage>();
        st->reps = m.reps > 0 ? m.reps : 1;
        st->engine = qnn_engine_load(backend_.c_str(), m.path.c_str(), device_id,
                                     /*core_id=*/0, perf_mode_.c_str());
        if (!st->engine) {
            throw std::runtime_error("Qualcomm: loading " + m.path + " on NSP " +
                                     std::to_string(device_id) + ": " +
                                     qnn_last_error());
        }
        const int ni = qnn_engine_num_inputs(st->engine);
        const int no = qnn_engine_num_outputs(st->engine);
        QnnTensorDesc d;
        for (int i = 0; i < ni; ++i) {
            if (qnn_engine_input_desc(st->engine, i, &d) != 0) {
                throw std::runtime_error("Qualcomm: input " + std::to_string(i) +
                                         ": " + qnn_last_error());
            }
            st->in_bufs.emplace_back(static_cast<std::size_t>(d.bytes));
            // Synthetic input, quantized uint8 already — filled once at load,
            // never per frame, so the timed loop is the NPU and not this CPU.
            fill_pattern(st->in_bufs.back().data(), st->in_bufs.back().size());
            st->in_sizes.push_back(d.bytes);
        }
        for (int i = 0; i < no; ++i) {
            if (qnn_engine_output_desc(st->engine, i, &d) != 0) {
                throw std::runtime_error("Qualcomm: output " + std::to_string(i) +
                                         ": " + qnn_last_error());
            }
            st->out_bufs.emplace_back(static_cast<std::size_t>(d.bytes));
            st->out_sizes.push_back(d.bytes);
        }
        // Outputs are left in raw quantized device layout — nothing is
        // dequantized here. Measured on this part, converting a detection
        // head's uint8 outputs to float32 costs more than the inference did.
        for (auto& b : st->in_bufs) st->in_ptrs.push_back(b.data());
        for (auto& b : st->out_bufs) st->out_ptrs.push_back(b.data());
        return st;
    }

    std::string build_describe() {
        std::string out;
        const Stage& st = *workers_.front()->stages.front();
        QnnTensorDesc d;
        if (!st.in_bufs.empty() && qnn_engine_input_desc(st.engine, 0, &d) == 0) {
            for (std::uint32_t i = 0; i < d.rank; ++i) {
                // Drop the leading batch dimension to match the other backends,
                // which all report the per-frame geometry.
                if (i == 0 && d.rank == 4 && d.dims[0] == 1) continue;
                if (!out.empty()) out += "x";
                out += std::to_string(d.dims[i]);
            }
            out += " ";
            out += dtype_name(d.data_type);
        }
        out += " · " + std::to_string(nsps_) + (nsps_ == 1 ? " NSP" : " NSPs");
        if (depth_ > 1) {
            out += " · depth " + std::to_string(depth_) + "/NSP";
        }
        out += " · " + perf_mode_;
        return out;
    }

    // Run every stage once for worker `w`. Throws on a runtime error.
    void run_one(size_t w) {
        for (auto& st : workers_[w]->stages) {
            for (int r = 0; r < st->reps; ++r) {
                if (qnn_engine_execute(st->engine, st->in_ptrs.data(),
                                       st->in_sizes.data(),
                                       static_cast<int>(st->in_ptrs.size()),
                                       st->out_ptrs.data(), st->out_sizes.data(),
                                       static_cast<int>(st->out_ptrs.size())) != 0) {
                    throw std::runtime_error(std::string("Qualcomm: execute failed: ") +
                                             qnn_last_error());
                }
            }
        }
    }

    void worker_loop(size_t w) {
        while (!stop_.load(std::memory_order_relaxed)) {
            try {
                run_one(w);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(mu_);
                if (error_.empty()) error_ = e.what();
                cv_.notify_all();
                return;
            }
            std::lock_guard<std::mutex> lk(mu_);
            ++completed_;
            cv_.notify_one();
        }
    }

    void shutdown() {
        stop_.store(true);
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
        // Only now: a thread still inside qnn_engine_execute would otherwise be
        // using an engine we are freeing.
        workers_.clear();
    }

    ApiMode mode_ = ApiMode::Sync;
    std::string backend_ = kDefaultBackend;
    std::string perf_mode_ = "burst";
    int nsps_ = 2;
    int depth_ = 1;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    int completed_ = 0;
    std::string error_;
    std::string describe_;
};

// Does `dir` hold a Hexagon skel library for any HTP architecture?
bool has_htp_skel(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("libQnnHtp", 0) == 0 && n.find("Skel.so") != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// The HTP backend is split between an application-side library and a DSP-side
// *skel* loaded by FastRPC — which does not consult LD_LIBRARY_PATH, only
// ADSP_LIBRARY_PATH. Getting this wrong is the single most common Qualcomm
// bring-up failure, and its worst form is silent: the graph falls back to CPU
// and simply reports an inexplicably low frame rate.
//
// On Ubuntu for Dragonwing the loader already finds /usr/lib/dsp/<domain>
// unaided — verified here by running a benchmark with the variable unset — so
// this is insurance for other images rather than a requirement. It therefore
// only fills the variable in when nothing has set it, and stays silent if it
// finds no skel directory, letting the SDK's own error stand.
//
// Called once from the main thread: it uses setenv(), which must not race the
// workers' getenv().
void qualcomm_prepare_environment() {
    if (const char* cur = ::getenv("ADSP_LIBRARY_PATH")) {
        if (*cur) return;  // the user or the image has an opinion; keep it
    }
    // Ubuntu-for-Dragonwing package layout first, then the SDK's own tree, then
    // the paths the vendor documentation uses on other images. Each NSP gets
    // its own skel directory (cdsp, cdsp1), and both belong on the path.
    std::vector<std::string> dirs;
    for (const char* base : {"/usr/lib/dsp", "/usr/lib/rfsa/adsp"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(base, ec)) {
            if (e.is_directory(ec) && has_htp_skel(e.path())) {
                dirs.push_back(e.path().string());
            }
        }
        std::error_code ec2;
        if (fs::is_directory(base, ec2) && has_htp_skel(base)) dirs.push_back(base);
    }
    // /usr/share/qcom/<soc>/<vendor>/<board>/dsp/<domain>/ — the board name is
    // the SDK's, not ours (an IQ-9075 EVK uses the SA8775P-RIDE tree), so this
    // is globbed rather than spelled out.
    {
        std::error_code ec;
        fs::recursive_directory_iterator it(
            "/usr/share/qcom", fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            // The tree is <soc>/<vendor>/<board>/dsp/<domain>; stop descending
            // past the domain directories rather than walking the whole share.
            if (it.depth() >= 4) it.disable_recursion_pending();
            if (it->path().filename().string().rfind("cdsp", 0) != 0) continue;
            if (has_htp_skel(it->path())) dirs.push_back(it->path().string());
        }
    }
    if (dirs.empty()) return;

    std::string joined;
    for (const auto& d : dirs) {
        if (!joined.empty()) joined += ";";  // ADSP_LIBRARY_PATH is ;-separated
        joined += d;
    }
    joined += ";/dsp";
    ::setenv("ADSP_LIBRARY_PATH", joined.c_str(), 1);
}

std::unique_ptr<BenchRunner> make_qualcomm_runner(ApiMode mode) {
    return std::make_unique<QualcommBenchRunner>(mode);
}
