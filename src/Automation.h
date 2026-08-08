// Unattended benchmark schedules, loaded from a file named with --automation.
//
// This replaced a set of checkboxes in the control panel. The checkboxes could
// only express "sweep every model" or "sweep every pipeline", which is the
// least interesting thing to want: a real unattended session is a specific
// ordered list, often the same subject several times, or one model swept across
// several accelerator settings. A file states that outright, and it is a record
// of what a log came from — the checkbox version left no trace of what had been
// configured, so a finished CSV could not be read back.
//
// GTK-free like Catalog/Probes/Logger, so it is testable headlessly. It pulls
// in Catalog.h only for the `Accel` enum and `accel_vendor()`, which is what
// lets a key be written `axelera.cores`; it deliberately does NOT resolve
// subject ids against the catalog — that is the caller's job, and keeping it
// out means a plan parses and validates with no models tree present.
#pragma once

#include <string>
#include <vector>

#include "Catalog.h"

// Per-card settings a plan may state. Every field has an "unset" value, so a
// step can override one knob without restating the rest; unset falls through to
// the [automation] defaults, and then to whatever the control panel shows.
struct AccelSettings {
    enum { kUnset = -1 };
    int enabled = kUnset;          // 0/1 — is this card in the run at all
    int api = kUnset;              // 0 = sync, 1 = async
    int depth = kUnset;
    int cores = kUnset;            // Axelera AIPU cores
    int freq_mhz = kUnset;         // MemryX MPU clock
    int nsps = kUnset;             // Qualcomm Hexagon NSPs
    std::string perf;              // Qualcomm HTP DCVS mode
    std::string backend;           // Qualcomm QNN backend: htp | gpu | cpu

    // Overlay `o` onto this, field by field: anything `o` states wins, anything
    // it leaves unset is kept. This is what makes a step an *override* of the
    // defaults rather than a replacement for them.
    void overlay(const AccelSettings& o);
};

struct AutomationSettings {
    AccelSettings accel[kAccelCount];
    void overlay(const AutomationSettings& o);
};

struct AutomationStep {
    // `id` is the catalog id — the bit after the colon in a `[model:…]` or
    // `[pipeline:…]` section of models.conf ("resnet50", "track_yolov8s"), not
    // the display name. Ids are stable and shell-safe; display names carry
    // spaces and a "·" and would have to be quoted.
    std::string id;
    bool is_pipeline = false;
    // Per-step overrides, < 0 meaning "use the plan default".
    double bench_seconds = -1.0;
    double idle_seconds = -1.0;
    AutomationSettings settings;
};

struct AutomationPlan {
    bool enabled = false;          // false when no --automation was given
    // Delay before the *first* run, separate from the between-runs idle. A
    // session usually wants a longer settle at the start — the cards have just
    // been powered or the app just launched — without paying that every cycle.
    double start_seconds = 60.0;
    // Delay after the last step, then close the app. < 0 means "stay open",
    // which is the default so a plan that says nothing behaves as before.
    // Only reachable when loop = false; a looping plan never completes.
    //
    // The wait is not cosmetic: it keeps the graphs and the CSV running long
    // enough to capture the cards cooling back to idle, which is the only
    // record of what they settle to.
    double end_seconds = -1.0;
    double bench_seconds = 30 * 60;
    double idle_seconds = 10 * 60;
    bool loop = true;              // wrap at the end, or stop after one pass
    AutomationSettings defaults;
    std::vector<AutomationStep> steps;
    std::string path;              // for the status line and the log

    // Resolved values for a step, honouring its overrides.
    double bench_for(std::size_t i) const;
    double idle_for(std::size_t i) const;
    AutomationSettings settings_for(std::size_t i) const;
};

// Parse `path`. Returns false and fills `error` on a problem — a missing file,
// an unparsable duration, an unknown key, or a plan with no steps. A plan that
// cannot be loaded is fatal at startup rather than silently ignored: the user
// asked for an unattended session and would otherwise come back to an idle app.
bool load_automation_plan(const std::string& path, AutomationPlan& out,
                          std::string& error);
