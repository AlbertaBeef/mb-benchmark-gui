// C ABI shim over the QAIRT / QNN C API. See qnnruntime.h for the contract.
//
// Shape follows hailoruntime.cpp: one engine owns one graph, every entry point
// catches and converts to an error code, and no C++ exception may unwind across
// the boundary.
//
// The QNN API is not linked against — a backend library is dlopen'd and asked
// for an interface struct via QnnInterface_getProviders. Tensor metadata for a
// context binary comes from a second library, libQnnSystem.so, which can parse a
// .bin without instantiating it on the accelerator.
#include "qnnruntime.h"

#include <dlfcn.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The SDK headers include each other with quoted relative paths, so the QNN
// include root itself must be on the search path (-I/usr/include/QNN), not just
// /usr/include. See meson.build.
#include "QnnInterface.h"
#include "QnnTypes.h"
#include "HTP/QnnHtpDevice.h"
#include "HTP/QnnHtpPerfInfrastructure.h"
#include "System/QnnSystemInterface.h"

namespace {

thread_local std::string g_last_error;
void set_error(const std::string& m) { g_last_error = m; }

std::string qnn_err(const char* what, Qnn_ErrorHandle_t e) {
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s failed (0x%llx)", what,
	              static_cast<unsigned long long>(e));
	return buf;
}

// ---------------------------------------------------------------- interfaces

struct BackendLib {
	void*                  handle = nullptr;
	QNN_INTERFACE_VER_TYPE iface{};
	std::string            build_id;
	// One backend handle per library, shared by every engine. QNN expects a
	// single backend per process for a given accelerator; creating one per engine
	// hands out two handles onto the same hardware.
	Qnn_LogHandle_t        log = nullptr;
	Qnn_BackendHandle_t    backend = nullptr;
	bool                   started = false;
};

// Backend libraries are dlopen'd once per process and shared: opening the same
// .so twice would hand out two backend handles onto one accelerator.
std::mutex                          g_lib_mutex;
std::map<std::string, BackendLib>   g_libs;

BackendLib* load_backend(const std::string& lib) {
	std::lock_guard<std::mutex> lock(g_lib_mutex);
	auto it = g_libs.find(lib);
	if (it != g_libs.end()) return &it->second;

	void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		set_error(std::string("dlopen(") + lib + "): " + dlerror());
		return nullptr;
	}

	using GetProvidersFn = Qnn_ErrorHandle_t (*)(const QnnInterface_t***, uint32_t*);
	auto get_providers = reinterpret_cast<GetProvidersFn>(dlsym(h, "QnnInterface_getProviders"));
	if (!get_providers) {
		set_error(lib + ": QnnInterface_getProviders not found");
		dlclose(h);
		return nullptr;
	}

	const QnnInterface_t** providers = nullptr;
	uint32_t num = 0;
	auto e = get_providers(&providers, &num);
	if (e != QNN_SUCCESS || !providers || num == 0) {
		set_error(lib + ": " + qnn_err("QnnInterface_getProviders", e));
		dlclose(h);
		return nullptr;
	}

	// Pick the provider whose core API major version we were built against.
	const QnnInterface_t* chosen = nullptr;
	for (uint32_t i = 0; i < num; i++) {
		if (providers[i]->apiVersion.coreApiVersion.major == QNN_API_VERSION_MAJOR) {
			chosen = providers[i];
			break;
		}
	}
	if (!chosen) {
		set_error(lib + ": no provider matching QNN core API major version " +
		          std::to_string(QNN_API_VERSION_MAJOR));
		dlclose(h);
		return nullptr;
	}

	BackendLib bl;
	bl.handle = h;
	bl.iface = chosen->QNN_INTERFACE_VER_NAME;

	const char* build = nullptr;
	if (bl.iface.backendGetBuildId && bl.iface.backendGetBuildId(&build) == QNN_SUCCESS && build) {
		bl.build_id = build;
	}

	auto res = g_libs.emplace(lib, bl);
	return &res.first->second;
}

// Create the shared backend handle for a library, once.
bool ensure_backend(BackendLib* bl, std::string* err) {
	std::lock_guard<std::mutex> lock(g_lib_mutex);
	if (bl->started) return true;
	if (bl->iface.logCreate) {
		bl->iface.logCreate(nullptr, QNN_LOG_LEVEL_ERROR, &bl->log);  // best effort
	}
	auto e = bl->iface.backendCreate(bl->log, nullptr, &bl->backend);
	if (e != QNN_SUCCESS) {
		*err = qnn_err("QnnBackend_create", e);
		return false;
	}
	bl->started = true;
	return true;
}

// Create a QNN device pinned to one hardware device id.
//
// QnnDevice_create with a null config always instantiates hardware device 0, so
// passing a device id to the load call is not by itself enough to place a graph
// on a particular NSP — the device has to be created from a QnnDevice_PlatformInfo_t
// that contains only the wanted entry. Getting this wrong is quiet: the model
// loads and runs, but on the wrong NSP and without its performance mode.
bool create_device_for(BackendLib* bl, uint32_t device_id, Qnn_DeviceHandle_t* out,
                       std::string* err) {
	*out = nullptr;
	if (!bl->iface.deviceCreate) return true;   // backend without device support

	const QnnDevice_PlatformInfo_t* full = nullptr;
	if (!bl->iface.deviceGetPlatformInfo ||
	    bl->iface.deviceGetPlatformInfo(bl->log, &full) != QNN_SUCCESS || !full ||
	    full->version != QNN_DEVICE_PLATFORM_INFO_VERSION_1) {
		// No platform info (CPU/GPU backends): fall back to the default device.
		if (full && bl->iface.deviceFreePlatformInfo) {
			bl->iface.deviceFreePlatformInfo(bl->log, full);
		}
		auto e = bl->iface.deviceCreate(bl->log, nullptr, out);
		if (e != QNN_SUCCESS) { *err = qnn_err("QnnDevice_create", e); return false; }
		return true;
	}

	const QnnDevice_HardwareDeviceInfo_t* match = nullptr;
	for (uint32_t i = 0; i < full->v1.numHwDevices; i++) {
		const auto& d = full->v1.hwDevices[i];
		if (d.version == QNN_DEVICE_HARDWARE_DEVICE_INFO_VERSION_1 &&
		    d.v1.deviceId == device_id) {
			match = &full->v1.hwDevices[i];
			break;
		}
	}
	if (!match) {
		*err = "no hardware device with id " + std::to_string(device_id) +
		       " (backend reports " + std::to_string(full->v1.numHwDevices) + ")";
		bl->iface.deviceFreePlatformInfo(bl->log, full);
		return false;
	}

	// A platform-info view holding just the one device. `only` aliases `full`'s
	// core array, so `full` must outlive the deviceCreate call.
	QnnDevice_HardwareDeviceInfo_t one_hw = *match;
	QnnDevice_PlatformInfo_t only = QNN_DEVICE_PLATFORM_INFO_INIT;
	only.version = QNN_DEVICE_PLATFORM_INFO_VERSION_1;
	only.v1.numHwDevices = 1;
	only.v1.hwDevices = &one_hw;

	QnnDevice_Config_t cfg = QNN_DEVICE_CONFIG_INIT;
	cfg.option = QNN_DEVICE_CONFIG_OPTION_PLATFORM_INFO;
	cfg.hardwareInfo = &only;
	const QnnDevice_Config_t* cfgs[] = {&cfg, nullptr};

	auto e = bl->iface.deviceCreate(bl->log, cfgs, out);
	bl->iface.deviceFreePlatformInfo(bl->log, full);
	if (e != QNN_SUCCESS) {
		*err = qnn_err("QnnDevice_create", e) + " for device id " + std::to_string(device_id);
		return false;
	}
	return true;
}

struct SystemLib {
	void*                         handle = nullptr;
	QNN_SYSTEM_INTERFACE_VER_TYPE iface{};
};

std::mutex g_sys_mutex;
SystemLib* g_sys = nullptr;

SystemLib* load_system() {
	std::lock_guard<std::mutex> lock(g_sys_mutex);
	if (g_sys) return g_sys;

	void* h = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		set_error(std::string("dlopen(libQnnSystem.so): ") + dlerror());
		return nullptr;
	}

	using GetProvidersFn = Qnn_ErrorHandle_t (*)(const QnnSystemInterface_t***, uint32_t*);
	auto get_providers =
	    reinterpret_cast<GetProvidersFn>(dlsym(h, "QnnSystemInterface_getProviders"));
	if (!get_providers) {
		set_error("libQnnSystem.so: QnnSystemInterface_getProviders not found");
		dlclose(h);
		return nullptr;
	}

	const QnnSystemInterface_t** providers = nullptr;
	uint32_t num = 0;
	auto e = get_providers(&providers, &num);
	if (e != QNN_SUCCESS || !providers || num == 0) {
		set_error(qnn_err("QnnSystemInterface_getProviders", e));
		dlclose(h);
		return nullptr;
	}

	auto* sl = new SystemLib();
	sl->handle = h;
	sl->iface = providers[0]->QNN_SYSTEM_INTERFACE_VER_NAME;
	g_sys = sl;
	return g_sys;
}

// ------------------------------------------------------- tensor accessors

// Qnn_Tensor_t is a version-tagged union; every field access has to go through
// the right arm. Only v1 and v2 exist in the SDK we build against.
#define TENSOR_FIELD(t, f) ((t).version == QNN_TENSOR_VERSION_1 ? (t).v1.f : (t).v2.f)

int map_dtype(Qnn_DataType_t dt, int* quantized) {
	*quantized = 0;
	switch (dt) {
	case QNN_DATATYPE_SFIXED_POINT_8:  *quantized = 1; return QNN_SHIM_DTYPE_INT8;
	case QNN_DATATYPE_UFIXED_POINT_8:  *quantized = 1; return QNN_SHIM_DTYPE_UINT8;
	case QNN_DATATYPE_SFIXED_POINT_16: *quantized = 1; return QNN_SHIM_DTYPE_INT16;
	case QNN_DATATYPE_UFIXED_POINT_16: *quantized = 1; return QNN_SHIM_DTYPE_UINT16;
	case QNN_DATATYPE_SFIXED_POINT_32: *quantized = 1; return QNN_SHIM_DTYPE_INT32;
	case QNN_DATATYPE_UFIXED_POINT_32: *quantized = 1; return QNN_SHIM_DTYPE_UINT32;
	case QNN_DATATYPE_INT_8:    return QNN_SHIM_DTYPE_INT8;
	case QNN_DATATYPE_UINT_8:   return QNN_SHIM_DTYPE_UINT8;
	case QNN_DATATYPE_INT_16:   return QNN_SHIM_DTYPE_INT16;
	case QNN_DATATYPE_UINT_16:  return QNN_SHIM_DTYPE_UINT16;
	case QNN_DATATYPE_INT_32:   return QNN_SHIM_DTYPE_INT32;
	case QNN_DATATYPE_UINT_32:  return QNN_SHIM_DTYPE_UINT32;
	case QNN_DATATYPE_FLOAT_16: return QNN_SHIM_DTYPE_FLOAT16;
	case QNN_DATATYPE_FLOAT_32: return QNN_SHIM_DTYPE_FLOAT32;
	default:                    return QNN_SHIM_DTYPE_UNKNOWN;
	}
}

uint64_t dtype_size(int shim_dtype) {
	switch (shim_dtype) {
	case QNN_SHIM_DTYPE_INT8:
	case QNN_SHIM_DTYPE_UINT8:    return 1;
	case QNN_SHIM_DTYPE_INT16:
	case QNN_SHIM_DTYPE_UINT16:
	case QNN_SHIM_DTYPE_FLOAT16:  return 2;
	case QNN_SHIM_DTYPE_INT32:
	case QNN_SHIM_DTYPE_UINT32:
	case QNN_SHIM_DTYPE_FLOAT32:  return 4;
	default:                      return 0;
	}
}

}  // namespace

// -------------------------------------------------------------- the engine

struct QnnEngine {
	BackendLib*             lib = nullptr;   // shared; log/backend owned by it
	Qnn_DeviceHandle_t      device = nullptr;
	Qnn_ContextHandle_t     context = nullptr;
	Qnn_GraphHandle_t       graph = nullptr;
	std::string             graph_name;

	// Execution tensors, owned here. The Qnn_Tensor_t structs handed out by the
	// system context die with it, so names and dimensions are deep-copied.
	std::vector<Qnn_Tensor_t>          in_tensors, out_tensors;
	std::vector<std::string>           in_names, out_names;
	std::vector<std::vector<uint32_t>> in_dims, out_dims;
	std::vector<QnnTensorDesc>         in_desc, out_desc;

	// The backend and log belong to the shared BackendLib and outlive the engine.
	~QnnEngine() {
		if (!lib) return;
		if (context && lib->iface.contextFree) lib->iface.contextFree(context, nullptr);
		if (device && lib->iface.deviceFree)   lib->iface.deviceFree(device);
	}
};

namespace {

// Copy one metadata tensor into an execution tensor + a flat descriptor.
bool adopt_tensor(const Qnn_Tensor_t& src, Qnn_Tensor_t& dst, std::string& name_store,
                  std::vector<uint32_t>& dim_store, QnnTensorDesc& desc, std::string* err) {
	if (src.version != QNN_TENSOR_VERSION_1 && src.version != QNN_TENSOR_VERSION_2) {
		*err = "unsupported Qnn_Tensor_t version";
		return false;
	}

	const char* nm = TENSOR_FIELD(src, name);
	uint32_t rank = TENSOR_FIELD(src, rank);
	uint32_t* dims = TENSOR_FIELD(src, dimensions);
	Qnn_DataType_t dt = TENSOR_FIELD(src, dataType);
	Qnn_QuantizeParams_t qp = TENSOR_FIELD(src, quantizeParams);

	if (rank == 0 || rank > QNN_SHIM_MAX_RANK) {
		*err = "tensor rank " + std::to_string(rank) + " out of range";
		return false;
	}

	name_store = nm ? nm : "";
	dim_store.assign(dims, dims + rank);

	std::memset(&desc, 0, sizeof(desc));
	std::snprintf(desc.name, sizeof(desc.name), "%s", name_store.c_str());
	desc.rank = rank;
	uint64_t numel = 1;
	for (uint32_t i = 0; i < rank; i++) {
		desc.dims[i] = dim_store[i];
		numel *= dim_store[i];
	}
	desc.data_type = map_dtype(dt, &desc.is_quantized);
	if (desc.data_type == QNN_SHIM_DTYPE_UNKNOWN) {
		*err = "unsupported tensor data type for '" + name_store + "'";
		return false;
	}
	desc.numel = numel;
	desc.bytes = numel * dtype_size(desc.data_type);
	desc.scale = 1.0f;
	desc.offset = 0;
	if (desc.is_quantized &&
	    qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
	    qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
		desc.scale = qp.scaleOffsetEncoding.scale;
		desc.offset = qp.scaleOffsetEncoding.offset;
	}

	// Execution tensor: same identity, RAW client buffer bound at execute time.
	dst = src;
	if (dst.version == QNN_TENSOR_VERSION_1) {
		dst.v1.name = name_store.c_str();
		dst.v1.dimensions = dim_store.data();
		dst.v1.memType = QNN_TENSORMEMTYPE_RAW;
		dst.v1.clientBuf = Qnn_ClientBuffer_t{nullptr, 0};
	} else {
		dst.v2.name = name_store.c_str();
		dst.v2.dimensions = dim_store.data();
		dst.v2.memType = QNN_TENSORMEMTYPE_RAW;
		dst.v2.clientBuf = Qnn_ClientBuffer_t{nullptr, 0};
		dst.v2.isDynamicDimensions = nullptr;
	}
	return true;
}

// Apply an HTP DCVS power mode. Silently no-ops on non-HTP backends, which do
// not expose a perf infrastructure.
void apply_perf_mode(QnnEngine* e, uint32_t device_id, uint32_t core_id, const char* mode) {
	if (!mode || !*mode) return;
	if (!e->lib->iface.deviceGetInfrastructure) return;

	QnnDevice_Infrastructure_t infra = nullptr;
	if (e->lib->iface.deviceGetInfrastructure(&infra) != QNN_SUCCESS || !infra) return;

	auto* htp = static_cast<QnnHtpDevice_Infrastructure_t*>(infra);
	if (htp->infraType != QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF) return;
	auto& perf = htp->perfInfra;
	if (!perf.createPowerConfigId || !perf.setPowerConfig) return;

	uint32_t id = 0;
	if (perf.createPowerConfigId(device_id, core_id, &id) != QNN_SUCCESS) return;

	QnnHtpPerfInfrastructure_PowerConfig_t cfg = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIG_INIT;
	cfg.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
	auto& d = cfg.dcvsV3Config;
	d.contextId = id;
	d.setSleepLatency = 1;
	d.setBusParams = 1;
	d.setCoreParams = 1;
	d.setDcvsEnable = 1;

	std::string m(mode);
	if (m == "burst" || m == "high_performance") {
		// Pin to the top corner and disable DCVS so short inferences do not
		// finish before the governor ramps.
		d.dcvsEnable = 0;
		d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_PERFORMANCE_MODE;
		d.sleepLatency = (m == "burst") ? 40 : 100;
		d.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
		d.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
		d.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
		d.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
		d.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
		d.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
	} else if (m == "sustained_high_performance") {
		// Steady-state pipelines: burst on a passively cooled board thermally
		// throttles into a *worse* sustained number than this.
		d.dcvsEnable = 1;
		d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_ADJUST_UP_DOWN;
		d.sleepLatency = 100;
		d.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
		d.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_TURBO;
		d.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_TURBO;
		d.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
		d.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_TURBO;
		d.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_TURBO;
	} else if (m == "power_saver") {
		d.dcvsEnable = 1;
		d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_POWER_SAVER_MODE;
		d.sleepLatency = 1000;
		d.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
		d.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS;
		d.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS;
		d.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
		d.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_SVS;
		d.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_SVS;
	} else {  // "balanced" and anything unrecognised: let DCVS do its thing
		d.dcvsEnable = 1;
		d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_ADJUST_UP_DOWN;
		d.sleepLatency = 1000;
		d.busVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
		d.busVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_NOM;
		d.busVoltageCornerMax = DCVS_VOLTAGE_VCORNER_TURBO;
		d.coreVoltageCornerMin = DCVS_VOLTAGE_VCORNER_SVS;
		d.coreVoltageCornerTarget = DCVS_VOLTAGE_VCORNER_NOM;
		d.coreVoltageCornerMax = DCVS_VOLTAGE_VCORNER_TURBO;
	}

	const QnnHtpPerfInfrastructure_PowerConfig_t* configs[] = {&cfg, nullptr};
	perf.setPowerConfig(id, configs);
}

}  // namespace

extern "C" {

const char* qnn_last_error(void) { return g_last_error.c_str(); }

int qnn_backend_available(const char* backend_lib) {
	if (!backend_lib) return 0;
	try {
		return load_backend(backend_lib) != nullptr ? 1 : 0;
	} catch (const std::exception& ex) {
		set_error(ex.what());
		return 0;
	}
}

// Walk the backend's platform info, invoking `fn(index, hwDeviceInfo)`.
// Returns the device count, or -1 on failure.
static int with_platform_info(const char* backend_lib,
                              const std::function<void(int, const QnnDevice_HardwareDeviceInfo_t&)>& fn) {
	BackendLib* bl = load_backend(backend_lib);
	if (!bl) return -1;
	std::string err;
	if (!ensure_backend(bl, &err)) { set_error(err); return -1; }
	if (!bl->iface.deviceGetPlatformInfo) { set_error("backend has no platform info"); return -1; }

	const QnnDevice_PlatformInfo_t* info = nullptr;
	auto e = bl->iface.deviceGetPlatformInfo(bl->log, &info);
	if (e != QNN_SUCCESS || !info) { set_error(qnn_err("QnnDevice_getPlatformInfo", e)); return -1; }
	if (info->version != QNN_DEVICE_PLATFORM_INFO_VERSION_1) {
		bl->iface.deviceFreePlatformInfo(bl->log, info);
		set_error("unsupported platform info version");
		return -1;
	}
	int n = (int)info->v1.numHwDevices;
	if (fn) {
		for (int i = 0; i < n; i++) fn(i, info->v1.hwDevices[i]);
	}
	bl->iface.deviceFreePlatformInfo(bl->log, info);
	return n;
}

int qnn_backend_device_count(const char* backend_lib) {
	if (!backend_lib) return 0;
	try {
		int n = with_platform_info(backend_lib, nullptr);
		return n < 0 ? 0 : n;
	} catch (const std::exception& ex) {
		set_error(ex.what());
		return 0;
	}
}

int qnn_backend_device_desc(const char* backend_lib, int index, QnnDeviceDesc* out) {
	if (!backend_lib || !out || index < 0) { set_error("bad device_desc args"); return 1; }
	try {
		bool found = false;
		std::memset(out, 0, sizeof(*out));
		out->arch = -1;
		int n = with_platform_info(backend_lib,
			[&](int i, const QnnDevice_HardwareDeviceInfo_t& d) {
				if (i != index || d.version != QNN_DEVICE_HARDWARE_DEVICE_INFO_VERSION_1) return;
				found = true;
				out->device_id = d.v1.deviceId;
				out->num_cores = d.v1.numCores;
				auto* ext = static_cast<QnnHtpDevice_DeviceInfoExtension_t*>(d.v1.deviceInfoExtension);
				if (ext) {
					out->soc_model = ext->onChipDevice.socModel;
					out->arch = (int32_t)ext->onChipDevice.arch;
					out->vtcm_size_mb = (uint32_t)ext->onChipDevice.vtcmSize;
				}
			});
		if (n < 0) return 1;
		if (!found) { set_error("device index out of range"); return 1; }
		return 0;
	} catch (const std::exception& ex) {
		set_error(ex.what());
		return 1;
	}
}

const char* qnn_backend_build_id(const char* backend_lib) {
	static thread_local std::string out;
	out.clear();
	if (!backend_lib) return out.c_str();
	try {
		BackendLib* bl = load_backend(backend_lib);
		if (bl) out = bl->build_id;
	} catch (const std::exception& ex) {
		set_error(ex.what());
	}
	return out.c_str();
}

QnnEngine* qnn_engine_load(const char* backend_lib,
                           const char* context_binary,
                           uint32_t device_id,
                           uint32_t core_id,
                           const char* perf_mode) {
	try {
		if (!backend_lib || !context_binary) throw std::runtime_error("null argument");

		BackendLib* bl = load_backend(backend_lib);
		if (!bl) throw std::runtime_error(g_last_error);

		SystemLib* sl = load_system();
		if (!sl) throw std::runtime_error(g_last_error);

		std::ifstream f(context_binary, std::ios::binary | std::ios::ate);
		if (!f) throw std::runtime_error(std::string("cannot open ") + context_binary);
		std::streamsize sz = f.tellg();
		f.seekg(0, std::ios::beg);
		std::vector<uint8_t> blob(static_cast<size_t>(sz));
		if (!f.read(reinterpret_cast<char*>(blob.data()), sz)) {
			throw std::runtime_error(std::string("cannot read ") + context_binary);
		}

		std::string berr;
		if (!ensure_backend(bl, &berr)) throw std::runtime_error(berr);

		auto e = std::make_unique<QnnEngine>();
		e->lib = bl;

		Qnn_ErrorHandle_t err;
		if (!create_device_for(bl, device_id, &e->device, &berr)) {
			throw std::runtime_error(berr);
		}

		apply_perf_mode(e.get(), device_id, core_id, perf_mode);

		// --- tensor metadata, read from the binary without instantiating it ---
		QnnSystemContext_Handle_t sys = nullptr;
		if ((err = sl->iface.systemContextCreate(&sys)) != QNN_SUCCESS) {
			throw std::runtime_error(qnn_err("QnnSystemContext_create", err));
		}
		const QnnSystemContext_BinaryInfo_t* info = nullptr;
		Qnn_ContextBinarySize_t info_size = 0;
		err = sl->iface.systemContextGetBinaryInfo(sys, blob.data(), blob.size(),
		                                           &info, &info_size);
		if (err != QNN_SUCCESS || !info) {
			sl->iface.systemContextFree(sys);
			throw std::runtime_error(qnn_err("QnnSystemContext_getBinaryInfo", err));
		}

		uint32_t num_graphs = 0;
		const QnnSystemContext_GraphInfo_t* graphs = nullptr;
		switch (info->version) {
		case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1:
			num_graphs = info->contextBinaryInfoV1.numGraphs;
			graphs = info->contextBinaryInfoV1.graphs;
			break;
		case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2:
			num_graphs = info->contextBinaryInfoV2.numGraphs;
			graphs = info->contextBinaryInfoV2.graphs;
			break;
		case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3:
			num_graphs = info->contextBinaryInfoV3.numGraphs;
			graphs = info->contextBinaryInfoV3.graphs;
			break;
		default:
			sl->iface.systemContextFree(sys);
			throw std::runtime_error("unsupported context binary info version");
		}
		if (num_graphs == 0 || !graphs) {
			sl->iface.systemContextFree(sys);
			throw std::runtime_error("context binary contains no graphs");
		}

		// One graph per engine, matching the Hailo/DeepX shims.
		const char* gname = nullptr;
		uint32_t n_in = 0, n_out = 0;
		const Qnn_Tensor_t *tin = nullptr, *tout = nullptr;
		switch (graphs[0].version) {
		case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1:
			gname = graphs[0].graphInfoV1.graphName;
			n_in = graphs[0].graphInfoV1.numGraphInputs;
			tin = graphs[0].graphInfoV1.graphInputs;
			n_out = graphs[0].graphInfoV1.numGraphOutputs;
			tout = graphs[0].graphInfoV1.graphOutputs;
			break;
		case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2:
			gname = graphs[0].graphInfoV2.graphName;
			n_in = graphs[0].graphInfoV2.numGraphInputs;
			tin = graphs[0].graphInfoV2.graphInputs;
			n_out = graphs[0].graphInfoV2.numGraphOutputs;
			tout = graphs[0].graphInfoV2.graphOutputs;
			break;
		case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3:
			gname = graphs[0].graphInfoV3.graphName;
			n_in = graphs[0].graphInfoV3.numGraphInputs;
			tin = graphs[0].graphInfoV3.graphInputs;
			n_out = graphs[0].graphInfoV3.numGraphOutputs;
			tout = graphs[0].graphInfoV3.graphOutputs;
			break;
		default:
			sl->iface.systemContextFree(sys);
			throw std::runtime_error("unsupported graph info version");
		}

		e->graph_name = gname ? gname : "";
		e->in_tensors.resize(n_in);   e->in_names.resize(n_in);
		e->in_dims.resize(n_in);      e->in_desc.resize(n_in);
		e->out_tensors.resize(n_out); e->out_names.resize(n_out);
		e->out_dims.resize(n_out);    e->out_desc.resize(n_out);

		std::string aerr;
		for (uint32_t i = 0; i < n_in; i++) {
			if (!adopt_tensor(tin[i], e->in_tensors[i], e->in_names[i], e->in_dims[i],
			                  e->in_desc[i], &aerr)) {
				sl->iface.systemContextFree(sys);
				throw std::runtime_error("input " + std::to_string(i) + ": " + aerr);
			}
		}
		for (uint32_t i = 0; i < n_out; i++) {
			if (!adopt_tensor(tout[i], e->out_tensors[i], e->out_names[i], e->out_dims[i],
			                  e->out_desc[i], &aerr)) {
				sl->iface.systemContextFree(sys);
				throw std::runtime_error("output " + std::to_string(i) + ": " + aerr);
			}
		}
		sl->iface.systemContextFree(sys);

		// --- instantiate ---
		err = bl->iface.contextCreateFromBinary(bl->backend, e->device, nullptr,
		                                        blob.data(), blob.size(),
		                                        &e->context, nullptr);
		if (err != QNN_SUCCESS) {
			throw std::runtime_error(qnn_err("QnnContext_createFromBinary", err));
		}
		if ((err = bl->iface.graphRetrieve(e->context, e->graph_name.c_str(), &e->graph)) !=
		    QNN_SUCCESS) {
			throw std::runtime_error(qnn_err("QnnGraph_retrieve", err));
		}

		return e.release();
	} catch (const std::exception& ex) {
		set_error(ex.what());
		return nullptr;
	}
}

void qnn_engine_free(QnnEngine* e) { delete e; }

const char* qnn_engine_graph_name(QnnEngine* e) {
	return e ? e->graph_name.c_str() : "";
}

int qnn_engine_num_inputs(QnnEngine* e) {
	return e ? static_cast<int>(e->in_desc.size()) : 0;
}

int qnn_engine_num_outputs(QnnEngine* e) {
	return e ? static_cast<int>(e->out_desc.size()) : 0;
}

int qnn_engine_input_desc(QnnEngine* e, int index, QnnTensorDesc* out) {
	if (!e || !out || index < 0 || index >= static_cast<int>(e->in_desc.size())) {
		set_error("bad input_desc args");
		return 1;
	}
	*out = e->in_desc[index];
	return 0;
}

int qnn_engine_output_desc(QnnEngine* e, int index, QnnTensorDesc* out) {
	if (!e || !out || index < 0 || index >= static_cast<int>(e->out_desc.size())) {
		set_error("bad output_desc args");
		return 1;
	}
	*out = e->out_desc[index];
	return 0;
}

int qnn_engine_execute(QnnEngine* e,
                       void* const* inputs, const uint64_t* input_sizes, int num_inputs,
                       void* const* outputs, const uint64_t* output_sizes, int num_outputs) {
	try {
		if (!e || !e->graph) throw std::runtime_error("null engine");
		if (num_inputs != static_cast<int>(e->in_tensors.size()) ||
		    num_outputs != static_cast<int>(e->out_tensors.size())) {
			throw std::runtime_error("tensor count mismatch");
		}

		for (int i = 0; i < num_inputs; i++) {
			if (!inputs[i] || input_sizes[i] < e->in_desc[i].bytes) {
				throw std::runtime_error("input " + std::to_string(i) + " buffer too small");
			}
			Qnn_ClientBuffer_t buf{inputs[i], static_cast<uint32_t>(e->in_desc[i].bytes)};
			if (e->in_tensors[i].version == QNN_TENSOR_VERSION_1) {
				e->in_tensors[i].v1.clientBuf = buf;
			} else {
				e->in_tensors[i].v2.clientBuf = buf;
			}
		}
		for (int i = 0; i < num_outputs; i++) {
			if (!outputs[i] || output_sizes[i] < e->out_desc[i].bytes) {
				throw std::runtime_error("output " + std::to_string(i) + " buffer too small");
			}
			Qnn_ClientBuffer_t buf{outputs[i], static_cast<uint32_t>(e->out_desc[i].bytes)};
			if (e->out_tensors[i].version == QNN_TENSOR_VERSION_1) {
				e->out_tensors[i].v1.clientBuf = buf;
			} else {
				e->out_tensors[i].v2.clientBuf = buf;
			}
		}

		auto err = e->lib->iface.graphExecute(e->graph,
		                                      e->in_tensors.data(), num_inputs,
		                                      e->out_tensors.data(), num_outputs,
		                                      nullptr, nullptr);
		if (err != QNN_SUCCESS) throw std::runtime_error(qnn_err("QnnGraph_execute", err));
		return 0;
	} catch (const std::exception& ex) {
		set_error(ex.what());
		return 1;
	}
}

// Both of these run on every inference — the repack once per input, the
// dequantize once per output tensor — so at 6 people the cascade spends ~21 ms
// a frame in here. Scalar versions cost ~4.7 ns/pixel and ~2.8 ns/element on an
// A78C; the NEON paths below are ~10x that. The scalar code is kept as the tail
// handler and the non-aarch64 fallback, and both paths must produce identical
// bytes.
void qnn_repack_rgba_to_rgb(const uint8_t* rgba, uint8_t* rgb, uint64_t pixels) {
	uint64_t i = 0;
#if defined(__aarch64__)
	// vld4q_u8 de-interleaves 16 RGBA pixels into four lanes; vst3q_u8
	// re-interleaves three of them, dropping alpha for free.
	for (; i + 16 <= pixels; i += 16) {
		uint8x16x4_t src = vld4q_u8(rgba + i * 4);
		uint8x16x3_t dst;
		dst.val[0] = src.val[0];
		dst.val[1] = src.val[1];
		dst.val[2] = src.val[2];
		vst3q_u8(rgb + i * 3, dst);
	}
#endif
	for (; i < pixels; i++) {
		rgb[i * 3 + 0] = rgba[i * 4 + 0];
		rgb[i * 3 + 1] = rgba[i * 4 + 1];
		rgb[i * 3 + 2] = rgba[i * 4 + 2];
	}
}

int qnn_dequantize(const void* src, float* dst, uint64_t count,
                   int src_dtype, float scale, int32_t offset) {
	switch (src_dtype) {
	case QNN_SHIM_DTYPE_UINT8: {
		const uint8_t* s = static_cast<const uint8_t*>(src);
		uint64_t i = 0;
#if defined(__aarch64__)
		// (q + offset) * scale is a widen-convert-fma, which vectorises far
		// better than the 256-entry lookup table it replaces: a LUT forces a
		// dependent load per element and cannot be gathered on NEON.
		const float32x4_t vscale = vdupq_n_f32(scale);
		const float32x4_t voff = vdupq_n_f32(static_cast<float>(offset));
		for (; i + 16 <= count; i += 16) {
			uint8x16_t q = vld1q_u8(s + i);
			uint16x8_t lo = vmovl_u8(vget_low_u8(q));
			uint16x8_t hi = vmovl_u8(vget_high_u8(q));
			uint32x4_t w[4] = {vmovl_u16(vget_low_u16(lo)), vmovl_u16(vget_high_u16(lo)),
			                   vmovl_u16(vget_low_u16(hi)), vmovl_u16(vget_high_u16(hi))};
			for (int k = 0; k < 4; k++) {
				float32x4_t f = vaddq_f32(vcvtq_f32_u32(w[k]), voff);
				vst1q_f32(dst + i + k * 4, vmulq_f32(f, vscale));
			}
		}
#endif
		for (; i < count; i++) dst[i] = (static_cast<float>(s[i]) + offset) * scale;
		return 0;
	}
	case QNN_SHIM_DTYPE_INT8: {
		float lut[256];
		for (int v = 0; v < 256; v++) {
			lut[v] = (static_cast<float>(static_cast<int8_t>(v)) + offset) * scale;
		}
		const uint8_t* s = static_cast<const uint8_t*>(src);
		for (uint64_t i = 0; i < count; i++) dst[i] = lut[s[i]];
		return 0;
	}
	case QNN_SHIM_DTYPE_UINT16: {
		const uint16_t* s = static_cast<const uint16_t*>(src);
		for (uint64_t i = 0; i < count; i++) dst[i] = (static_cast<float>(s[i]) + offset) * scale;
		return 0;
	}
	case QNN_SHIM_DTYPE_INT16: {
		const int16_t* s = static_cast<const int16_t*>(src);
		for (uint64_t i = 0; i < count; i++) dst[i] = (static_cast<float>(s[i]) + offset) * scale;
		return 0;
	}
	case QNN_SHIM_DTYPE_INT32: {
		const int32_t* s = static_cast<const int32_t*>(src);
		for (uint64_t i = 0; i < count; i++) dst[i] = (static_cast<float>(s[i]) + offset) * scale;
		return 0;
	}
	case QNN_SHIM_DTYPE_UINT32: {
		const uint32_t* s = static_cast<const uint32_t*>(src);
		for (uint64_t i = 0; i < count; i++) dst[i] = (static_cast<float>(s[i]) + offset) * scale;
		return 0;
	}
	case QNN_SHIM_DTYPE_FLOAT32: {
		std::memcpy(dst, src, count * sizeof(float));
		return 0;
	}
	default:
		set_error("qnn_dequantize: unsupported source data type");
		return 1;
	}
}

}  // extern "C"
