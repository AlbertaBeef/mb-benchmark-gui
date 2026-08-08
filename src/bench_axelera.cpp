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
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <fstream>
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

        // ---- how many sub-devices this model's constants need ----
        //
        // Voyager sizes num_sub_devices from the model's L2 constant footprint
        // at ~1.5 MB per AIPU core. Nothing in libaxruntime reports it, so it
        // comes from model.json.
        //
        // **It is only applied when the model actually fits in the part's L2.**
        // The vendor prebuilts do not: ResNet-50 declares 7.97 MB of L2 and
        // YOLOv8m 8.01 MB against the Metis's 6 MB total, and both load and run
        // on a single sub-device — so those pools are evidently paged, and the
        // formula cannot be read literally for them. Taking it literally would
        // demand 4+ cores each and collapse ResNet-50 from four instances to
        // one, throwing away half its measured throughput. Where the model does
        // fit — our locally compiled ArcFace at 3.55 MB, which matches the
        // Voyager guidance of 3 cores exactly — the rule is honoured.
        size_t l2_per_core = env_bytes("MB_AXELERA_L2_PER_CORE_MB", 0);
        if (l2_per_core == 0) l2_per_core = 1500000;  // ~1.5 MB per AIPU core
        const size_t l2_total = l2_per_core * avail;
        const size_t ddr_budget = env_bytes("MB_AXELERA_DDR_BUDGET_MB", 0);

        size_t want = 1;          // sub-devices per connection
        size_t ddr_per_inst = 0;
        for (const auto& m : members) {
            const Footprint fp = read_footprint(resolve_model_json(m.path));
            if (!fp.ok) continue;
            ddr_per_inst += fp.ddr;
            // Declared L2 larger than the whole part means the pools are
            // paged, so the per-core arithmetic says nothing useful — leave
            // this stage at one sub-device. Not surfaced in describe(): it is
            // true of every vendor prebuilt we ship, so it would be noise on
            // every row of every CSV rather than information.
            if (fp.l2 == 0 || fp.l2 > l2_total) continue;
            const size_t need = (fp.l2 + l2_per_core - 1) / l2_per_core;
            want = std::max(want, std::clamp<size_t>(need, 1, avail));
        }

        // Total sub-devices in flight cannot exceed what the card has.
        size_t max_inst = std::max<size_t>(1, avail / (want * nstages));

        // ---- cap instances by device memory ----
        //
        // Every instance loads its own copy of the model's DDR constants, so N
        // instances cost N x that. YOLOv8m carries 53.8 MB against ResNet-50's
        // 20.4 MB, i.e. 215 MB at four instances versus 82 MB — and YOLOv8m at
        // four instances is what died on 2026-08-08, during *load*, at idle
        // power (2.3 W, no rail collapse) with the smallest L2 of any model we
        // ship. Device memory is the suspect that fits those facts.
        //
        // axrDeviceInfo::max_memory is documented as always 0 in this runtime,
        // so the budget cannot be queried and is not guessed at by default:
        // set $MB_AXELERA_DDR_BUDGET_MB to enable the cap. The footprint is
        // reported eitherway so the next failure has a number attached to it.
        if (ddr_per_inst > 0) {
            // Round rather than truncate: ArcFace's 0.83 MB would otherwise
            // report as "0 MB/instance", which reads like a missing number.
            const size_t mb = (ddr_per_inst + 512 * 1024) / (1024 * 1024);
            if (mb > 0) ddr_note_ = std::to_string(mb) + " MB/instance";
            if (ddr_budget > 0) {
                const size_t fits = std::max<size_t>(1, ddr_budget / ddr_per_inst);
                if (fits < max_inst) {
                    sizing_note_ = "capped to " + std::to_string(fits) +
                                   " instance(s) by the " +
                                   std::to_string(ddr_budget / (1024 * 1024)) +
                                   " MB device-memory budget";
                }
                max_inst = std::min(max_inst, fits);
            }
        }

        instances_ = static_cast<int>(
            std::clamp<size_t>(static_cast<size_t>(cores_), 1, max_inst));

        // Say so when the request could not be honoured. Without this the AIPU
        // cores control fails silently: ArcFace needs 3 sub-devices for its L2
        // constants, so only one instance fits and asking for 2, 3 or 4 all
        // produce the same run — the right answer, arriving with no
        // explanation. The DDR cap sets its own note; this covers the rest.
        if (static_cast<size_t>(cores_) > max_inst && sizing_note_.empty()) {
            sizing_note_ = std::to_string(cores_) + " requested, " +
                           std::to_string(instances_) + " fits";
            if (want > 1) {
                sizing_note_ += " (" + std::to_string(want) +
                                " sub-dev/instance for L2)";
            } else if (nstages > 1) {
                sizing_note_ += " (" + std::to_string(nstages) + " stages share the card)";
            }
        }

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
                if (st->cores > 1)
                    describe_ += " x" + std::to_string(st->cores) + " sub-dev";
                if (depth_ > 1) describe_ += " · depth " + std::to_string(depth_);
                if (!ddr_note_.empty()) describe_ += " · " + ddr_note_;
                if (!sizing_note_.empty()) describe_ += " · " + sizing_note_;
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
    // ---- model.json footprint -------------------------------------------
    //
    // The Voyager SDK sizes num_sub_devices from the model's L2 constant
    // footprint (~1.5 MB per AIPU core), not from a number the caller picks.
    // Nothing in libaxruntime exposes it, so it is read from model.json.
    //
    // Deliberately a minimal scanner rather than a JSON library, in the same
    // spirit as Catalog's hand-rolled INI reader: model.json's memory_pools is
    // a flat array of flat objects, and pulling in a dependency to read three
    // fields would not pay for itself.
    struct Footprint {
        bool ok = false;
        size_t l2 = 0;        // every l2 pool, blob-backed or scratch
        size_t l2_const = 0;  // just the blob-backed constants
        size_t ddr = 0;       // every ddr pool
    };

    static Footprint read_footprint(const std::string& model_json) {
        Footprint fp;
        std::ifstream f(model_json);
        if (!f) return fp;
        const std::string j((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        const size_t arr = j.find("\"memory_pools\"");
        if (arr == std::string::npos) return fp;
        size_t i = j.find('[', arr);
        if (i == std::string::npos) return fp;

        int depth = 0;
        size_t obj_start = std::string::npos;
        for (; i < j.size(); ++i) {
            if (j[i] == '{') { if (depth++ == 0) obj_start = i; continue; }
            if (j[i] == ']' && depth == 0) break;
            if (j[i] != '}') continue;
            if (--depth != 0 || obj_start == std::string::npos) continue;

            const std::string obj = j.substr(obj_start, i - obj_start + 1);
            obj_start = std::string::npos;

            auto field = [&](const char* key) -> std::string {
                const size_t k = obj.find(std::string("\"") + key + "\"");
                if (k == std::string::npos) return {};
                size_t c = obj.find(':', k);
                if (c == std::string::npos) return {};
                ++c;
                while (c < obj.size() && std::isspace((unsigned char)obj[c])) ++c;
                if (c < obj.size() && obj[c] == '"') {
                    const size_t e = obj.find('"', c + 1);
                    return e == std::string::npos ? std::string()
                                                  : obj.substr(c + 1, e - c - 1);
                }
                size_t e = c;
                while (e < obj.size() && (std::isdigit((unsigned char)obj[e]) ||
                                          obj[e] == '.' || obj[e] == '-')) ++e;
                return obj.substr(c, e - c);
            };

            const std::string mem = field("memory");
            const std::string sz = field("size");
            if (mem.empty() || sz.empty()) continue;
            const double bytes = std::strtod(sz.c_str(), nullptr);
            if (bytes <= 0) continue;
            const bool has_blob = obj.find("\"blob_file\"") != std::string::npos;
            if (mem == "l2") {
                fp.l2 += static_cast<size_t>(bytes);
                if (has_blob) fp.l2_const += static_cast<size_t>(bytes);
            } else if (mem == "ddr") {
                fp.ddr += static_cast<size_t>(bytes);
            }
            fp.ok = true;
        }
        return fp;
    }

    static size_t env_bytes(const char* name, size_t fallback_mb) {
        const char* e = std::getenv(name);
        if (e && *e) {
            const double mb = std::strtod(e, nullptr);
            if (mb > 0) return static_cast<size_t>(mb * 1024 * 1024);
        }
        return fallback_mb * 1024 * 1024;
    }

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
    std::string sizing_note_;   // why the instance count was clamped, if it was
    std::string ddr_note_;      // device-memory footprint per instance

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
