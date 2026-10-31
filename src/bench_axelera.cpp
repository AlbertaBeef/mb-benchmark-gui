// Axelera Metis benchmark runner (libaxruntime C API).
//
// The runtime takes already-quantized, already-padded int8/uint8 buffers and
// returns the same, so both the input quantization and the output
// unpad+dequantize are host work that the caller controls. Neither happens in
// the loop here: the input buffer is prepared once at load, and the outputs are
// left in their raw device layout. What is timed is axr_run_model_instance.
//
// Cores. Through Voyager 1.7 libaxruntime had NO async inference API at all, and
// the card's only concurrency knob was how many AIPU cores a model claims,
// passed as num_sub_devices to axr_device_connect(). That is still the main
// knob; 1.8 adds an experimental async path, see MB_AXELERA_HAVE_ASYNC below.
// The Frequency graph shows the cores directly: on one core
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

// Async inference arrived in Voyager 1.8 (axr_run_async_model_instance,
// AXR_API_VERSION 3.2). The C API is otherwise purely ADDITIVE between 1.7 and
// 1.8 — same major version, nothing removed or changed — so this file still
// compiles and runs against 1.7, where the async path simply compiles out.
//
// The vendor marks that entry point "INTERNAL USE ONLY ... experimental and
// subject to change or removal without notice - it is not yet part of the
// stable public ABI", and it appears in no release note, doc or example. Hence
// env-gated and OFF by default; see run_frame_async().
#if defined(AXR_API_VERSION) && AXR_API_VERSION >= ((3u << 16) | 2u)
#define MB_AXELERA_HAVE_ASYNC 1
#else
#define MB_AXELERA_HAVE_ASYNC 0
#endif

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include "bench_pacer.h"

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
        // item.depth carries whichever control the API mode made the depth:
        // the async frames-in-flight spin in Async, the double-buffer boolean
        // (1 or 2) in Sync. See ControlPanel::depth().
        depth_ = std::max(1, std::min(2, item.depth));
        // Only the multi-instance path needs this: the shipped batched path
        // submits inline in run_frame(), so BenchEngine's own sleep already
        // gates it. Setting it unconditionally is harmless and keeps the two
        // paths from drifting.
        pacer_.set_target(item.target_fps);
        // Experimental: the old behaviour, N independent batch-1 connections.
        // See the note on multi_instance_ for why it is no longer the default.
        multi_instance_ = env_flag("MB_AXELERA_MULTI_INSTANCE");
        // Concurrent submission into the single batched instance. CLAMPED TO
        // 1 -- i.e. the environment variable cannot currently turn it on, and
        // raising this ceiling is a deliberate source edit, not a runtime
        // choice. It is worth a real gain and is still not safe; the note at
        // the clone loop has the measurements. Re-enable by raising the 1
        // below, and read that note first.
        submit_threads_ = std::clamp(env_int("MB_AXELERA_SUBMIT_THREADS", 1), 1, 1);
        // Voyager 1.8's async path. OFF by default: the vendor calls the entry
        // point internal and unstable, and it has not been measured on this
        // card yet. $MB_AXELERA_ASYNC=1 turns it on, $MB_AXELERA_ASYNC_DEPTH
        // sets frames in flight (1-8, default 4).
        //
        // NOT wired to item.depth: on this card that control is the
        // double-buffering radio, which maps to 1 or 2 and means something
        // else entirely. Conflating them would let a UI labelled "double
        // buffer: on" silently mean "2 frames in flight through an
        // experimental API".
        // The UI's API radio is the source of truth now that Axelera reports
        // both modes; $MB_AXELERA_ASYNC still forces it on for headless
        // drivers, which pass no mode.
        async_ = MB_AXELERA_HAVE_ASYNC &&
                 (mode_ == ApiMode::Async || env_flag("MB_AXELERA_ASYNC"));
        // In Async the depth IS item.depth (1-8 from the spin); the env var
        // remains for a headless driver that has no widget to read.
        async_depth_ = std::clamp(
            mode_ == ApiMode::Async ? item.depth
                                    : env_int("MB_AXELERA_ASYNC_DEPTH", 4),
            1, 8);
        if (async_) depth_ = 1;   // double buffering is the OTHER mode's knob
#if !MB_AXELERA_HAVE_ASYNC
        // Built against 1.7, where the entry point does not exist -- but the
        // UI offers the radio unconditionally (accel_has_both_api_modes), so
        // an Async run would otherwise be indistinguishable from a Sync one in
        // the CSV, with item.depth quietly reinterpreted as double buffering.
        if (mode_ == ApiMode::Async) async_note_ = "async needs Voyager 1.8; ran sync";
#endif
    }

    void load(const std::vector<BenchMember>& members) override {
        ctx_ = axr_create_context();
        if (!ctx_) throw std::runtime_error("Axelera: axr_create_context failed");

        axrDeviceInfo* devices = nullptr;
        const size_t n = axr_list_devices(ctx_, &devices);
        if (n == 0 || !devices) throw std::runtime_error("Axelera: no devices found");
        device_ = devices[0];  // the array is owned by the context

        const size_t nstages = std::max<size_t>(1, members.size());
        const size_t avail = std::max<size_t>(1, device_.subdevice_count);

        // ---- pick the artifact compiled for the requested core count -------
        //
        // Voyager deploys a model per core count, and the N-core build is a
        // genuine fixed-batch-N artifact: resnet50-imagenet-onnx-4core declares
        // input [4,230,240,4] and output [4,1,1,1024]. One invocation of it
        // retires FOUR frames. The base directory is the 1-core, batch-1 build.
        //
        // Sibling naming is "<dir>-<N>core", used generically — nothing here
        // knows the model is ResNet-50. A missing sibling is reported, never
        // silently substituted: benchmarking batch 1 while the UI says "4 cores"
        // is precisely the mistake that produced the old numbers.
        //
        // ASYNC TAKES THE BATCH-1 BUILD, whatever the cores control says.
        // AsyncExecutor::init() refuses anything else -- "AsyncExecutor
        // requires exactly one kernel at stage 0", and an N-core build has N
        // of them (resnet50-...-4core ships kernel_function_0..3.elf). Asking
        // for both does not degrade, it fails the load outright, so the choice
        // is made here and said out loud in describe().
        std::vector<std::string> paths;
        paths.reserve(members.size());
        if (async_ && cores_ > 1) {
            async_note_ = "async needs the batch-1 build; ignoring " +
                          std::to_string(cores_) + " cores";
        }
        for (const auto& m : members) {
            std::string chosen = m.path;
            if (cores_ > 1 && !async_) {
                const std::string sib = m.path + "-" + std::to_string(cores_) + "core";
                std::error_code ec;
                if (fs::exists(fs::path(sib) / "model.json", ec) ||
                    !first_model_json(sib).empty()) {
                    chosen = sib;
                } else if (artifact_note_.empty()) {
                    artifact_note_ = "no " + std::to_string(cores_) +
                                     "-core build; using batch 1";
                }
            }
            paths.push_back(std::move(chosen));
        }

        // ---- load, and take the batch size from the model itself -----------
        for (size_t mi = 0; mi < members.size(); ++mi) {
            const BenchMember& m = members[mi];
            auto st = std::make_unique<Stage>();
            st->reps = m.reps > 0 ? m.reps : 1;
            st->model_path = paths[mi];

            st->model = axr_load_model(ctx_, resolve_model_json(paths[mi]).c_str());
            if (!st->model) {
                throw std::runtime_error("Axelera: load_model('" + paths[mi] +
                                         "') failed: " +
                                         axr_last_error_string(AXR_OBJECT(ctx_)));
            }
            const size_t ni = axr_num_model_inputs(st->model);
            const size_t no = axr_num_model_outputs(st->model);
            if (ni == 0) throw std::runtime_error("Axelera: model has no inputs");
            for (size_t i = 0; i < ni; ++i)
                st->in_infos.push_back(axr_get_model_input(st->model, i));
            for (size_t i = 0; i < no; ++i)
                st->out_infos.push_back(axr_get_model_output(st->model, i));

            // Dimension zero is the batch. Every input must agree, or "one
            // invocation retires N frames" has no single N and the frame count
            // would be a guess.
            const size_t b = st->in_infos[0].ndims ? st->in_infos[0].dims[0] : 1;
            for (size_t i = 1; i < ni; ++i) {
                const size_t bi = st->in_infos[i].ndims ? st->in_infos[i].dims[0] : 1;
                if (bi != b) {
                    throw std::runtime_error(
                        "Axelera: model '" + paths[mi] + "' has inconsistent input "
                        "batch dimensions (" + std::to_string(b) + " vs " +
                        std::to_string(bi) + "); cannot count frames");
                }
            }
            st->batch = std::max<size_t>(1, b);
            stages_.push_back(std::move(st));
        }

        // Every stage of a pipeline processes the same frames, so their batches
        // must agree for the retired-frame count to mean anything.
        batch_ = static_cast<int>(stages_.front()->batch);
        for (const auto& st : stages_) {
            if (static_cast<int>(st->batch) != batch_) {
                throw std::runtime_error(
                    "Axelera: pipeline stages have different batch sizes (" +
                    std::to_string(batch_) + " vs " + std::to_string(st->batch) +
                    "); refusing to guess the frame count");
            }
        }

#if MB_AXELERA_HAVE_ASYNC
        // ---- is async usable for THIS model? -------------------------------
        //
        // Decided here, before a single instance exists, because async_mode is
        // an INSTANCE property: an instance built for the async executor and
        // then driven with the blocking call would be a configuration nobody
        // asked for, and one the CSV could not describe.
        //
        // A pipeline would need its stages chained through the completion
        // callback; a stage with reps > 1 retires a different number of frames
        // per submission than batch_ says; and the multi-instance path runs its
        // own producer threads on the same borrowed instance, which is the
        // concurrent-submission hazard this file documents at length.
        if (async_) {
            const char* why = nullptr;
            if (stages_.size() != 1)        why = "async needs a single-stage model; ran sync";
            else if (stages_.front()->reps != 1) why = "async needs reps=1; ran sync";
            else if (multi_instance_)       why = "async is single-instance only; ran sync";
            // Back to the sync path — with double buffering OFF, which is
            // this card's default. Falling back INTO the faster-but-unsoaked
            // configuration would be the opposite of what a fallback is for.
            if (why) { async_ = false; depth_ = 1; async_note_ = why; }
        }
#endif

        // ---- how many sub-devices, and how many instances ------------------
        //
        // For a fixed-batch artifact the compiler already decided: reserve
        // `batch` sub-devices and give the instance `aipu_cores=batch`. One
        // connection, one instance, one execution thread — the arrangement the
        // SDK's own axruntime_example.cpp uses.
        //
        // For a batch-1 artifact there is a choice, and the L2 rule below is
        // what makes it (ArcFace's 3.55 MB of constants needs 3 cores).
        size_t want = 1;
        if (batch_ > 1) {
            want = static_cast<size_t>(batch_);
            if (want * nstages > avail) {
                throw std::runtime_error(
                    "Axelera: batch " + std::to_string(batch_) + " x " +
                    std::to_string(nstages) + " stage(s) needs " +
                    std::to_string(want * nstages) + " sub-devices, card has " +
                    std::to_string(avail));
            }
            instances_ = 1;
        }

        size_t l2_per_core = env_bytes("MB_AXELERA_L2_PER_CORE_MB", 0);
        if (l2_per_core == 0) l2_per_core = 1500000;  // ~1.5 MB per AIPU core
        const size_t l2_total = l2_per_core * avail;
        const size_t ddr_budget = env_bytes("MB_AXELERA_DDR_BUDGET_MB", 0);

        size_t ddr_per_inst = 0;
        for (const auto& path : paths) {
            const Footprint fp = read_footprint(resolve_model_json(path));
            if (!fp.ok) continue;
            ddr_per_inst += fp.ddr;
            if (batch_ > 1) continue;  // the artifact's batch already decided
            // Declared L2 larger than the whole part means the pools are
            // paged, so the per-core arithmetic says nothing useful — leave
            // this stage at one sub-device.
            if (fp.l2 == 0 || fp.l2 > l2_total) continue;
            const size_t need = (fp.l2 + l2_per_core - 1) / l2_per_core;
            want = std::max(want, std::clamp<size_t>(need, 1, avail));
        }

        if (batch_ == 1) {
            // Batch-1 default is ONE instance on `want` sub-devices. Running N
            // independent batch-1 connections is the old behaviour and is now
            // opt-in: it conflates "cores" with "instances", and on a card whose
            // model has a real N-core build the batched artifact is the honest
            // way to use N cores.
            size_t max_inst = std::max<size_t>(1, avail / (want * nstages));
            if (ddr_per_inst > 0 && ddr_budget > 0) {
                const size_t fits = std::max<size_t>(1, ddr_budget / ddr_per_inst);
                if (fits < max_inst) {
                    sizing_note_ = "capped to " + std::to_string(fits) +
                                   " instance(s) by the " +
                                   std::to_string(ddr_budget / (1024 * 1024)) +
                                   " MB device-memory budget";
                }
                max_inst = std::min(max_inst, fits);
            }
            instances_ = multi_instance_
                             ? static_cast<int>(std::clamp<size_t>(
                                   static_cast<size_t>(cores_), 1, max_inst))
                             : 1;
        }

        if (ddr_per_inst > 0) {
            const size_t mb = (ddr_per_inst + 512 * 1024) / (1024 * 1024);
            if (mb > 0) ddr_note_ = std::to_string(mb) + " MB/instance";
        }

        // ---- connect once, instantiate ------------------------------------
        for (auto& st : stages_) {
            // The FIRST connect uploads the 3.8 MB firmware ELF to a cold card.
            // Let it complete alone before anything else touches the device.
            connect_stage(*st, want);
            if (!st->conn) {
                throw std::runtime_error("Axelera: could not connect to the device: " +
                                         std::string(axr_last_error_string(AXR_OBJECT(ctx_))));
            }
            for (int s = 0; s < instances_; ++s) {
                auto inst = std::make_unique<Inst>();
                if (s == 0) {
                    inst->instance = make_instance(st->conn, *st, want);
                } else {
                    inst->conn = axr_device_connect(ctx_, &device_, want, nullptr);
                    if (inst->conn) inst->instance = make_instance(inst->conn, *st, want);
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
                                             st->model_path + ": " + why);
                }

                // Per-instance buffers, sized from the tensor info — which
                // already includes the batch dimension, so a batch-4 model gets
                // a 4x buffer without any arithmetic here.
                const size_t ni = st->in_infos.size(), no = st->out_infos.size();
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

            // ---- concurrent submission into ONE instance -------------------
            // MEASURED 2026-09-14, libaxruntime 1.7.0: this does not work, and
            // the knob is kept only so a future runtime can be re-checked in
            // one environment variable rather than by rebuilding the argument.
            //
            // The reasoning that led here is still right. Little's law says
            // frames_in_flight = throughput x latency; the batch-4 artifact
            // blocks 2.50 ms and retires 4 frames, pinning one submitting
            // thread at exactly 4 frames resident (1598.5 fps) where the vendor
            // pipeline holds 21. A second INSTANCE cannot buy that -- the
            // artifact is 28.34 MB of the part's ~32.47 MB of L2 and a second
            // axr_load_model_instance fails outright -- but extra *submitting
            // threads* need no L2, only their own buffers, which Inst has.
            //
            // It half-works, and that is worse than either extreme. Measured:
            //
            //   1 thread   1598.0 / 1598.7 / 1601.3 / 1621.0 / 1625.6  (tight)
            //   2 threads  BIMODAL: 1565 .. 1643 most of the time,
            //                   or  2037.3 / 2046.3 / 2051.7 / 2058.3
            //   4 threads  run fails:
            //     [ERROR][axeCommandQueueExecuteCommandLists]: All command
            //       lists must be closed before execution. Close command
            //       list 0 first.
            //     AXR_ERROR_RUNTIME_ERROR
            //
            // The fast mode is REAL WORK, not phantom completions -- checked
            // against the INA228 because a shared command list could plausibly
            // return success without running, and the frame counter cannot tell
            // on its own. Idle is 2.34 W; subtracting it, fps per dynamic watt
            // is 696 (1 thread), 720 (2 fast), 683 and 693 (2 slow) -- constant
            // within 5%. Fabricated frames cost no silicon, so the fast run's
            // efficiency would be ~25% high. It is not. 2037 fps is also the
            // vendor's own published 2054, which suggests Voyager submits
            // concurrently too, with synchronisation we do not have.
            //
            // But it is a RACE. axr_run_model_instance() is not safe to call
            // concurrently on one instance: the instance owns a single Level
            // Zero command list, and a second thread entering before the first
            // closes it corrupts the queue. The fast mode appears maybe a third
            // of the time and vanishes entirely under extra system load
            // (running an INA228 sampler alongside was enough to suppress it).
            // WHY two threads sometimes interleave cleanly is UNKNOWN. The
            // obvious theory -- double buffering supplying a second command
            // list, since the runtime's own log calls double_buffer "depth=2"
            // -- was tested and is WRONG: five interleaved on/off pairs, and
            // the one fast run (2051.7 fps) came with double buffering OFF.
            // Don't re-run that experiment; it is settled.
            //
            //   double buffer ON   1586.3 1600.3 1736.0 1567.3 1568.3
            //   double buffer OFF  1566.0 1565.0 1598.3 1598.7 2051.7
            //
            // And the reason this is clamped to 1 rather than 2: EVERY 2-thread
            // run logs `Failed to wait for MSI` at teardown -- 10 of 10, both
            // db settings -- while 1 thread logs none. That message is this
            // card's documented precursor to a PCIe link drop and a power
            // cycle (see Known issues). No run has actually dropped the link
            // yet, but shipping a path that trips the precursor every time to
            // chase a gain we cannot explain or reproduce on demand is a bad
            // trade.
            //
            // So ~1600 fps is NOT the ceiling -- the card demonstrably does
            // ~2050, which is the vendor's own published 2054 -- but
            // single-threaded submission is the only SAFE way we have to
            // drive it today.
            if (submit_threads_ > 1 && instances_ == 1 && !st->insts.empty()) {
                const Inst& proto = *st->insts.front();
                for (int t = 1; t < submit_threads_; ++t) {
                    auto clone = std::make_unique<Inst>();
                    clone->instance = proto.instance;   // borrowed
                    clone->owns_instance = false;
                    clone->in_bufs = proto.in_bufs;     // deep copy: distinct
                    clone->out_bufs = proto.out_bufs;   // memory per thread
                    clone->in_args.resize(clone->in_bufs.size());
                    for (size_t i = 0; i < clone->in_bufs.size(); ++i) {
                        clone->in_args[i] = axrArgument{clone->in_bufs[i].data(), 0, 0,
                                                        clone->in_bufs[i].size()};
                    }
                    clone->out_args.resize(clone->out_bufs.size());
                    for (size_t i = 0; i < clone->out_bufs.size(); ++i) {
                        clone->out_args[i] = axrArgument{clone->out_bufs[i].data(), 0, 0,
                                                         clone->out_bufs[i].size()};
                    }
                    st->insts.push_back(std::move(clone));
                }
            }
        }

#if MB_AXELERA_HAVE_ASYNC
        // ---- async slots ---------------------------------------------------
        // One buffer set per frame in flight, borrowing stage 0's instance.
        // Eligibility was settled above, before the instance was built.
        if (async_ && !stages_.front()->insts.empty()) {
            async_state_ = std::make_shared<AsyncState>();
            {
                const Inst& proto = *stages_.front()->insts.front();
                for (int i = 0; i < async_depth_; ++i) {
                    auto slot = std::make_unique<AsyncSlot>();
                    slot->state = async_state_;
                    slot->inst = std::make_unique<Inst>();
                    Inst& in = *slot->inst;
                    in.instance = proto.instance;   // borrowed, never destroyed here
                    in.owns_instance = false;
                    in.in_bufs = proto.in_bufs;     // deep copy: distinct memory
                    in.out_bufs = proto.out_bufs;   // per frame in flight
                    in.in_args.resize(in.in_bufs.size());
                    for (size_t k = 0; k < in.in_bufs.size(); ++k) {
                        in.in_args[k] = axrArgument{in.in_bufs[k].data(), 0, 0,
                                                    in.in_bufs[k].size()};
                    }
                    in.out_args.resize(in.out_bufs.size());
                    for (size_t k = 0; k < in.out_bufs.size(); ++k) {
                        in.out_args[k] = axrArgument{in.out_bufs[k].data(), 0, 0,
                                                     in.out_bufs[k].size()};
                    }
                    async_slots_.push_back(std::move(slot));
                }
            }
        }
#endif

        // ---- status line ---------------------------------------------------
        {
            const axrTensorInfo& in = stages_.front()->in_infos[0];
            std::string shape;
            for (size_t d = 0; d < in.ndims; ++d) {
                const size_t u = in.dims[d] - in.padding[d][0] - in.padding[d][1];
                if (!shape.empty()) shape += "x";
                shape += std::to_string(u);
            }
            // Cores reserved is per-connection sub-devices x connections: the
            // batched path is (batch x 1), the experimental multi-instance path
            // is (1 x N). Reporting `want` alone would say "1 AIPU core" for a
            // run that is actually occupying four.
            const size_t cores_busy = want * static_cast<size_t>(instances_);
            describe_ = shape + " int8 · batch " + std::to_string(batch_) + " · " +
                        std::to_string(cores_busy) +
                        (cores_busy == 1 ? " AIPU core" : " AIPU cores") + " · " +
                        std::to_string(instances_) +
                        (instances_ == 1 ? " instance" : " instances");
            describe_ += depth_ > 1 ? " · double buffer" : " · no double buffer";
            // Never let a run claim a configuration it did not get: a second
            // submitting thread changes nothing about throughput but it IS a
            // different run, and axelera_cfg is where that has to show.
            if (submit_threads_ > 1) {
                describe_ += " · " + std::to_string(submit_threads_) +
                             " submit threads";
            }
            // The CSV has to be able to tell an async run from a sync one:
            // they are different measurements of the same card.
            if (async_) {
                describe_ += " · async depth " + std::to_string(async_depth_) +
                             " (experimental)";
            }
            if (!async_note_.empty()) describe_ += " · " + async_note_;
            if (!ddr_note_.empty()) describe_ += " · " + ddr_note_;
            if (!artifact_note_.empty()) describe_ += " · " + artifact_note_;
            if (!sizing_note_.empty()) describe_ += " · " + sizing_note_;
        }

        // >1 instance: free-running threads, one per instance. The batched path
        // is always a single instance and stays inline — no threads, no
        // synchronisation.
        streams_ = instances_ > 1 ? instances_ : submit_threads_;
        if (streams_ > 1) {
            for (int s = 0; s < streams_; ++s)
                threads_.emplace_back([this, s] { stream_loop(s); });
        }
    }

    // Returns the number of frames retired: `batch_` per successful invocation.
    unsigned run_frame() override {
#if MB_AXELERA_HAVE_ASYNC
        if (async_) return run_frame_async();
#endif
        if (streams_ <= 1) {
            run_one(0);
            return static_cast<unsigned>(batch_);
        }
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::seconds(10),
                          [this] { return completed_ > 0 || !error_.empty(); })) {
            throw std::runtime_error("Axelera: inference timed out");
        }
        if (!error_.empty()) throw std::runtime_error(error_);
        --completed_;
        return static_cast<unsigned>(batch_);
    }

    std::string describe() const override { return describe_; }

private:
    struct Inst {
        // Normally null: the instance lives on Stage::conn. Only set when the
        // shared connection refused an extra instance and this stream had to
        // fall back to its own — safe by then, since the firmware is loaded.
        axrConnection* conn = nullptr;
        axrModelInstance* instance = nullptr;
        // False on a submit-thread clone, which BORROWS insts[0]'s instance.
        // Teardown destroys the instance once, from its owner only.
        bool owns_instance = true;
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
        size_t batch = 1;         // input dims[0] — frames per invocation
        std::string model_path;   // the artifact actually chosen, for messages
        int reps = 1;
    };

    // Run every stage once for stream `s`. Throws on a runtime error.
#if MB_AXELERA_HAVE_ASYNC
    // Completion callback. Runs on a libaxruntime thread, so it touches only
    // mutex-guarded state and never the SDK.
    //
    // It reaches that state through the slot's shared_ptr, NOT through the
    // runner: drain_async() gives up after 10 s on a card that may never call
    // back, and the runner is destroyed straight afterwards. Leaking the slots
    // keeps their buffers alive, but a callback that dereferenced the runner
    // to find the mutex would still be a use-after-free -- the same class of
    // bug as the Hailo Stage::drain() one, a level further out.
    static void on_async_complete(void* arg, axrResult result) {
        auto* slot = static_cast<AsyncSlot*>(arg);
        const std::shared_ptr<AsyncState> st = slot->state;
        if (!st) return;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            slot->busy = false;
            if (st->inflight > 0) --st->inflight;
            if (result != AXR_SUCCESS && st->error.empty()) {
                st->error = std::string("Axelera: async run failed: ") +
                            axr_error_string(result);
            } else if (result == AXR_SUCCESS) {
                ++st->completed;
            }
        }
        // notify_ALL, not one: at teardown drain_async() waits on this same
        // condition while a worker may still be parked in run_frame_async().
        // Waking only one of them can leave the drain asleep until its timeout
        // and turn a clean stop into a ten-second stall.
        st->cv.notify_all();
    }

    // Submit one frame into a free slot. Called with the state mutex HELD; the
    // SDK call itself is made with the lock dropped, because the completion
    // callback takes the same mutex and a fast-completing frame would
    // otherwise self-deadlock.
    bool submit_async_locked(std::unique_lock<std::mutex>& lk) {
        AsyncState& st = *async_state_;
        AsyncSlot* slot = nullptr;
        for (auto& sp : async_slots_) {
            if (!sp->busy) { slot = sp.get(); break; }
        }
        if (!slot) return false;       // pipeline already full
        slot->busy = true;
        ++st.inflight;
        Inst& in = *slot->inst;
        lk.unlock();
        const axrResult res = axr_run_async_model_instance(
            in.instance, in.in_args.data(), in.in_args.size(),
            in.out_args.data(), in.out_args.size(), &on_async_complete, slot);
        lk.lock();
        if (res != AXR_SUCCESS) {
            // Never queued, so no callback will ever arrive for this slot.
            slot->busy = false;
            if (st.inflight > 0) --st.inflight;
            if (st.error.empty()) {
                st.error = std::string("Axelera: async submit failed: ") +
                           axr_error_string(res);
            }
            return false;
        }
        return true;
    }

    // Top the pipeline up to async_depth_, then block for exactly one
    // completion — the contract every async runner in this project keeps, so
    // BenchEngine's frame counter needs no special case.
    unsigned run_frame_async() {
        AsyncState& st = *async_state_;
        std::unique_lock<std::mutex> lk(st.mu);
        while (st.error.empty() && st.inflight < static_cast<size_t>(async_depth_)) {
            if (!submit_async_locked(lk)) break;
        }
        if (!st.error.empty()) throw std::runtime_error(st.error);
        if (!st.cv.wait_for(lk, std::chrono::seconds(10),
                            [&st] { return st.completed > 0 || !st.error.empty(); })) {
            throw std::runtime_error("Axelera: async inference timed out");
        }
        if (!st.error.empty()) throw std::runtime_error(st.error);
        --st.completed;
        return static_cast<unsigned>(batch_);
    }

    // Wait for every outstanding callback before anything is destroyed. This
    // is the Hailo lesson, not a precaution: a completion firing into freed
    // slot memory is exactly how bench_hailo.cpp produced intermittent
    // "corrupted double-linked list" aborts before Stage::drain() existed.
    // Bounded, because hanging teardown is worse than leaking — a wedged card
    // may never call back at all.
    void drain_async() {
        if (!async_state_) return;
        AsyncState& st = *async_state_;
        std::unique_lock<std::mutex> lk(st.mu);
        if (!st.cv.wait_for(lk, std::chrono::seconds(10),
                            [&st] { return st.inflight == 0; })) {
            // Leak the slots deliberately: their buffers stay alive for a
            // callback that may still arrive, and each one holds a shared_ptr
            // to this state, so the mutex it locks outlives the runner too.
            // One leaked allocation beats a use-after-free or a frozen Stop
            // button.
            for (auto& sp : async_slots_) (void)sp.release();
        }
        async_slots_.clear();
    }
#endif  // MB_AXELERA_HAVE_ASYNC

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
            // Free-running producer: pace at submission. Advance by batch_,
            // so a target stays a frame rate on a fixed-batch artifact.
            if (!pacer_.await(static_cast<unsigned>(batch_))) return;
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
        pacer_.stop();   // wake a producer parked waiting for its next slot
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
#if MB_AXELERA_HAVE_ASYNC
        // Before ANY instance is destroyed: an outstanding completion callback
        // would otherwise fire into freed slot memory.
        drain_async();
#endif
        // Only now is it safe to tear the instances down — a stream thread
        // still inside axr_run_model_instance would otherwise use freed state.
        for (auto& st : stages_) {
            for (auto& inst : st->insts) {
                if (inst->instance && inst->owns_instance)
                    axr_destroy(AXR_OBJECT(inst->instance));
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

    // Plain count from the environment -- NOT env_bytes(), which scales by MB.
    static int env_int(const char* name, int fallback) {
        const char* e = std::getenv(name);
        if (!e || !*e) return fallback;
        char* end = nullptr;
        const long v = std::strtol(e, &end, 10);
        if (end == e || v <= 0 || v > 1024) return fallback;
        return static_cast<int>(v);
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
    static bool env_flag(const char* name) {
        const char* e = std::getenv(name);
        return e && *e && std::strcmp(e, "0") != 0;
    }

    // First model_*.json in `dir`, or empty. Used to test whether an -Ncore
    // sibling exists without assuming it is called plain model.json.
    static std::string first_model_json(const std::string& dir) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return {};
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string nm = e.path().filename().string();
            if (nm.rfind("model", 0) == 0 && nm.size() > 5 &&
                nm.substr(nm.size() - 5) == ".json") {
                return e.path().string();
            }
        }
        return {};
    }

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

    // ---- instance properties -------------------------------------------
    //
    // ONE PROPERTY PER LINE. axr_create_properties() splits the string on
    // '\n' and on nothing else -- axr::split(s, 0xa), verified in the
    // disassembly of BOTH libaxruntime 1.7.0 and 1.8.0. A ';'-separated
    // string is NOT rejected: the first key swallows the whole remainder as
    // its value, strtoul stops at the ';', and every later property is
    // silently dropped. This file used ';' until 2026-09-20, so aipu_cores,
    // double_buffer and async_mode had never once reached the runtime.
    //
    // Measured the day it was found, ResNet-50 batch-4, four cores, 8 s each:
    //
    //   ';'  (what we shipped)            1586.0 fps
    //   '\n' with every property set      1903.2 / 1938.5 / 1961.6 fps
    //   '\n' but no double_buffer         1618.6 fps
    //
    // +20%, and it is ALL double_buffer -- aipu_cores adds nothing on top of
    // the connection's own num_sub_devices for a fixed-batch artifact. It also
    // retires "~1600 fps is where libaxruntime tops out": 1600 was double
    // buffering being thrown away, and ~1940 is 94% of the vendor's 2054 from
    // a single submitting thread, with none of the MSI timeouts that the
    // two-thread experiment tripped.
    //
    // A ';' is therefore a silent misconfiguration, never an error. Keep the
    // one-per-line form, and check a new key by passing it ALONE first: an
    // unrecognised key is reported ("Unknown property key: <k>") only when it
    // is parsed as a key rather than swallowed as somebody's value.
    axrModelInstance* make_instance(axrConnection* conn, Stage& st, size_t sub_devices) {
        std::string p;
        const auto add = [&p](const std::string& kv) {
            if (!p.empty()) p += "\n";
            p += kv;
        };
        add("input_dmabuf=0");
        // num_sub_devices says how many the CONNECTION reserved; aipu_cores
        // says how many this instance may spread across. Both, as the SDK's
        // own axruntime_example.cpp does.
        add("num_sub_devices=" + std::to_string(sub_devices));
        add("aipu_cores=" + std::to_string(sub_devices));
        // double_buffer overlaps the next frame's transfer with the current
        // run. It is a boolean -- any truthy value just turns it on -- so
        // depth tops out at 2 on this card.
        if (depth_ > 1) add("double_buffer=1");
#if MB_AXELERA_HAVE_ASYNC
        // async_mode is what makes axr_load_model_instance() build an
        // axr::AsyncExecutor instead of the default V1/Lockstep one; without
        // it axr_run_async_model_instance() lands on the base Executor's weak
        // stub and returns AXR_ERROR_NOT_IMPLEMENTED. max_inflight is the
        // executor's lane count and max_pending its queue depth -- the same
        // two knobs the vendor's own AxInferenceNet exposes (defaults 4 / 8).
        if (async_) {
            add("async_mode=1");
            add("max_inflight=" + std::to_string(async_depth_));
            add("max_pending=" + std::to_string(async_depth_ * 2));
        }
#endif
        axrProperties* props = axr_create_properties(ctx_, p.c_str());
        // A null here would otherwise load with SDK defaults -- no cores, no
        // double buffering, no async -- while describe() still reported the
        // configuration that was asked for. Never let a run claim a
        // configuration it did not get.
        if (!props) {
            throw std::runtime_error(
                std::string("Axelera: could not build instance properties: ") +
                axr_last_error_string(AXR_OBJECT(ctx_)));
        }
        axrModelInstance* mi = axr_load_model_instance(conn, st.model, props);
        axr_destroy(AXR_OBJECT(props));
        return mi;
    }

    axrContext* ctx_ = nullptr;
    axrDeviceInfo device_{};
    std::vector<std::unique_ptr<Stage>> stages_;
    std::string describe_;
    ApiMode mode_ = ApiMode::Sync;   // Async needs Voyager >= 1.8; see configure()
    // Double buffering OFF unless asked for: see ControlPanel's radio pair.
    // This is the runner's own default, used when configure() is never called
    // (a headless driver); it must agree with the widget or the two paths
    // measure different configurations.
    int depth_ = 1;
    int cores_ = 4;
    int instances_ = 1;
    // Host threads submitting into the ONE instance. 1 = today's behaviour.
    // $MB_AXELERA_SUBMIT_THREADS while this is being measured.
    int submit_threads_ = 1;
    int streams_ = 1;   // threads actually spawned: instances_ or submit_threads_
    int batch_ = 1;            // frames retired per successful invocation
    bool multi_instance_ = false;  // $MB_AXELERA_MULTI_INSTANCE, experimental
    std::string artifact_note_;    // why the requested -Ncore build was not used
    std::string sizing_note_;   // why the instance count was clamped, if it was
    std::string async_note_;    // why async was asked for and not used
    std::string ddr_note_;      // device-memory footprint per instance

    std::vector<std::thread> threads_;
    FramePacer pacer_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    size_t completed_ = 0;
    std::string error_;

    // ---- async submission (Voyager >= 1.8) -----------------------------
    // One slot per frame allowed in flight. Each carries its OWN buffers:
    // concurrent submissions into one instance cannot share an input or output
    // buffer, the same reason the submit-thread clones deep-copy theirs.
    // `inst` borrows stage 0's instance and never owns it.
    //
    // The mutex, the condition and the counters live in a refcounted block
    // rather than in the runner, so a completion arriving after a timed-out
    // drain has somewhere valid to land. See on_async_complete().
    struct AsyncState {
        std::mutex mu;
        std::condition_variable cv;
        size_t inflight = 0;     // slots currently submitted
        size_t completed = 0;    // completions not yet counted as frames
        std::string error;
    };
    struct AsyncSlot {
        std::shared_ptr<AsyncState> state;
        std::unique_ptr<Inst> inst;
        bool busy = false;       // guarded by state->mu
    };
    std::shared_ptr<AsyncState> async_state_;
    std::vector<std::unique_ptr<AsyncSlot>> async_slots_;
    bool async_ = false;
    int async_depth_ = 4;
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
