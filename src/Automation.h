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

#include <limits>
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

// Fan duty for an RA6M5 probe board's EMC2101, with an optional ramp.
//
// Not per-accelerator: there is one fan and it cools whatever is in front of
// it, so this sits beside `benchmark`/`idle` as a plan-and-step setting rather
// than under a `<vendor>.` prefix.
//
// `duty` is held from the moment a run starts. The ramp, if stated, takes over
// `after` seconds in and steps every `every` seconds until it reaches `max` —
// which is a thermal experiment's natural shape: hold one condition until the
// card settles, then walk the cooling and watch it respond.
//
// Durations are seconds here and MINUTES in the file, like every other
// duration in a plan; load_automation_plan() converts.
//
// Two things the caller should know. The EMC2101 has 64 duty levels, so a
// requested percent is rounded to the nearest 1/63 (1.587 %) and consecutive
// 1 % requests can land on the same level — the board reports what it actually
// applied, and that readback is what the CSV's FAN_DUTY column carries, so
// read the applied value and not the request. And the firmware keeps its duty
// across host disconnects, so a plan that never mentions the fan leaves it
// exactly as it was.
struct FanPlan {
    static constexpr double kUnset = -1.0;
    double duty  = kUnset;   // % held from the start of a run
    double start = kUnset;   // ramp: the first duty it applies
    double step  = kUnset;   // added at every interval; may be negative
    double every = kUnset;   // seconds between increments
    double after = kUnset;   // seconds into the run before the ramp begins
    double max   = kUnset;   // the duty the ramp stops at (a floor if step < 0)

    // The duty to hold whenever NO loop is driving: before the first step, and
    // during any step whose mode is off. Deliberately separate from `duty`, and
    // deliberately exempt from conflicts() — it is the complement of the loop
    // rather than a rival to it, so "fan off for half an hour, then hand it to
    // the board" is expressible. Without it that plan cannot be written at all:
    // a bare `fan` is refused alongside `fan.mode`, correctly, and the
    // plan-level fan is never applied during the start pause.
    double idle = kUnset;

    // The RA6M5's on-board control loop. Plain ints rather than the enums in
    // Probes.h: Automation is pure data and deliberately depends on nothing,
    // the same reason it does not resolve catalog ids. MainWindow translates.
    //
    // ONE MODE since firmware 0.6.0 — `host` and `smart` were the same control
    // law with a different constant, and the input selector was never a real
    // choice. See firmware/mb-probe-ra6m5/Control.h.
    static constexpr int kModeUnset = -1, kModeOff = 0, kModeOn = 1;
    static constexpr int kFlagUnset = -1, kFlagOff = 0, kFlagOn = 1;
    int mode = kModeUnset;
    int tc = kFlagUnset;          // fan.tc — use the physical thermocouple
    int nn = kFlagUnset;          // fan.nn — substitute the learned estimate
    int target_auto = kFlagUnset; // fan.target = auto: the board's own bound
    // NaN, not kUnset: a setpoint of -1 °C is silly but legal (the firmware
    // accepts -40..150), and reusing -1 as the sentinel would quietly swallow
    // it. Test with std::isnan, never with ==.
    double target = std::numeric_limits<double>::quiet_NaN();

    // A ramp needs all three of start/step/every; anything less is a constant.
    bool ramps() const {
        return start != kUnset && step != kUnset && every != kUnset;
    }
    bool loops() const { return mode != kModeUnset && mode != kModeOff; }
    // A ramp and a loop both drive the fan, so a plan asking for both cannot
    // mean anything. Checked at load, where a bad plan is fatal by design.
    // `idle` is not in here on purpose: it applies only when the loop is not.
    bool conflicts() const { return loops() && (ramps() || duty != kUnset); }
    // Whether this plan says anything about the fan at all. A plan that says
    // nothing must not touch it.
    bool states_anything() const {
        return duty != kUnset || idle != kUnset || ramps() || mode != kModeUnset;
    }
    // The duty wanted `t` seconds into a run, or kUnset for "say nothing".
    // Meaningless while a loop is running — the board owns the duty then.
    double at(double t) const;

    void overlay(const FanPlan& o);
};

struct AutomationSettings {
    AccelSettings accel[kAccelCount];
    FanPlan fan;
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
