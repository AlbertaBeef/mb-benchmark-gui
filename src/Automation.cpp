#include "Automation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Split on `sep`, trimming each field. Empty fields are kept: "a,,b" has three,
// and a blank per-step duration deliberately means "keep the default".
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = s.find(sep, start);
        out.push_back(trim(s.substr(start, p == std::string::npos ? p : p - start)));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

// Durations are written in minutes — the interesting values are tens of
// minutes, and a plan full of "1800" would read badly. Fractions are allowed so
// a smoke test can say 0.5 without the file needing a second unit.
bool parse_minutes(const std::string& v, double& out) {
    const std::string t = trim(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const double m = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || m < 0) return false;
    out = m * 60.0;
    return true;
}

// A bare number, for the fan's percentages. Signed: fan.step may ramp down.
bool parse_num(const std::string& v, double& out) {
    const std::string t = trim(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return false;
    out = d;
    return true;
}

bool parse_int(const std::string& v, int& out) {
    const std::string t = trim(v);
    if (t.empty()) return false;
    char* end = nullptr;
    const long n = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') return false;
    out = static_cast<int>(n);
    return true;
}

bool parse_bool(const std::string& v, bool& out) {
    const std::string t = lower(trim(v));
    if (t == "1" || t == "true" || t == "yes" || t == "on") { out = true; return true; }
    if (t == "0" || t == "false" || t == "no" || t == "off") { out = false; return true; }
    return false;
}

// "hailo" → Accel, using the same vendor strings the CSV columns use.
bool vendor_to_accel(const std::string& name, Accel& out) {
    for (int i = 0; i < kAccelCount; ++i) {
        const Accel a = accel_at(i);
        if (lower(accel_vendor(a)) == lower(name)) { out = a; return true; }
    }
    return false;
}

// One `<vendor>.<knob> = value` assignment.
bool apply_accel_key(AutomationSettings& s, const std::string& vendor,
                     const std::string& knob, const std::string& val,
                     std::string& why) {
    Accel a{};
    if (!vendor_to_accel(vendor, a)) {
        why = "unknown accelerator '" + vendor + "'";
        return false;
    }
    AccelSettings& t = s.accel[accel_index(a)];
    const std::string k = lower(knob);
    const std::string v = lower(trim(val));

    if (k == "enabled") {
        bool b{};
        if (!parse_bool(v, b)) { why = "enabled must be true or false"; return false; }
        t.enabled = b ? 1 : 0;
        return true;
    }
    if (k == "api") {
        if (v == "sync") { t.api = 0; return true; }
        if (v == "async") { t.api = 1; return true; }
        why = "api must be sync or async";
        return false;
    }
    if (k == "depth")  return parse_int(v, t.depth)    ? true : (why = "depth must be a number", false);
    // Axelera's control is double buffering, not a depth — see ControlPanel.
    // It still travels as `depth` (1 off / 2 on) so nothing downstream changes,
    // and `<vendor>.depth` keeps working for plans already written against it.
    if (k == "double_buffer" || k == "dbuf") {
        bool b{};
        if (!parse_bool(v, b)) { why = "double_buffer must be true or false"; return false; }
        t.depth = b ? 2 : 1;
        return true;
    }
    if (k == "cores")  return parse_int(v, t.cores)    ? true : (why = "cores must be a number", false);
    if (k == "freq")   return parse_int(v, t.freq_mhz) ? true : (why = "freq must be a number (MHz)", false);
    if (k == "nsps")   return parse_int(v, t.nsps)     ? true : (why = "nsps must be a number", false);
    if (k == "perf")    { t.perf = v; return true; }
    if (k == "backend") {
        if (v != "htp" && v != "gpu" && v != "cpu") {
            why = "backend must be htp, gpu or cpu";
            return false;
        }
        t.backend = v;
        return true;
    }
    why = "unknown setting '" + knob + "' for " + vendor;
    return false;
}

// `fan` and `fan.<knob>`. Kept apart from apply_accel_key() because the fan is
// not a card: "fan" has no dot at all, and "fan.start" has one whose left half
// is not a vendor, so both would fall through to "unknown accelerator".
bool apply_fan_key(FanPlan& f, const std::string& knob, const std::string& val,
                   std::string& why) {
    const std::string k = lower(knob);
    // Bare `fan = N` is the duty to hold.
    if (k.empty() || k == "duty") {
        if (!parse_num(val, f.duty) || f.duty < 0 || f.duty > 100) {
            why = "fan must be a percentage 0..100, got '" + val + "'";
            return false;
        }
        return true;
    }
    if (k == "start" || k == "max") {
        double* dst = (k == "start") ? &f.start : &f.max;
        if (!parse_num(val, *dst) || *dst < 0 || *dst > 100) {
            why = "fan." + k + " must be a percentage 0..100, got '" + val + "'";
            return false;
        }
        return true;
    }
    if (k == "step") {
        if (!parse_num(val, f.step) || f.step < -100 || f.step > 100) {
            why = "fan.step must be a percentage -100..100, got '" + val + "'";
            return false;
        }
        return true;
    }
    // Intervals are minutes in the file like every other duration, so a plan
    // never mixes units — "fan.every = 1" is once a minute.
    if (k == "every" || k == "after") {
        double* dst = (k == "every") ? &f.every : &f.after;
        if (!parse_minutes(val, *dst)) {
            why = "fan." + k + " must be minutes, got '" + val + "'";
            return false;
        }
        if (k == "every" && *dst <= 0) {
            why = "fan.every must be greater than zero";
            return false;
        }
        return true;
    }
    why = "unknown fan setting '" + knob + "'";
    return false;
}

// "accelerators = hailo deepx" — the whitelist for a run. Space-separated,
// because the surrounding per-step syntax already uses commas.
bool apply_accel_list(AutomationSettings& s, const std::string& val,
                      std::string& why) {
    for (int i = 0; i < kAccelCount; ++i) s.accel[i].enabled = 0;
    const std::string t = lower(trim(val));
    if (t == "all") {
        for (int i = 0; i < kAccelCount; ++i) s.accel[i].enabled = 1;
        return true;
    }
    if (t == "none") return true;
    std::string tok;
    bool any = false;
    auto flush = [&]() -> bool {
        if (tok.empty()) return true;
        Accel a{};
        if (!vendor_to_accel(tok, a)) { why = "unknown accelerator '" + tok + "'"; return false; }
        s.accel[accel_index(a)].enabled = 1;
        any = true;
        tok.clear();
        return true;
    };
    for (char c : t) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!flush()) return false;
        } else {
            tok += c;
        }
    }
    if (!flush()) return false;
    if (!any) { why = "accelerators list is empty"; return false; }
    return true;
}

// One `key = value` that may be a plain knob or a `<vendor>.<knob>` pair.
// Shared by the [automation] defaults and the per-step option list, so the two
// can never drift into accepting different spellings.
bool apply_setting(AutomationSettings& s, const std::string& key,
                   const std::string& val, std::string& why) {
    if (lower(key) == "accelerators") return apply_accel_list(s, val, why);
    const std::size_t dot = key.find('.');
    if (dot == std::string::npos) {
        if (lower(key) == "fan") return apply_fan_key(s.fan, "", val, why);
        why = "unknown key '" + key + "'";
        return false;
    }
    if (lower(key.substr(0, dot)) == "fan")
        return apply_fan_key(s.fan, key.substr(dot + 1), val, why);
    return apply_accel_key(s, key.substr(0, dot), key.substr(dot + 1), val, why);
}

}  // namespace

void AccelSettings::overlay(const AccelSettings& o) {
    if (o.enabled  != kUnset) enabled  = o.enabled;
    if (o.api      != kUnset) api      = o.api;
    if (o.depth    != kUnset) depth    = o.depth;
    if (o.cores    != kUnset) cores    = o.cores;
    if (o.freq_mhz != kUnset) freq_mhz = o.freq_mhz;
    if (o.nsps     != kUnset) nsps     = o.nsps;
    if (!o.perf.empty())      perf     = o.perf;
    if (!o.backend.empty())   backend  = o.backend;
}

void FanPlan::overlay(const FanPlan& o) {
    if (o.duty  != kUnset) duty  = o.duty;
    if (o.start != kUnset) start = o.start;
    if (o.step  != kUnset) step  = o.step;
    if (o.every != kUnset) every = o.every;
    if (o.after != kUnset) after = o.after;
    if (o.max   != kUnset) max   = o.max;
}

// The ramp is computed from `t` rather than accumulated, so it cannot drift and
// a missed tick cannot lose a step: at any moment the answer depends only on
// how far into the run we are.
double FanPlan::at(double t) const {
    if (!ramps()) return duty;              // constant, or kUnset = say nothing
    const double a = (after == kUnset) ? 0.0 : after;
    // Before the ramp begins the base duty holds — that is the "fan off until
    // it settles" half of a thermal plan.
    if (t < a) return duty;
    double d = start + std::floor((t - a) / every) * step;
    // `max` is where the ramp stops: a ceiling going up, a floor coming down.
    if (max != kUnset) d = (step < 0) ? std::max(d, max) : std::min(d, max);
    return std::min(100.0, std::max(0.0, d));
}

void AutomationSettings::overlay(const AutomationSettings& o) {
    for (int i = 0; i < kAccelCount; ++i) accel[i].overlay(o.accel[i]);
    fan.overlay(o.fan);
}

double AutomationPlan::bench_for(std::size_t i) const {
    if (i < steps.size() && steps[i].bench_seconds >= 0.0)
        return steps[i].bench_seconds;
    return bench_seconds;
}

double AutomationPlan::idle_for(std::size_t i) const {
    if (i < steps.size() && steps[i].idle_seconds >= 0.0)
        return steps[i].idle_seconds;
    return idle_seconds;
}

AutomationSettings AutomationPlan::settings_for(std::size_t i) const {
    AutomationSettings s = defaults;
    if (i < steps.size()) s.overlay(steps[i].settings);
    return s;
}

// A deliberately small line-oriented reader, in the same spirit as Catalog's:
// no INI library, no glib, so this stays headlessly testable. The step list is
// *ordered* and INI sections are not, so steps are repeated keys rather than
// sections; the [headers] are decorative and ignored.
bool load_automation_plan(const std::string& path, AutomationPlan& out,
                          std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }

    out = AutomationPlan{};
    out.path = path;

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        const std::string at = path + ":" + std::to_string(lineno) + ": ";
        // '#' starts a comment anywhere.
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty() || line.front() == '[') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = at + "expected key = value";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        const std::string k = lower(key);

        if (k == "start" || k == "end" || k == "benchmark" || k == "idle") {
            double* dst = k == "start"     ? &out.start_seconds
                        : k == "end"       ? &out.end_seconds
                        : k == "benchmark" ? &out.bench_seconds
                                           : &out.idle_seconds;
            if (!parse_minutes(val, *dst)) {
                error = at + key + " must be minutes, got '" + val + "'";
                return false;
            }
            continue;
        }
        if (k == "loop") {
            if (!parse_bool(val, out.loop)) {
                error = at + "loop must be true or false, got '" + val + "'";
                return false;
            }
            continue;
        }
        if (k == "model" || k == "pipeline") {
            // "id" then any number of "name=value" options, comma separated.
            // Named rather than positional: with a dozen possible knobs, a
            // positional list would be unreadable and unextendable.
            AutomationStep st;
            st.is_pipeline = (k == "pipeline");
            const auto parts = split(val, ',');
            st.id = parts.empty() ? std::string() : parts[0];
            if (st.id.empty()) {
                error = at + "empty " + key + " id";
                return false;
            }
            for (std::size_t p = 1; p < parts.size(); ++p) {
                const std::string& opt = parts[p];
                if (opt.empty()) continue;
                const std::size_t oe = opt.find('=');
                if (oe == std::string::npos) {
                    error = at + "step option '" + opt + "' must be name=value";
                    return false;
                }
                const std::string ok_ = trim(opt.substr(0, oe));
                const std::string ov = trim(opt.substr(oe + 1));
                const std::string okl = lower(ok_);
                if (okl == "benchmark" || okl == "idle") {
                    double* dst = okl == "benchmark" ? &st.bench_seconds
                                                     : &st.idle_seconds;
                    if (!parse_minutes(ov, *dst)) {
                        error = at + ok_ + " must be minutes, got '" + ov + "'";
                        return false;
                    }
                    continue;
                }
                std::string why;
                if (!apply_setting(st.settings, ok_, ov, why)) {
                    error = at + why;
                    return false;
                }
            }
            out.steps.push_back(std::move(st));
            continue;
        }

        // Anything else is an accelerator default for the whole plan.
        std::string why;
        if (!apply_setting(out.defaults, key, val, why)) {
            error = at + why;
            return false;
        }
    }

    if (out.steps.empty()) {
        error = path + ": no model or pipeline steps — nothing to run";
        return false;
    }
    out.enabled = true;
    return true;
}
