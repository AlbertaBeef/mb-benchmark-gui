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

// Verified against the installed headers, not assumed:
//   Hailo   — InferVStreams::infer()      and InferModel::run_async()
//   DeepX   — InferenceEngine::Run()      and RunAsync()/Wait()
//   Axelera — axr_run_model_instance() only; 38 axr_* entry points, one of them
//             inference, blocking. No submit/enqueue exists.
//   MemryX  — connect_stream() only; no blocking call exists at all.
//   Qualcomm— QnnGraph_execute() only; blocking, and the shim exposes no
//             submit/poll pair. Async is host threads over the two NSPs.
bool api_mode_is_native(Accel a, ApiMode m) {
    switch (a) {
        case Accel::Hailo:
        case Accel::DeepX:
            return true;  // both modes native
        case Accel::Axelera:
        case Accel::Qualcomm:
            return m == ApiMode::Sync;
        case Accel::MemryX:
            return m == ApiMode::Async;
    }
    return false;
}

const char* api_mode_note(Accel a, ApiMode m) {
    if (api_mode_is_native(a, m)) return "";
    if (a == Accel::Axelera) return "double-buffered · no async API";
    if (a == Accel::MemryX) return "1 in flight · no sync API";
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
