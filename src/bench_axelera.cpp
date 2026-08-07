// Axelera Metis benchmark runner (libaxruntime C API).
//
// The runtime takes already-quantized, already-padded int8/uint8 buffers and
// returns the same, so both the input quantization and the output
// unpad+dequantize are host work that the caller controls. Neither happens in
// the loop here: the input buffer is prepared once at load, and the outputs are
// left in their raw device layout. What is timed is axr_run_model_instance.
//
// Cores. libaxruntime has no async inference API; the card's concurrency knob is
// how many AIPU cores a model claims, passed as num_sub_devices to
// axr_device_connect(). The Frequency graph shows it directly: on one core
// aicore0 runs at 800 MHz while aicore1-3 sit at 50 MHz. A multi-stage pipeline
// divides subdevice_count between its stages.
//
// ONE axr_device_connect() PER STAGE. Every connect makes the runtime reload the
// card's firmware ("fwtrace: detached for firmware reload" in dmesg). This file
// used to open one connection per host thread, which fired three or four reloads
// within seconds and, on a card still in bootloader, raced the 3.8 MB firmware
// ELF upload badly enough to drop the PCIe link. Host instances are now an
// internal detail (instances_, default 1) and share the stage's connection.
#include "axruntime/axruntime.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Bench.h"
#include "bench_input.h"

namespace {

namespace fs = std::filesystem;

bool on_path(const char* exe) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path), tok;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t colon = p.find(':', start);
        tok = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!tok.empty() && ::access((fs::path(tok) / exe).c_str(), X_OK) == 0) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

class AxeleraBenchRunner : public BenchRunner {
public:
    explicit AxeleraBenchRunner(ApiMode mode) : mode_(mode) {}

    ~AxeleraBenchRunner() override { shutdown(); }

    void configure(const BenchItem& item) override {
        cores_ = std::max(1, std::min(4, item.cores));
        // Depth is this card's only buffering knob: `double_buffer` is a boolean,
        // so 2 is as deep as the Metis goes. It is NOT gated on the API mode —
        // libaxruntime has no async API, so there is no mode radio here and
        // depth would otherwise be unreachable.
        depth_ = std::max(1, std::min(2, item.depth));
        // One model instance occupies exactly ONE AI core, whatever
        // num_sub_devices the connection asked for. Measured 2026-08-04 with the
        // clocks sampled mid-run: cores=4 with a single instance leaves
        // aicore1..3 at 50 MHz and gives 375.6 fps, no better than one core.
        // So "N cores busy" means N instances, and each needs its own
        // connection — four instances shoved onto one connection collide
        // (`Failed to wait for MSI`, 201.7 fps). Hence: instances == cores.
        instances_ = cores_;
    }

    void load(const std::vector<BenchMember>& members) override {
        ctx_ = axr_create_context();
        if (!ctx_) throw std::runtime_error("Axelera: axr_create_context failed");

        axrDeviceInfo* devices = nullptr;
        const size_t n = axr_list_devices(ctx_, &devices);
        if (n == 0 || !devices) throw std::runtime_error("Axelera: no devices found");
        device_ = devices[0];  // the array is owned by the context

        // One core per instance. A multi-stage pipeline shares the card between
        // its stages, so cap the instance count at what each stage can own.
        const size_t nstages = std::max<size_t>(1, members.size());
        const size_t avail = std::max<size_t>(1, device_.subdevice_count);
        instances_ = static_cast<int>(
            std::clamp<size_t>(static_cast<size_t>(cores_), 1,
                               std::max<size_t>(1, avail / nstages)));
        const size_t want = 1;  // sub-devices per connection

        for (const auto& m : members) {
            auto st = std::make_unique<Stage>();
            st->reps = m.reps > 0 ? m.reps : 1;

            st->model = axr_load_model(ctx_, resolve_model_json(m.path).c_str());
            if (!st->model) {
                throw std::runtime_error("Axelera: load_model('" + m.path + "') failed: " +
                                         axr_last_error_string(AXR_OBJECT(ctx_)));
            }
            const size_t ni = axr_num_model_inputs(st->model);
            const size_t no = axr_num_model_outputs(st->model);
            for (size_t i = 0; i < ni; ++i)
                st->in_infos.push_back(axr_get_model_input(st->model, i));
            for (size_t i = 0; i < no; ++i)
                st->out_infos.push_back(axr_get_model_output(st->model, i));

            // One connection for the whole stage, opened before any instance
            // exists. Ask for this stage's full share of the card so the
            // instances on it can spread across cores.
            // The FIRST connect is the one that uploads the firmware ELF to a
            // cold card. Do it alone and let it complete before opening any
            // further connection, so nothing resets the device mid-upload.
            connect_stage(*st, want);
            if (!st->conn) {
                throw std::runtime_error("Axelera: could not connect to the device: " +
                                         std::string(axr_last_error_string(AXR_OBJECT(ctx_))));
            }

            for (int s = 0; s < instances_; ++s) {
                auto inst = std::make_unique<Inst>();
                // Instance 0 rides the stage connection; each later one gets
                // its own, which is what actually lights up another AI core.
                if (s == 0) {
                    inst->instance = st->conn ? make_instance(st->conn, *st) : nullptr;
                } else {
                    inst->conn = axr_device_connect(ctx_, &device_, want, nullptr);
                    if (inst->conn) inst->instance = make_instance(inst->conn, *st);
                    if (!inst->instance && inst->conn) {
                        axr_destroy(AXR_OBJECT(inst->conn));
                        inst->conn = nullptr;
                    }
                }
                if (!inst->instance) {
                    std::string why = axr_last_error_string(AXR_OBJECT(ctx_));
                    if (why.find("kernel_function") != std::string::npos &&
                        !on_path("riscv64-unknown-elf-gcc")) {
                        why += " — riscv64-unknown-elf-gcc is not on PATH, so the "
                               "runtime cannot JIT-compile the model's kernels. "
                               "Install the Voyager toolchain or set "
                               "$MB_AXELERA_TOOLCHAIN to the directory holding it.";
                    }
                    throw std::runtime_error("Axelera: could not instantiate " +
                                             m.path + ": " + why);
                }

                // Per-instance buffers: concurrent runs must not share them.
                inst->in_bufs.resize(ni);
                inst->in_args.resize(ni);
                for (size_t i = 0; i < ni; ++i) {
                    inst->in_bufs[i].resize(axr_tensor_size(&st->in_infos[i]));
                    fill_pattern(inst->in_bufs[i].data(), inst->in_bufs[i].size());
                    inst->in_args[i] = axrArgument{inst->in_bufs[i].data(), 0, 0,
                                                   inst->in_bufs[i].size()};
                }
                inst->out_bufs.resize(no);
                inst->out_args.resize(no);
                for (size_t i = 0; i < no; ++i) {
                    inst->out_bufs[i].resize(axr_tensor_size(&st->out_infos[i]));
                    inst->out_args[i] = axrArgument{inst->out_bufs[i].data(), 0, 0,
                                                    inst->out_bufs[i].size()};
                }
                st->insts.push_back(std::move(inst));
            }

            if (describe_.empty() && ni > 0) {
                const axrTensorInfo& in = st->in_infos[0];
                std::string shape;
                for (size_t d = 0; d < in.ndims; ++d) {
                    const size_t u = in.dims[d] - in.padding[d][0] - in.padding[d][1];
                    if (!shape.empty()) shape += "x";
                    shape += std::to_string(u);
                }
                describe_ = shape + " int8 · " + std::to_string(instances_) +
                            (instances_ == 1 ? " core" : " cores");
                if (depth_ > 1) describe_ += " · depth " + std::to_string(depth_);
            }
            stages_.push_back(std::move(st));
        }

        // >1 stream: free-running threads, one per stream, each retiring frames
        // into a shared counter. One stream stays inline — no threads, no
        // synchronisation, identical to the original single-instance path.
        if (instances_ > 1) {
            for (int s = 0; s < instances_; ++s) {
                threads_.emplace_back([this, s] { stream_loop(s); });
            }
        }
    }

    void run_frame() override {
        if (instances_ == 1) {
            run_one(0);
            return;
        }
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::seconds(10),
                          [this] { return completed_ > 0 || !error_.empty(); })) {
            throw std::runtime_error("Axelera: inference timed out");
        }
        if (!error_.empty()) throw std::runtime_error(error_);
        --completed_;
    }

    std::string describe() const override { return describe_; }

private:
    struct Inst {
        // Normally null: the instance lives on Stage::conn. Only set when the
        // shared connection refused an extra instance and this stream had to
        // fall back to its own — safe by then, since the firmware is loaded.
        axrConnection* conn = nullptr;
        axrModelInstance* instance = nullptr;
        std::vector<std::vector<std::uint8_t>> in_bufs, out_bufs;
        std::vector<axrArgument> in_args, out_args;
    };
    struct Stage {
        axrModel* model = nullptr;
        // One device connection shared by this stage's stream instances. Every
        // axr_device_connect() makes the runtime reload the card's firmware
        // ("fwtrace: detached for firmware reload" in dmesg); opening one per
        // stream meant a second connect could reset the device while the first
        // was still DMA-ing the 3.8 MB firmware ELF into it. That took the PCIe
        // link down and left the card stuck in bootloader. Connect once,
        // instantiate many.
        axrConnection* conn = nullptr;
        std::vector<axrTensorInfo> in_infos, out_infos;
        std::vector<std::unique_ptr<Inst>> insts;  // one per stream
        size_t cores = 1;
        int reps = 1;
    };

    // Run every stage once for stream `s`. Throws on a runtime error.
    void run_one(int s) {
        for (auto& st : stages_) {
            Inst& inst = *st->insts[static_cast<size_t>(s)];
            for (int r = 0; r < st->reps; ++r) {
                const axrResult res = axr_run_model_instance(
                    inst.instance, inst.in_args.data(), inst.in_args.size(),
                    inst.out_args.data(), inst.out_args.size());
                if (res != AXR_SUCCESS) {
                    throw std::runtime_error(std::string("Axelera: run failed: ") +
                                             axr_error_string(res));
                }
            }
        }
    }

    void stream_loop(int s) {
        while (!stop_.load(std::memory_order_relaxed)) {
            try {
                run_one(s);
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
        // Only now is it safe to tear the instances down — a stream thread
        // still inside axr_run_model_instance would otherwise use freed state.
        for (auto& st : stages_) {
            for (auto& inst : st->insts) {
                if (inst->instance) axr_destroy(AXR_OBJECT(inst->instance));
                // Only a fallback stream owns a connection; the usual case is
                // null and the shared Stage::conn is released just below.
                if (inst->conn) axr_destroy(AXR_OBJECT(inst->conn));
            }
            st->insts.clear();
            // After every instance on it is gone, never before.
            if (st->conn) { axr_destroy(AXR_OBJECT(st->conn)); st->conn = nullptr; }
            if (st->model) axr_destroy(AXR_OBJECT(st->model));
        }
        stages_.clear();
        if (ctx_) { axr_destroy(AXR_OBJECT(ctx_)); ctx_ = nullptr; }
    }

    // axr_load_model wants the model_*.json (or model.json) file, not the
    // directory the catalog names.
    static std::string resolve_model_json(const std::string& dir) {
        if (!fs::is_directory(dir)) return dir;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string nm = e.path().filename().string();
            if (nm.rfind("model_", 0) == 0 && nm.size() > 5 &&
                nm.substr(nm.size() - 5) == ".json") {
                return e.path().string();
            }
        }
        const fs::path plain = fs::path(dir) / "model.json";
        if (fs::exists(plain)) return plain.string();
        throw std::runtime_error("Axelera: no model_*.json in " + dir);
    }

    // Open the stage's single device connection. This is the call that loads
    // firmware onto a cold card, so it must happen exactly once and finish
    // before anything else touches the device.
    bool connect_stage(Stage& st, size_t cores) {
        if (st.conn) { axr_destroy(AXR_OBJECT(st.conn)); st.conn = nullptr; }
        st.conn = axr_device_connect(ctx_, &device_, cores, nullptr);
        if (!st.conn) return false;
        st.cores = cores;
        return true;
    }

    // libaxruntime has no async inference API at all; the nearest thing it
    // offers is its own double_buffer property, which overlaps the next frame's
    // transfer with the current run. That is what "Async" means for this card,
    // and the status line says so rather than implying parity.
    axrModelInstance* make_instance(axrConnection* conn, Stage& st) {
        axrProperties* props = nullptr;
        if (depth_ > 1) {
            props = axr_create_properties(ctx_, "double_buffer=1");
        }
        axrModelInstance* mi = axr_load_model_instance(conn, st.model, props);
        if (props) axr_destroy(AXR_OBJECT(props));
        return mi;
    }

    axrContext* ctx_ = nullptr;
    axrDeviceInfo device_{};
    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
    ApiMode mode_ = ApiMode::Sync;   // no async API; kept for make_runner's signature
    int depth_ = 2;                  // double_buffer on by default
    int cores_ = 4;
    int instances_ = 1;

    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    size_t completed_ = 0;
    std::string error_;
};

}  // namespace

// The Voyager runtime JIT-compiles each model's kernel_function.c with
// riscv64-unknown-elf-gcc inside axr_load_model_instance(). When that compiler
// is not on PATH the only symptom is
//     Failed to create executor for model: kernel_function
// which says nothing about a toolchain and sends you hunting the model instead.
// The toolchain ships alongside the runtime, so find it and put it on PATH
// rather than making every user export one before launching a GUI.
// Override with $MB_AXELERA_TOOLCHAIN (a directory containing the compiler).
void axelera_prepare_environment() {
    if (on_path("riscv64-unknown-elf-gcc")) return;

    std::string bin;
    if (const char* e = std::getenv("MB_AXELERA_TOOLCHAIN")) {
        if (::access((fs::path(e) / "riscv64-unknown-elf-gcc").c_str(), X_OK) == 0) bin = e;
    }
    if (bin.empty()) {
        std::error_code ec;
        for (const auto& d : fs::directory_iterator("/opt/axelera", ec)) {
            if (ec) break;
            const fs::path candidate = d.path() / "bin";
            if (::access((candidate / "riscv64-unknown-elf-gcc").c_str(), X_OK) == 0) {
                bin = candidate.string();
                break;
            }
        }
    }
    if (bin.empty()) return;  // nothing found — let the SDK's own error stand

    const char* cur = std::getenv("PATH");
    const std::string next = cur && *cur ? bin + ":" + cur : bin;
    ::setenv("PATH", next.c_str(), 1);
}

std::unique_ptr<BenchRunner> make_axelera_runner(ApiMode mode) {
    return std::make_unique<AxeleraBenchRunner>(mode);
}
