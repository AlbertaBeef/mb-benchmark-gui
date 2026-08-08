#include "Logger.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sys/time.h>

#include "Catalog.h"

namespace fs = std::filesystem;

namespace {

// Third copy of this helper, after Catalog.cpp and Probes.cpp. The duplication
// is deliberate there — it keeps each of those GTK-free and independent of the
// others — and a logger that pulled in either just to find its own directory
// would undo that.
fs::path exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return fs::current_path();
    buf[n] = '\0';
    return fs::path(buf).parent_path();
}

bool env_flag(const char* name) {
    const char* e = std::getenv(name);
    return e && *e && std::strcmp(e, "0") != 0;
}

// Local naive ISO-8601 with millisecond precision — "2026-08-07T14:23:05.123".
// Matches mb-powermon's `datetime.now().isoformat(timespec="milliseconds")`,
// which is what csv-to-html-plot.py feeds to datetime.fromisoformat().
std::string iso_now() {
    struct timeval tv{};
    ::gettimeofday(&tv, nullptr);
    struct tm lt{};
    ::localtime_r(&tv.tv_sec, &lt);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
    char out[80];
    std::snprintf(out, sizeof(out), "%s.%03d", buf,
                  static_cast<int>(tv.tv_usec / 1000));
    return out;
}

// Hostname. Logs get collected from several machines (the x86_64 four-card box
// and the aarch64 IQ-9075), and a row that does not say where it came from is
// ambiguous the moment two files are concatenated.
std::string host_name() {
    char b[256];
    if (::gethostname(b, sizeof(b) - 1) != 0) return "unknown";
    b[sizeof(b) - 1] = '\0';
    return b;
}

std::string stamp_for_name() {
    const std::time_t t = std::time(nullptr);
    struct tm lt{};
    ::localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &lt);
    return buf;
}

// %.6f, or empty for a missing reading. NEVER "nan" and never "0" — see the
// header comment; the distinction is what makes a gap a gap.
std::string num(double v) {
    if (std::isnan(v)) return {};
    char b[40];
    std::snprintf(b, sizeof(b), "%.6f", v);
    return b;
}

// RFC4180 quoting, used only for the free-text columns. Telemetry columns are
// never quoted, so a file with no benchmark running is byte-identical in shape
// to one mb-powermon would produce.
std::string quote(std::string s) {
    for (auto& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

}  // namespace

Logger::~Logger() { close(); }

std::string Logger::default_path() {
    if (env_flag("MB_BENCH_NO_LOG")) return {};
    if (const char* e = std::getenv("MB_BENCH_LOG")) {
        if (*e) return e;
    }
    const std::string name = "mb-benchmark-" + stamp_for_name() + ".csv";
    if (const char* d = std::getenv("MB_BENCH_LOG_DIR")) {
        if (*d) return (fs::path(d) / name).string();
    }
    // Same convention as config/ and models/: the binary lives in build/, so the
    // repo is one level up and the path works from any working directory.
    std::error_code ec;
    return fs::weakly_canonical(exe_dir() / ".." / "logs" / name, ec).string();
}

bool Logger::open(const std::string& path,
                  const std::vector<std::string>& telemetry) {
    close();
    if (path.empty()) return false;

    // Fetcher's discipline: create_directories with an error_code, never the
    // throwing filesystem overloads. This runs on the GTK main loop.
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    f_ = std::fopen(path.c_str(), "w");
    if (!f_) return false;
    // Line-buffered, so `tail -f` stays current and a hard kill loses at most a
    // partial final line. mb-powermon's `buffering=1` contract.
    std::setvbuf(f_, nullptr, _IOLBF, 0);
    path_ = path;
    telemetry_cols_ = telemetry.size();
    broken_ = false;

    std::string h = "time,host";
    for (const auto& c : telemetry) h += "," + c;
    h += ",bench_state,bench_model,bench_target_fps";
    for (int i = 0; i < kAccelCount; ++i)
        h += "," + std::string(accel_vendor(accel_at(i))) + "_cfg";
    for (int i = 0; i < kAccelCount; ++i)
        h += "," + std::string(accel_vendor(accel_at(i))) + "_fps";
    for (int i = 0; i < kAccelCount; ++i)
        h += "," + std::string(accel_vendor(accel_at(i))) + "_fps_per_w";
    for (int i = 0; i < kAccelCount; ++i)
        h += "," + std::string(accel_vendor(accel_at(i))) + "_mj_per_frame";
    h += ",message";
    write_line(h);
    return enabled();
}

void Logger::note(const std::string& msg) {
    if (!f_ || msg.empty()) return;
    pending_.push_back(msg);
}

void Logger::write_line(const std::string& s) {
    if (!f_ || broken_) return;
    if (std::fputs(s.c_str(), f_) < 0 || std::fputc('\n', f_) == EOF) {
        // Self-disable rather than throw into the tick, exactly as mb-powermon
        // drops its file handle on OSError.
        broken_ = true;
    }
}

void Logger::write_row(const Row& r) {
    if (!f_ || broken_) return;

    // `time` stays column 0: ../mb-powermon/csv-to-html-plot.py reads that
    // position as the timestamp, not by name, so anything before it is a hard
    // ValueError rather than a skipped column. host goes immediately after.
    std::string line = iso_now() + "," + quote(host_name());

    // Telemetry. Width is fixed at open(); if a caller ever disagrees, pad or
    // truncate rather than shifting every later column out of its header.
    for (std::size_t i = 0; i < telemetry_cols_; ++i) {
        line += ",";
        if (r.telemetry && i < r.telemetry->size()) line += num((*r.telemetry)[i]);
    }

    line += "," + r.bench_state;
    line += "," + quote(r.bench_model);
    // <= 0 means "max speed", which is an absence of a target rather than a
    // target of zero — so it is written empty.
    line += "," + (r.target_fps > 0.0 ? num(r.target_fps) : std::string());

    auto emit_str = [&](const std::vector<std::string>* v) {
        for (int i = 0; i < kAccelCount; ++i) {
            line += ",";
            line += quote(v && static_cast<std::size_t>(i) < v->size() ? (*v)[i]
                                                                      : std::string());
        }
    };
    auto emit_num = [&](const std::vector<double>* v) {
        for (int i = 0; i < kAccelCount; ++i) {
            line += ",";
            if (v && static_cast<std::size_t>(i) < v->size()) line += num((*v)[i]);
        }
    };
    emit_str(r.cfg);
    emit_num(r.fps);
    emit_num(r.fps_per_w);
    emit_num(r.mj_per_frame);

    std::string msg;
    for (const auto& m : pending_) {
        if (!msg.empty()) msg += "; ";
        msg += m;
    }
    pending_.clear();
    line += "," + quote(msg);

    write_line(line);
}

void Logger::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
    pending_.clear();
    telemetry_cols_ = 0;
    broken_ = false;
}
