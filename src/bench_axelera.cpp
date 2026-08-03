// Axelera Metis benchmark runner (libaxruntime C API).
//
// The runtime takes already-quantized, already-padded int8/uint8 buffers and
// returns the same, so both the input quantization and the output
// unpad+dequantize are host work that the caller controls. Neither happens in
// the loop here: the input buffer is prepared once at load, and the outputs are
// left in their raw device layout. What is timed is axr_run_model_instance.
//
// Core allocation: the card's sub-devices are split evenly across the stages of
// a pipeline (a single model gets them all), so the whole card is exercised
// either way. Some models are compiled with a deploy-time core cap and refuse a
// wider connection, so a failure falls back to one core per stage.
#include "axruntime/axruntime.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
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

    ~AxeleraBenchRunner() override {
        for (auto& st : stages_) {
            if (st->instance) axr_destroy(AXR_OBJECT(st->instance));
            if (st->conn) axr_destroy(AXR_OBJECT(st->conn));
            if (st->model) axr_destroy(AXR_OBJECT(st->model));
        }
        stages_.clear();
        if (ctx_) axr_destroy(AXR_OBJECT(ctx_));
    }

    void load(const std::vector<BenchMember>& members) override {
        ctx_ = axr_create_context();
        if (!ctx_) throw std::runtime_error("Axelera: axr_create_context failed");

        axrDeviceInfo* devices = nullptr;
        const size_t n = axr_list_devices(ctx_, &devices);
        if (n == 0 || !devices) {
            throw std::runtime_error("Axelera: no devices found");
        }
        device_ = devices[0];  // the array is owned by the context

        const size_t nstages = std::max<size_t>(1, members.size());
        const size_t avail = std::max<size_t>(1, device_.subdevice_count);
        const size_t want = std::max<size_t>(1, avail / nstages);

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

            if (!connect_stage(*st, want) && want > 1) {
                connect_stage(*st, 1);  // deploy-time core cap — retry narrow
            }
            if (!st->instance) {
                std::string why = axr_last_error_string(AXR_OBJECT(ctx_));
                if (why.find("kernel_function") != std::string::npos &&
                    !on_path("riscv64-unknown-elf-gcc")) {
                    why += " — riscv64-unknown-elf-gcc is not on PATH, so the "
                           "runtime cannot JIT-compile the model's kernels. "
                           "Install the Voyager toolchain or set "
                           "$MB_AXELERA_TOOLCHAIN to the directory holding it.";
                }
                throw std::runtime_error("Axelera: could not instantiate " + m.path +
                                         ": " + why);
            }

            st->in_bufs.resize(ni);
            st->in_args.resize(ni);
            for (size_t i = 0; i < ni; ++i) {
                st->in_bufs[i].resize(axr_tensor_size(&st->in_infos[i]));
                fill_pattern(st->in_bufs[i].data(), st->in_bufs[i].size());
                st->in_args[i] =
                    axrArgument{st->in_bufs[i].data(), 0, 0, st->in_bufs[i].size()};
            }
            st->out_bufs.resize(no);
            st->out_args.resize(no);
            for (size_t i = 0; i < no; ++i) {
                st->out_bufs[i].resize(axr_tensor_size(&st->out_infos[i]));
                st->out_args[i] =
                    axrArgument{st->out_bufs[i].data(), 0, 0, st->out_bufs[i].size()};
            }

            if (describe_.empty() && ni > 0) {
                const axrTensorInfo& in = st->in_infos[0];
                std::string shape;
                for (size_t d = 0; d < in.ndims; ++d) {
                    const size_t u = in.dims[d] - in.padding[d][0] - in.padding[d][1];
                    if (!shape.empty()) shape += "x";
                    shape += std::to_string(u);
                }
                describe_ = shape + " int8 · " + std::to_string(st->cores) +
                            (st->cores == 1 ? " core" : " cores");
                if (mode_ == ApiMode::Async) describe_ += " · double-buffered";
            }
            stages_.push_back(std::move(st));
        }
    }

    void run_frame() override {
        for (auto& st : stages_) {
            for (int r = 0; r < st->reps; ++r) {
                const axrResult res = axr_run_model_instance(
                    st->instance, st->in_args.data(), st->in_args.size(),
                    st->out_args.data(), st->out_args.size());
                if (res != AXR_SUCCESS) {
                    throw std::runtime_error(std::string("Axelera: run failed: ") +
                                             axr_error_string(res));
                }
            }
        }
    }

    std::string describe() const override { return describe_; }

private:
    struct Stage {
        axrModel* model = nullptr;
        axrConnection* conn = nullptr;
        axrModelInstance* instance = nullptr;
        std::vector<axrTensorInfo> in_infos, out_infos;
        std::vector<std::vector<std::uint8_t>> in_bufs, out_bufs;
        std::vector<axrArgument> in_args, out_args;
        size_t cores = 1;
        int reps = 1;
    };

    ApiMode mode_ = ApiMode::Sync;

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

    bool connect_stage(Stage& st, size_t cores) {
        if (st.conn) { axr_destroy(AXR_OBJECT(st.conn)); st.conn = nullptr; }
        st.conn = axr_device_connect(ctx_, &device_, cores, nullptr);
        if (!st.conn) return false;
        // libaxruntime has no async inference API at all — `axr_run_model_instance`
        // is the runtime's only inference entry point and it blocks. The nearest
        // equivalent it offers is its own `double_buffer` property, which lets
        // the instance overlap the next frame's transfer with the current run.
        // That is what "Async" means for this card, and the legend says so
        // rather than implying parity with Hailo's and DeepX's real job queues.
        axrProperties* props = nullptr;
        if (mode_ == ApiMode::Async) {
            props = axr_create_properties(ctx_, "double_buffer=1");
        }
        st.instance = axr_load_model_instance(st.conn, st.model, props);
        if (props) axr_destroy(AXR_OBJECT(props));
        if (!st.instance) {
            axr_destroy(AXR_OBJECT(st.conn));
            st.conn = nullptr;
            return false;
        }
        st.cores = cores;
        return true;
    }

    axrContext* ctx_ = nullptr;
    axrDeviceInfo device_{};
    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
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
