#include "Catalog.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

// Directory holding this executable; the repo is one level up (binary lives in
// build/), so config/ and models/ resolve regardless of working directory.
fs::path exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return fs::current_path();
    buf[n] = '\0';
    return fs::path(buf).parent_path();
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Minimal INI reader: [section] headers and `key = value` lines, '#' comments.
// Sections are kept in file order (the config's order is the UI's order) and
// duplicate keys within a section take the last value.
struct Section {
    std::string header;                            // "model:yolov8s"
    std::vector<std::pair<std::string, std::string>> kv;

    const std::string* get(const std::string& key) const {
        for (const auto& p : kv)
            if (p.first == key) return &p.second;
        return nullptr;
    }
    std::string value(const std::string& key, const std::string& dflt = {}) const {
        const std::string* v = get(key);
        return v ? *v : dflt;
    }
    // "model:yolov8s" with prefix "model:" -> "yolov8s"; "" if it doesn't match.
    std::string id_for(const std::string& prefix) const {
        if (header.rfind(prefix, 0) != 0) return {};
        return header.substr(prefix.size());
    }
};

std::vector<Section> parse_ini(const std::string& path) {
    std::vector<Section> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            Section s;
            s.header = trim(line.substr(1, line.size() - 2));
            out.push_back(std::move(s));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || out.empty()) continue;
        out.back().kv.emplace_back(trim(line.substr(0, eq)),
                                   trim(line.substr(eq + 1)));
    }
    return out;
}

// "a, b ,c" -> {"a","b","c"}
std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const std::string tok =
            trim(s.substr(start, comma == std::string::npos ? std::string::npos
                                                           : comma - start));
        if (!tok.empty()) out.push_back(tok);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

Fetch parse_fetch(const std::string& s) {
    if (s == "file") return Fetch::File;
    if (s == "zip") return Fetch::Zip;
    if (s == "zipfile") return Fetch::ZipFile;
    return Fetch::None;
}

}  // namespace

const char* accel_name(Accel a) {
    switch (a) {
        case Accel::Hailo:   return "Hailo";
        case Accel::DeepX:   return "DeepX";
        case Accel::Axelera: return "Axelera";
        case Accel::MemryX:  return "MemryX";
    }
    return "?";
}

const char* accel_vendor(Accel a) {
    switch (a) {
        case Accel::Hailo:   return "hailo";
        case Accel::DeepX:   return "deepx";
        case Accel::Axelera: return "axelera";
        case Accel::MemryX:  return "memryx";
    }
    return "?";
}

bool accel_compiled_in(Accel a) {
    switch (a) {
        case Accel::Hailo:
#ifdef MB_HAVE_HAILO
            return true;
#else
            return false;
#endif
        case Accel::DeepX:
#ifdef MB_HAVE_DEEPX
            return true;
#else
            return false;
#endif
        case Accel::Axelera:
#ifdef MB_HAVE_AXELERA
            return true;
#else
            return false;
#endif
        case Accel::MemryX:
#ifdef MB_HAVE_MEMRYX
            return true;
#else
            return false;
#endif
    }
    return false;
}

const char* accel_unavailable_reason(Accel a) {
    if (accel_compiled_in(a)) return "";
    return "SDK not found at build time";
}

bool BenchSubject::configured_anywhere() const {
    for (int i = 0; i < kAccelCount; ++i)
        if (!members[i].empty()) return true;
    return false;
}

void Catalog::discover(std::vector<std::string>* notes) {
    models_.clear();
    pipelines_.clear();
    artifacts_.clear();
    for (auto& v : vendors_) v = VendorSource{};

    const fs::path repo = exe_dir() / "..";

    // Config: $MB_BENCH_MODELS_CONFIG, else <repo>/config/models.conf.
    config_path_.clear();
    {
        std::vector<fs::path> candidates;
        if (const char* e = std::getenv("MB_BENCH_MODELS_CONFIG")) candidates.emplace_back(e);
        candidates.push_back(repo / "config" / "models.conf");
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::is_regular_file(c, ec)) {
                config_path_ = fs::weakly_canonical(c, ec).string();
                break;
            }
        }
    }
    if (config_path_.empty()) {
        if (notes) notes->push_back("catalog: config/models.conf not found — no models offered");
        return;
    }

    // Local models tree: $MB_BENCH_MODELS_ROOT, else <repo>/models.
    {
        std::error_code ec;
        const char* e = std::getenv("MB_BENCH_MODELS_ROOT");
        models_root_ = fs::weakly_canonical(e ? fs::path(e) : repo / "models", ec).string();
    }

    const auto sections = parse_ini(config_path_);

    // ---- vendors ----
    for (const auto& s : sections) {
        const std::string vendor = s.id_for("vendor:");
        if (vendor.empty()) continue;
        for (int i = 0; i < kAccelCount; ++i) {
            if (vendor != accel_vendor(accel_at(i))) continue;
            VendorSource& v = vendors_[i];
            v.vendor = vendor;
            v.accelerator = s.value("accelerator", vendor);
            v.url = s.value("url");
            v.fetch = parse_fetch(s.value("fetch", "none"));
            v.strip = s.value("strip");
            v.dir = (fs::path(models_root_) / v.vendor / v.accelerator).string();
            if (v.url.empty()) v.fetch = Fetch::None;
        }
    }

    // ---- models ----
    // Track each model's per-vendor artifact name so pipelines can reuse it.
    std::map<std::string, std::string> artifact_of[kAccelCount];  // model id -> name
    std::map<std::string, bool> local_of[kAccelCount];

    for (const auto& s : sections) {
        const std::string id = s.id_for("model:");
        if (id.empty()) continue;

        BenchSubject sub;
        sub.id = id;
        sub.name = s.value("name", id);
        sub.detail = s.value("detail");
        sub.is_pipeline = false;

        for (int i = 0; i < kAccelCount; ++i) {
            const char* vk = accel_vendor(accel_at(i));
            const std::string* art = s.get(vk);
            if (!art || art->empty()) continue;
            const bool local = s.value(std::string(vk) + ".source") == "local";
            artifact_of[i][id] = *art;
            local_of[i][id] = local;
            // path filled in by resolve_paths()
            sub.members[i].push_back(BenchMember{sub.name, *art, 1});

            Artifact a;
            a.accel = accel_at(i);
            a.name = *art;
            a.local = local;
            // De-duplicate: the same artifact appears in several pipelines.
            const bool seen = std::any_of(
                artifacts_.begin(), artifacts_.end(), [&](const Artifact& x) {
                    return x.accel == a.accel && x.name == a.name;
                });
            if (!seen) artifacts_.push_back(std::move(a));
        }
        if (sub.configured_anywhere()) models_.push_back(std::move(sub));
    }

    // ---- pipelines: offered for a vendor only when every stage resolves ----
    for (const auto& s : sections) {
        const std::string id = s.id_for("pipeline:");
        if (id.empty()) continue;

        BenchSubject sub;
        sub.id = id;
        sub.name = s.value("name", id);
        sub.detail = s.value("detail");
        sub.is_pipeline = true;

        const auto member_ids = split_list(s.value("members"));
        for (int i = 0; i < kAccelCount; ++i) {
            std::vector<BenchMember> stages;
            bool complete = !member_ids.empty();
            for (const auto& mid : member_ids) {
                auto it = artifact_of[i].find(mid);
                if (it == artifact_of[i].end()) { complete = false; break; }
                // Display the stage under its model's own name.
                std::string stage_name = mid;
                for (const auto& m : models_)
                    if (m.id == mid) { stage_name = m.name; break; }
                stages.push_back(BenchMember{stage_name, it->second, 1});
            }
            if (complete) sub.members[i] = std::move(stages);
        }
        if (sub.configured_anywhere()) pipelines_.push_back(std::move(sub));
    }

    resolve_paths();
    refresh_presence();

    if (notes) {
        notes->push_back("catalog: " + config_path_);
        notes->push_back("models tree: " + models_root_);
        notes->push_back("catalog: " + std::to_string(models_.size()) + " models, " +
                         std::to_string(pipelines_.size()) + " pipelines, " +
                         std::to_string(artifacts_.size()) + " artifacts");
    }
}

void Catalog::resolve_paths() {
    // Rewrite every member's `path` from the artifact name to its local path.
    auto fix = [&](std::vector<BenchSubject>& subjects) {
        for (auto& s : subjects) {
            for (int i = 0; i < kAccelCount; ++i) {
                for (auto& m : s.members[i]) {
                    m.path = (fs::path(vendors_[i].dir) / m.path).string();
                }
            }
        }
    };
    fix(models_);
    fix(pipelines_);
    for (auto& a : artifacts_) {
        a.path = (fs::path(vendors_[accel_index(a.accel)].dir) / a.name).string();
    }
}

void Catalog::refresh_presence() {
    // An Axelera artifact is a directory of blobs; everything else is a file.
    auto exists = [](Accel a, const std::string& p) {
        std::error_code ec;
        return a == Accel::Axelera ? fs::is_directory(p, ec) : fs::is_regular_file(p, ec);
    };
    for (auto& a : artifacts_) a.present = exists(a.accel, a.path);

    auto mark = [&](std::vector<BenchSubject>& subjects) {
        for (auto& s : subjects) {
            for (int i = 0; i < kAccelCount; ++i) {
                bool ok = !s.members[i].empty();
                for (const auto& m : s.members[i]) {
                    if (!exists(accel_at(i), m.path)) { ok = false; break; }
                }
                s.ready[i] = ok;
            }
        }
    };
    mark(models_);
    mark(pipelines_);
}

std::vector<Artifact> Catalog::missing_downloadable() const {
    std::vector<Artifact> out;
    for (const auto& a : artifacts_) {
        if (a.present || a.local) continue;
        if (vendors_[accel_index(a.accel)].fetch == Fetch::None) continue;
        out.push_back(a);
    }
    return out;
}
