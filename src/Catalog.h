// What can be benchmarked, where it comes from, and where it lives locally.
//
// The catalog is *data*, read from `config/models.conf` — see that file for the
// format. Nothing about which models exist is compiled in; adding one is an
// edit to the config.
//
// It is organised around *logical* subjects rather than files: one row is
// "YOLOv8s", not "yolov8s.hef". Each row resolves to a different artifact per
// vendor (`yolov8s.hef` / `YoloV8S.dxnn` / `yolov8s-coco-onnx`), which is what
// makes the interesting comparison possible — the same model run on several
// accelerators at once, their FPS and FPS/W side by side.
//
// Artifacts live under `models/<vendor>/<accelerator>/` and are downloaded on
// demand by Fetcher; see `local_path()`.
//
// No GTK dependency: pure data, like Probes.
#pragma once

#include <string>
#include <vector>

// Accelerators this GUI can drive. The order fixes the series order in every
// graph and indexes the per-accelerator arrays below.
// Qualcomm is appended rather than inserted: accel_index()/accel_at() are raw
// casts, so the position of an existing card is load-bearing (graph series
// order, the watts[] slots, Catalog::vendors_[]).
enum class Accel { Hailo, MemryX, DeepX, Axelera, Qualcomm };
inline constexpr int kAccelCount = 5;

inline int accel_index(Accel a) { return static_cast<int>(a); }
inline Accel accel_at(int i) { return static_cast<Accel>(i); }

// Display name — also the `device_name` Probes reports for this card, which is
// what pairs a benchmark's frame rate with the watts that produced it.
const char* accel_name(Accel a);
// Lowercase vendor key used in the config and in the models path ("hailo").
const char* accel_vendor(Accel a);

// Was this backend's SDK found at build time?
bool accel_compiled_in(Accel a);
// Why a backend is unavailable, or "" when it is fine.
const char* accel_unavailable_reason(Accel a);

// Should this card appear in the UI at all?
//
// A card that fails this is hidden *completely* — no Inference checkbox, no
// settings tab, no Graphs filter checkbox, no legend entry and no trace on the
// benchmark graphs. This replaced showing every card greyed-out with its
// reason: on a host that builds one or two backends (the IQ-9075 builds Hailo
// and Qualcomm only) the greyed majority was most of the panel, and a Qualcomm
// card greyed because a model has no artifact looked identical to a MemryX card
// greyed because the SDK is absent.
//
// Today this is purely build-time. It deliberately does NOT try to detect a
// compiled-in card whose hardware is unplugged: that needs each vendor's
// enumeration API on the GUI thread at startup, and a false negative would hide
// a working card — worse than showing one that then reports a clear load error.
// Extend *here* if runtime detection is ever wanted; every UI site asks this
// one question.
bool accel_present(Accel a);

// How a vendor's artifacts are obtained.
enum class Fetch {
    File,     // GET <url>/<artifact>, save as-is
    Zip,      // GET <url>/<artifact>.zip, extract `strip` flat into <artifact>/
    ZipFile,  // GET <url>/<stem(artifact)>.zip, extract the single <artifact>
    None,     // no public source; the user drops files in by hand
};

// One vendor's download source and local layout, from a [vendor:*] section.
struct VendorSource {
    std::string vendor;       // "hailo"
    std::string accelerator;  // "hailo-8" — the models/<vendor>/<here>/ segment
    std::string url;          // base URL, artifact name appended
    Fetch fetch = Fetch::None;
    std::string strip;        // Zip only; %m expands to the artifact name
    std::string dir;          // resolved models/<vendor>/<accelerator>
};

// One inference stage of a benchmark subject.
struct BenchMember {
    std::string name;  // "yolov8s"
    std::string path;  // absolute path to the artifact (Axelera: its directory)
    // How many times this stage runs per benchmark frame. Detectors run once;
    // a recognizer in a real cascade would run once per detected object, but a
    // synthetic benchmark has no detections, so every stage is 1 here. Kept as
    // a field so a "crops per frame" knob can be added without a redesign.
    int reps = 1;
};

// One artifact that may need downloading.
struct Artifact {
    Accel accel = Accel::Hailo;
    std::string name;   // artifact name as it appears in the config
    std::string path;   // where it must end up locally
    bool local = false;  // no public source — never fetch, just use if present
    bool present = false;
};

// A logical model or pipeline, plus the per-vendor resolution of its stages.
// `members[i]` is empty when accelerator `i` has no artifact configured.
struct BenchSubject {
    std::string id;      // "yolov8s" / "track_yolov8s"
    std::string name;    // "YOLOv8s"
    std::string detail;  // "person detection · 640×640"
    bool is_pipeline = false;
    std::vector<BenchMember> members[kAccelCount];
    // True when every stage's artifact is on disk for that accelerator. A
    // subject that is configured but not yet downloaded shows as pending.
    bool ready[kAccelCount] = {false, false, false, false, false};

    bool configured(Accel a) const { return !members[accel_index(a)].empty(); }
    bool runnable(Accel a) const {
        return configured(a) && ready[accel_index(a)];
    }
    bool configured_anywhere() const;
};

// A single unit of work handed to the engine: one subject on one card.
// Which of a vendor's two inference APIs to drive. Lives here rather than in
// Bench.h because BenchItem carries it per card, and Bench.h already includes
// this header; putting it the other way round would invert the dependency.
//
// The five SDKs are NOT symmetric — see accel_has_both_api_modes(). Hailo,
// DeepX and MemryX ship both a blocking and an async/streaming surface; Axelera
// and Qualcomm have no async inference API at all.
enum class ApiMode { Sync, Async };

struct BenchItem {
    std::string label;  // "YOLOv8s"
    Accel accel = Accel::Hailo;
    std::vector<BenchMember> members;
    // Per-card settings from the accelerator tabs. Defaults mean "leave alone".
    // Axelera: AIPU cores to claim (num_sub_devices on axr_device_connect).
    // The Metis has 4. This is the card's real concurrency primitive — an
    // earlier version exposed "streams" (host threads) instead and derived the
    // core count from it, which opened one device connection per stream and
    // made each connect reload the card's firmware.
    // 2 by default: claiming all four wedges this Metis, and 3 sits one step
    // from that cliff (see bench_axelera.cpp and Known issues).
    int cores = 2;
    int freq_mhz = 0;   // MemryX: MPU clock to request, 0 = don't touch it
    // Which inference API this card drives. Per card, not per run: the SDKs are
    // not symmetric, so one global choice offered a mode that does not exist on
    // some of them. Cards without both modes ignore this and always run the one
    // they have.
    ApiMode api_mode = ApiMode::Async;
    // Frames this card keeps in flight. Every backend has some form of it, but
    // the mechanism differs — see the Depth control in each tab:
    //   Hailo    async queue depth (capped by get_async_queue_size())
    //   DeepX    RunAsync job depth
    //   MemryX   connect_stream permit depth
    //   Axelera  double_buffer — the card's ONLY buffering knob, so 1 or 2
    //   Qualcomm engines in flight per NSP
    // On a card offering both API modes, Sync forces it to 1 by definition.
    int depth = 4;
    // Qualcomm: how many of the SoC's NSPs to claim. QCS9075 has 2, each with
    // its own VTCM, addressed as QNN device ids 0 and 1. Measured ~2.3x on
    // OSNet, so unlike Axelera's cores there is no reason to default below the
    // maximum — nothing here wedges.
    int nsps = 2;
    // Qualcomm: HTP DCVS mode. Empty means "leave the governor alone", which is
    // not a neutral choice — a short inference can finish before the governor
    // ramps, so the default states a mode rather than inheriting one.
    std::string perf_mode = "burst";
    // Qualcomm: which QNN backend library to dlopen. Only the HTP one can load
    // the context binaries the catalog ships; see bench_qualcomm.cpp.
    std::string qnn_backend;
};

class Catalog {
public:
    // Read config/models.conf and resolve every artifact against the local
    // models tree. `notes` (optional) collects human-readable diagnostics.
    // Safe to call again to pick up newly downloaded files.
    void discover(std::vector<std::string>* notes = nullptr);
    // Re-stat every artifact and refresh the `ready` flags, without re-parsing.
    void refresh_presence();

    const std::vector<BenchSubject>& models() const { return models_; }
    const std::vector<BenchSubject>& pipelines() const { return pipelines_; }

    // Every distinct artifact the config names, in download order.
    const std::vector<Artifact>& artifacts() const { return artifacts_; }
    const VendorSource& vendor(Accel a) const { return vendors_[accel_index(a)]; }

    // Root of the local models tree (`<repo>/models`), and the config file
    // actually read (empty when none was found).
    const std::string& models_root() const { return models_root_; }
    const std::string& config_path() const { return config_path_; }
    // Artifacts that are missing and have a source — what Fetcher will pull.
    std::vector<Artifact> missing_downloadable() const;

private:
    void resolve_paths();

    std::vector<BenchSubject> models_, pipelines_;
    std::vector<Artifact> artifacts_;
    VendorSource vendors_[kAccelCount];
    std::string models_root_, config_path_;
};
