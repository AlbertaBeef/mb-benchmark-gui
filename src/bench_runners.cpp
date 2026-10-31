// make_runner(): picks the per-backend BenchRunner implementation, or explains
// why there isn't one. Keeps every SDK #ifdef in a single place, and records
// which (accelerator, mode) pairs the SDKs actually implement natively.
#include "Bench.h"

#ifdef MB_HAVE_HAILO
std::unique_ptr<BenchRunner> make_hailo_runner(ApiMode mode);
#endif
#ifdef MB_HAVE_DEEPX
std::unique_ptr<BenchRunner> make_deepx_runner(ApiMode mode);
#endif
#ifdef MB_HAVE_AXELERA
std::unique_ptr<BenchRunner> make_axelera_runner(ApiMode mode);
#endif
#ifdef MB_HAVE_MEMRYX
std::unique_ptr<BenchRunner> make_memryx_runner(ApiMode mode);
#endif
#ifdef MB_HAVE_QUALCOMM
std::unique_ptr<BenchRunner> make_qualcomm_runner(ApiMode mode);
#endif
#ifdef MB_HAVE_AXELERA
void axelera_prepare_environment();
#endif
#ifdef MB_HAVE_QUALCOMM
void qualcomm_prepare_environment();
#endif

void prepare_backend_environment() {
#ifdef MB_HAVE_AXELERA
    axelera_prepare_environment();
#endif
#ifdef MB_HAVE_QUALCOMM
    qualcomm_prepare_environment();
#endif
}

const char* api_mode_name(ApiMode m) {
    return m == ApiMode::Sync ? "Sync API" : "Async API";
}

// Verified 2026-08-04 against the installed headers AND `nm -D` on the shipped
// libraries — not assumed, and not taken from vendor docs:
//   Hailo    — InferVStreams::infer() / ConfiguredInferModel::run(), and
//              ConfiguredInferModel::run_async() + AsyncInferJob::wait().
//              (Two further async surfaces exist — raw Stream read_async/
//              write_async, and the internal ConfiguredNetworkGroup::infer_async
//              — neither is used here.)
//   DeepX    — InferenceEngine::Run(), and RunAsync() -> jobId + Wait(jobId).
//              RegisterCallback() also exists; unused. No queue-depth query, so
//              the depth is ours to self-throttle.
//   MemryX   — MxAcclMT::run() blocks with a timeout; MxAccl::connect_stream()
//              is the callback/streaming surface. NOTE: it is the *class* that
//              differs, not the SDK — an earlier version of this file claimed
//              MemryX had no blocking call, which was wrong.
//   Axelera  — axr_run_model_instance() only. All 41 axr_* symbols enumerated;
//              exactly one is inference and it blocks. The 233 ze* symbols are
//              the statically linked oneAPI Level Zero loader — model-agnostic,
//              so not an async path without reimplementing the model loader.
//   Qualcomm — QnnGraph_execute() only; the shim exposes no submit/poll pair and
//              nothing in it references executeAsync. Verified on the aarch64
//              IQ-9075; the QNN headers are absent on x86_64 hosts, so this one
//              cannot be re-checked here.
bool api_mode_is_native(Accel a, ApiMode m) {
    switch (a) {
        case Accel::Hailo:
        case Accel::DeepX:
        case Accel::MemryX:
            return true;  // both modes native
        // Axelera's async entry point is real and, since 2026-09-20, measured
        // WORKING on 1.8.0 (axr::AsyncExecutor, 387.7 fps on ResNet-50 batch 1)
        // — but the vendor still marks it INTERNAL and not part of the stable
        // ABI, and it refuses any multi-kernel artifact, so it cannot reach the
        // 4-core batched build that sync uses. Not "native" in the sense this
        // predicate means: "the vendor supports it".
        case Accel::Axelera:
        case Accel::Qualcomm:
            return m == ApiMode::Sync;
    }
    return false;
}

// No `default:` on purpose — -Wswitch then catches a newly added Accel here.
// (make_runner()'s switch below *does* have one and will not warn; check it by
// hand when adding a card.)
bool accel_has_both_api_modes(Accel a) {
    switch (a) {
        case Accel::Hailo:
        case Accel::DeepX:
        case Accel::MemryX:
            return true;
        // Axelera joined this list with Voyager 1.8, which added
        // axr_run_async_model_instance(). The mode is OFFERED even though the
        // call currently returns AXR_ERROR_NOT_IMPLEMENTED — same principle as
        // the Qualcomm Backend control: the entry point genuinely exists, the
        // failure is loud and names its own cause, and hiding it would mean the
        // UI disagreeing with the SDK. See api_mode_note().
        case Accel::Axelera:
            return true;
        case Accel::Qualcomm:
            return false;
    }
    return false;
}

// The mode a card's row starts in.
//
// Async everywhere it is the faster path — and on four of the five cards it is,
// by 3-4x. Axelera is the exception and starts in **Sync**: its 1.8 async
// executor refuses any multi-kernel artifact, so choosing Async drops the run
// to the batch-1 build and ~1/5 of the throughput (387.7 fps against 1967 for
// 4-core batched sync, ResNet-50, measured 2026-09-20). A default that quietly
// costs 5x is not a default.
//
// No `default:` — -Wswitch then catches a newly added Accel here.
ApiMode accel_default_api_mode(Accel a) {
    switch (a) {
        case Accel::Hailo:
        case Accel::DeepX:
        case Accel::MemryX:
        case Accel::Qualcomm:
            return ApiMode::Async;
        case Accel::Axelera:
            return ApiMode::Sync;
    }
    return ApiMode::Async;
}

const char* accel_sole_api_mode_note(Accel a) {
    switch (a) {
        case Accel::Hailo:
        case Accel::DeepX:
        case Accel::MemryX:
            return "";
        case Accel::Axelera:
            return "";   // both modes offered since 1.8; see api_mode_note()
        case Accel::Qualcomm:
            return "Sync only \u2014 QNN has no async graph-execute";
    }
    return "";
}

const char* api_mode_note(Accel a, ApiMode m) {
    if (api_mode_is_native(a, m)) return "";
    if (a == Accel::Axelera) {
        // Measured, not feared: it runs. What it cannot do is use a
        // multi-core build — AsyncExecutor takes one kernel at stage 0 — so
        // the runner drops to batch 1 and the card runs ~5x slower than
        // 4-core sync. The note has to say that, or a mode that "works" reads
        // as the faster one.
        return "1.8 internal API · batch-1 build only · slower than sync";
    }
    // Unlike Axelera's, this one is a measured ~1.45x at depth 4 — but it is
    // host-side pipelining, not a vendor async API, and must not read as one.
    if (a == Accel::Qualcomm) return "host threads · no async API";
    return "emulated";
}

std::unique_ptr<BenchRunner> make_runner(Accel a, ApiMode mode, std::string* err) {
    switch (a) {
#ifdef MB_HAVE_HAILO
        case Accel::Hailo: return make_hailo_runner(mode);
#endif
#ifdef MB_HAVE_DEEPX
        case Accel::DeepX: return make_deepx_runner(mode);
#endif
#ifdef MB_HAVE_AXELERA
        case Accel::Axelera: return make_axelera_runner(mode);
#endif
#ifdef MB_HAVE_MEMRYX
        case Accel::MemryX: return make_memryx_runner(mode);
#endif
#ifdef MB_HAVE_QUALCOMM
        case Accel::Qualcomm: return make_qualcomm_runner(mode);
#endif
        default: break;
    }
    if (err) {
        *err = std::string(accel_name(a)) + ": " + accel_unavailable_reason(a);
    }
    return nullptr;
}
