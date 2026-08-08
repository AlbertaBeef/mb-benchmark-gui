// CSV logging of everything the app measures, one row per 1 Hz tick.
//
// Deliberately GTK-free, like Probes and Catalog — it is the only file-writing
// code in the app and must be testable headlessly.
//
// The telemetry columns are byte-compatible with the sibling Python TUI
// `../mb-powermon`, so `../mb-powermon/csv-to-html-plot.py` plots our files
// unchanged: first column `time` (local naive ISO-8601, millisecond precision,
// read *positionally* by that tool), then `<bdf>_<LABEL>` per metric, `%.6f`,
// and **a missing reading is the empty string** — never `nan`, never `0`. That
// last rule is load-bearing: `0.0 W` is a real INA228 overflow signal, and the
// plotter renders empty as a gap.
//
// Everything after the telemetry is ours and has no counterpart there: the
// benchmark state, the per-card configuration, the per-card results, and a
// message column. The plotter warns once per unrecognised column and ignores
// them, which is the intended cost.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

class Logger {
public:
    ~Logger();

    // Resolve where to write. Honours, in order:
    //   $MB_BENCH_NO_LOG=1   — disabled, every call becomes a no-op
    //   $MB_BENCH_LOG=<file> — exact path, no timestamp
    //   $MB_BENCH_LOG_DIR=<dir>/mb-benchmark-YYYYMMDD-HHMMSS.csv
    //   <exe>/../logs/mb-benchmark-YYYYMMDD-HHMMSS.csv
    // Same env-var-then-repo-relative shape as $MB_BENCH_MODELS_CONFIG.
    static std::string default_path();

    // Open (truncating) and write the header. `telemetry` are the `<bdf>_<LABEL>`
    // column names in the order values will be supplied. Safe to call with an
    // empty path — the logger simply stays disabled.
    //
    // The header is written once and never revisited: unlike mb-powermon, whose
    // INA228 labels mutate after rail classification, our metric lists are fixed
    // for the process lifetime (Probes::discover() runs exactly once).
    bool open(const std::string& path, const std::vector<std::string>& telemetry);

    bool enabled() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }

    // Queue a diagnostic for the *next* row, so it lands on the second it
    // happened. Several in one tick are joined with "; ".
    void note(const std::string& msg);

    // One tick. `telemetry` must match the width passed to open(); NaN is
    // written as an empty field. `cfg`/`fps`/`fps_per_w`/`mj_per_frame` are all
    // kAccelCount long, in Accel order.
    struct Row {
        const std::vector<double>* telemetry = nullptr;
        std::string bench_state;                  // idle|starting|running|…
        std::string bench_model;
        double target_fps = 0.0;                  // <= 0 → "max speed", logged empty
        const std::vector<std::string>* cfg = nullptr;
        const std::vector<double>* fps = nullptr;
        const std::vector<double>* fps_per_w = nullptr;
        const std::vector<double>* mj_per_frame = nullptr;
    };
    void write_row(const Row& r);

    void close();

private:
    void write_line(const std::string& s);

    std::FILE* f_ = nullptr;
    std::string path_;
    std::size_t telemetry_cols_ = 0;
    std::vector<std::string> pending_;   // notes awaiting the next row
    bool broken_ = false;                // a write failed; stay quiet afterwards
};
