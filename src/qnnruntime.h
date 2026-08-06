// C ABI shim over the Qualcomm AI Runtime (QAIRT / "QNN AI Engine Direct") C API.
//
// VENDORED FILE — the original lives in
// ../envic_ai_aerex/envic/src/components/accelerator/qnn/, where it backs that
// project's Vala QNN plugin. `qnnruntime.cpp` here is byte-identical to it and
// must stay that way (`cmp` after any change) so fixes can be carried in either
// direction with a plain `cp`; this header differs from the original only in
// this comment block. bench_qualcomm.cpp is the only consumer here.
//
// The QNN API is reached by dlopen'ing a backend library and asking it for an
// interface struct — there is no link-time symbol to bind against. That, plus
// the version-tagged tensor unions, is why it lives behind a flat C ABI: it also
// means nothing links against QNN, only libdl.
//
// Backend selection is just which library you load, which is what lets one
// plugin expose the SoC's three compute units as separate devices:
//   libQnnHtp.so  -> Hexagon NPU (HTP)     qnn://htp-0, qnn://htp-1
//   libQnnGpu.so  -> Adreno GPU (OpenCL)   qnn://gpu
//   libQnnCpu.so  -> CPU reference          qnn://cpu
//
// MODEL FORMAT: this shim loads a *context binary* (.bin) produced by
// qnn-context-binary-generator, not a .dlc. That is deliberate — measured on
// QCS9075/HTP v73, loading a cached context takes 14-27 ms against 0.6-2.6 s to
// finalize a graph from scratch, so anything that finalizes per process start is
// a startup-latency bug. A context binary is locked to the HTP architecture it
// was built for; a v73 binary will not load on a v75 part.
//
// I/O CONTRACT: models are compiled with --quantize_io and
// --force_channel_last_input, so graph inputs are uint8 NHWC — exactly what the
// GL tensorizer already emits for the Hailo path. Outputs are likewise
// quantized; the caller dequantizes using the scale/offset reported here.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QnnEngine QnnEngine;  // opaque; owns backend + device + context + graph

#define QNN_SHIM_MAX_RANK 8
#define QNN_SHIM_MAX_NAME 256

// Mirrors the subset of Qnn_DataType_t the pipeline can encounter, flattened so
// Vala does not have to track QNN's encoding scheme.
typedef enum QnnShimDataType {
	QNN_SHIM_DTYPE_UNKNOWN = 0,
	QNN_SHIM_DTYPE_INT8,
	QNN_SHIM_DTYPE_UINT8,
	QNN_SHIM_DTYPE_INT16,
	QNN_SHIM_DTYPE_UINT16,
	QNN_SHIM_DTYPE_INT32,
	QNN_SHIM_DTYPE_UINT32,
	QNN_SHIM_DTYPE_FLOAT16,
	QNN_SHIM_DTYPE_FLOAT32,
} QnnShimDataType;

typedef struct QnnTensorDesc {
	char     name[QNN_SHIM_MAX_NAME];
	uint32_t rank;
	uint32_t dims[QNN_SHIM_MAX_RANK];  // NHWC for image inputs (see I/O contract)
	int      data_type;                // QnnShimDataType
	int      is_quantized;             // 1 => scale/offset meaningful
	float    scale;
	int32_t  offset;                   // QNN convention: real = (q + offset) * scale.
	                                   // Envic's TensorQuantization uses
	                                   // real = (q - zero_point) * scale, so
	                                   // zero_point = -offset.
	uint64_t numel;                    // element count
	uint64_t bytes;                    // numel * sizeof(data_type)
} QnnTensorDesc;

// One addressable accelerator instance as QNN reports it. On QCS9075 the HTP
// backend reports two of these — the SoC's two NSPs (cdsp0, cdsp1) — each with
// its own VTCM, so they can run independent graphs concurrently.
typedef struct QnnDeviceDesc {
	uint32_t device_id;
	uint32_t num_cores;
	uint32_t soc_model;     // QNN SoC id (77 on QCS9075)
	int32_t  arch;          // HTP architecture version (73 here); -1 if not HTP
	uint32_t vtcm_size_mb;
} QnnDeviceDesc;

// Thread-local last-error string (valid until the next failing call in this thread).
const char* qnn_last_error(void);

// 1 if `backend_lib` can be dlopen'd and yields a usable QNN interface. Used by
// the provider to decide which of htp/gpu/cpu to advertise. Never throws.
int qnn_backend_available(const char* backend_lib);

// Number of hardware devices the backend reports (0 on error). Prefer this over
// inferring the count from /dev/fastrpc-* nodes: the kernel exposes a node per
// FastRPC domain whether or not QNN can drive it.
int qnn_backend_device_count(const char* backend_lib);

// Describe the index'th hardware device. Returns 0 on success.
int qnn_backend_device_desc(const char* backend_lib, int index, QnnDeviceDesc* out);

// Build id of a backend library ("" if unavailable). Reported as DeviceInfo.version.
const char* qnn_backend_build_id(const char* backend_lib);

// Load a context binary onto a backend.
//   backend_lib    "libQnnHtp.so" / "libQnnGpu.so" / "libQnnCpu.so"
//   context_binary path to the .bin from qnn-context-binary-generator
//   device_id      QNN device index. This is what selects the NSP on a
//                  multi-NSP part: cdsp0 is device 0 and cdsp1 is device 1 on
//                  QCS9075. Measured — addressing the second NSP as core_id 1
//                  instead fails with "core type for device id 0 core id 1 not
//                  found in platform info" and silently loses the perf mode.
//   core_id        core within the device; 0 on every part seen so far
//   perf_mode      "burst" | "sustained_high_performance" | "high_performance" |
//                  "balanced" | "power_saver" | NULL (leave governor alone).
//                  HTP only; ignored by the GPU and CPU backends. Always state
//                  the mode alongside any latency number — under DCVS a short
//                  inference can finish before the governor ramps.
// Returns NULL on failure; see qnn_last_error().
QnnEngine* qnn_engine_load(const char* backend_lib,
                           const char* context_binary,
                           uint32_t device_id,
                           uint32_t core_id,
                           const char* perf_mode);
void qnn_engine_free(QnnEngine* e);

// Name of the graph retrieved from the context binary ("" if none).
const char* qnn_engine_graph_name(QnnEngine* e);

int qnn_engine_num_inputs(QnnEngine* e);
int qnn_engine_num_outputs(QnnEngine* e);
int qnn_engine_input_desc(QnnEngine* e, int index, QnnTensorDesc* out);   // 0 ok
int qnn_engine_output_desc(QnnEngine* e, int index, QnnTensorDesc* out);  // 0 ok

// Synchronous execution. `inputs`/`outputs` are caller-owned buffers whose sizes
// must match the corresponding descriptor's `bytes`. Not re-entrant for a single
// engine — the caller serialises (the Vala side runs each engine on its own
// executor thread). Returns 0 on success.
int qnn_engine_execute(QnnEngine* e,
                       void* const* inputs, const uint64_t* input_sizes, int num_inputs,
                       void* const* outputs, const uint64_t* output_sizes, int num_outputs);

// ---------------------------------------------------------------- helpers
//
// Hot per-frame loops, kept on this side of the boundary so they vectorise
// (mirrors hailo_repack_rgba_to_rgb).

// Drop the 4th byte of each pixel: rgba[pixels*4] -> rgb[pixels*3]. The GL
// tensorizer always renders and reads back RGBA8, so a 3-channel graph input
// needs this repack.
void qnn_repack_rgba_to_rgb(const uint8_t* rgba, uint8_t* rgb, uint64_t pixels);

// Dequantize a graph output into float32: dst[i] = (src[i] + offset) * scale.
// `src_dtype` is a QnnShimDataType. Returns 0 on success, 1 if the type is not
// a supported quantized integer type.
int qnn_dequantize(const void* src, float* dst, uint64_t count,
                   int src_dtype, float scale, int32_t offset);

#ifdef __cplusplus
}  // extern "C"
#endif
