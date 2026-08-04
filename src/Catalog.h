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
enum class Accel { Hailo, MemryX, DeepX, Axelera };
inline constexpr int kAccelCount = 4;

inline int accel_index(Accel a) { return static_cast<int>(a); }
inline Accel accel_at(int i) { return static_cast<Accel>(i); }

// Display name — also the `device_name` Probes reports for this card, which is
// what pairs a benchmark's frame rate with the watts that produced it.
const char* accel_name(Accel a);
// Lowercase vendor key used in the config and in the models path ("hailo").
const char* accel_vendor(Accel a);

// Was this backend's SDK found at build time? A backend that wasn't compiled in
// still appears in the UI (greyed out, with the reason) rather than vanishing.
bool accel_compiled_in(Accel a);
// Why a backend is unavailable, or "" when it is fine.
const char* accel_unavailable_reason(Accel a);

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
    bool ready[kAccelCount] = {false, false, false, false};

    bool configured(Accel a) const { return !members[accel_index(a)].empty(); }
    bool runnable(Accel a) const {
        return configured(a) && ready[accel_index(a)];
    }
    bool configured_anywhere() const;
};

// A single unit of work handed to the engine: one subject on one card.
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
    // Frames each card keeps outstanding in Async mode. Sync is always 1.
    // Axelera has no async API and ignores this — its knob is `cores`.
    int threads = 4;
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
