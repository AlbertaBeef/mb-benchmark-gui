// Passive telemetry collection for the edge-AI NPUs on this host.
//
// "Passive" = never claims a device or perturbs another application's use of
// it: DeepX via `dxrt-cli -s` (reads the kernel driver), MemryX via sysfs/hwmon,
// Axelera via `triton_trace --peek` (reads the collector log, no Context claim),
// Qualcomm IQ via the `nsp-*-thermal` sysfs zones.
// No GTK dependency — pure data, mirroring the Python mb-powermon probes.
#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

// One trace / legend row. `label` is device-qualified, e.g. "DeepX T0".
// `device` is the owning device's global index (assigned at discovery), so the
// UI can color every metric from the same device identically.
struct MetricInfo {
    std::string label;
    std::string unit;         // "°C" or "W"
    int device = -1;          // filled during discovery
    std::string device_name;  // "Hailo", "DeepX", ...
    // PCIe BDF, e.g. "0000:01:00.0". For an SoC-integrated NPU there is no BDF,
    // so this carries an equivalent locator instead (e.g. the SoC id "QCS9075").
    std::string bdf;
    // Name of another device whose color this one should share (empty = own
    // color). Set for a mapped INA228 so it reuses its accelerator's swatch.
    std::string color_alias;
};

// The RA6M5 probe board's on-board thermal control loop (firmware >= 0.5.0).
//
// The loop runs on the BOARD, not here: it holds the thermocouple, the card's
// rail watts and board air, so it keeps cooling the card when this process
// crashes, exits or loses the port. The host's whole job is to configure it and
// read back what it is doing.
//
// `Die` regulates an ESTIMATE, `heatsink + R_int x watts`, because the real die
// temperature only exists on this side of PCIe. For the Hailo-8 R_int was
// measured at 1.709 C/W; see docs/reports/ra6m5-thermocouple/.
// The board's loop has ONE mode since firmware 0.6.0. `FanInput` is gone with
// it: the setpoint is always the estimated on-die temperature, because
// "regulate the die" and "regulate the heatsink" were the same control law with
// the observer moved between the setpoint and the error. See Control.h.
enum class FanMode : int { Off = 0, On = 1 };

// Where the board's regulated signal came from. This is `ctl.src`, which before
// 0.6.0 reported die-vs-heatsink. A reader of the log cannot afford to guess
// whether the loop was closed on a measurement or on an estimate, so it now
// reports that instead.
enum class FanSrc : int { Measured = 0, Modelled = 1, None = 2 };

// What the host ASKED the loop to use, distinct from FanSrc above, which is
// what it managed. `Real` degrades to `Model` and then to open loop; `Model`
// does not fall back to the probe, because asking for the estimate while one is
// fitted is exactly what the experiment does.
enum class FanTc : int { Off = 0, Real = 1, Model = 2 };

struct FanControlStatus {
    bool present = false;        // a board with a loop answered
    FanMode mode = FanMode::Off;
    FanSrc src = FanSrc::None;
    int state = 0;               // see state_name()
    FanTc tc = FanTc::Real;      // what was ASKED for; `src` is what was got
    // Whether a model EXISTS, which `src` cannot say -- it reads `None` both
    // with no model and with a request of `Off`. Firmware >= 0.7.0; false on an
    // older board, which then looks the same as "no model".
    bool model_ready = false;
    bool target_auto = true;     // was the setpoint defaulted or chosen
    double target = 0.0 / 0.0, in = 0.0 / 0.0, die_est = 0.0 / 0.0;
    double err = 0.0 / 0.0, duty = 0.0 / 0.0;
    const char* state_name() const {
        switch (state) {
            case 1: return "settling";
            case 2: return "holding";
            // 3 and 4 split what was one "saturated" state, which was set from
            // "the output is clamped" — true at both rails and meaning the
            // opposite thing at each.
            case 3: return "at the floor";
            case 4: return "at the ceiling";
            case 5: return "open loop";
            case 6: return "degraded";
            case 7: return "fault";
            default: return "off";
        }
    }
    // NAMES THAT CANNOT BE CONFUSED WITH EACH OTHER. `None` used to print
    // "model only", meaning "the static feedforward model and nothing else" --
    // which was clear until `Modelled` existed and made it read as the exact
    // opposite, i.e. that the soft sensor HAD been used. Renaming a value is
    // not finished until every other value's name is re-read against it.
    const char* src_name() const {
        switch (src) {
            case FanSrc::Measured: return "thermocouple";
            case FanSrc::Modelled: return "model estimate";
            default: return "open loop, no temperature";
        }
    }
};

// A single device's telemetry source.
class DeviceProbe {
public:
    virtual ~DeviceProbe() = default;
    virtual const char* name() const = 0;
    const std::string& bdf() const { return bdf_; }
    // Optional human-readable status when a device is present but not returning
    // data (e.g. firmware/runtime version mismatch, idle collector). Empty when
    // the device is nominal or genuinely absent.
    const std::string& note() const { return note_; }
    // Name of another device whose color to share (empty = own color).
    const std::string& color_alias() const { return color_alias_; }

    // Refresh readings. Fills temp_values()/power_values(), aligned to the
    // corresponding *_metrics() lists; a missing reading is NaN.
    virtual void poll() = 0;

    // Fan duty, percent. Only a probe that drives a fan overrides this (the
    // RA6M5 board, through its EMC2101); everything else has nothing to set.
    //
    // The firmware REFUSES this while its control loop is running, so a caller
    // that gets false back while a mode is active is being told the truth
    // rather than failing: two writers on one register is the bug this avoids.
    virtual bool set_fan_duty(double /*pct*/) { return false; }

    // Configure the board's control loop. One call rather than one per setting,
    // so they are always applied together and in an order that works — the
    // sources and the setpoint first, `mode` LAST, so the loop never runs a
    // step with a stale setting. (Before 0.6.0 mode went first, because
    // selecting Smart installed its own target; with one mode there is nothing
    // for that ordering to protect.)
    //
    // `target` is NaN for "auto": let the board use its own measured per-card
    // bound rather than naming a number here.
    virtual bool set_fan_control(FanMode /*m*/, FanTc /*tc*/,
                                 double /*target*/) {
        return false;
    }
    virtual FanControlStatus fan_control_status() const { return {}; }

    const std::vector<MetricInfo>& temp_metrics() const { return temp_metrics_; }
    const std::vector<double>& temp_values() const { return temp_values_; }
    const std::vector<MetricInfo>& power_metrics() const { return power_metrics_; }
    const std::vector<double>& power_values() const { return power_values_; }
    // Core clock, MHz. Only some cards expose one — see Probes.cpp.
    const std::vector<MetricInfo>& freq_metrics() const { return freq_metrics_; }
    const std::vector<double>& freq_values() const { return freq_values_; }
    // Accumulated energy (J) and charge (C) since discovery. Only the INA228
    // shunts have these — they are hardware accumulators integrating at the ADC
    // rate, not something derived from the 1 Hz samples.
    //
    // Energy and charge are separate families rather than one, because a
    // GraphArea carries a single unit and formatter: joules and coulombs cannot
    // share a plot. Energy is graphed; charge is CSV-only, which is why it has a
    // family but no section in MainWindow.
    //
    // Energy is emphatically NOT in the power family: power_for_device() returns
    // the max over power metrics and feeds the engine its watts, so joules
    // parked there would overtake watts within seconds and silently corrupt
    // every fps/W and mJ/frame figure in the app.
    const std::vector<MetricInfo>& energy_metrics() const { return energy_metrics_; }
    const std::vector<double>& energy_values() const { return energy_values_; }
    const std::vector<MetricInfo>& charge_metrics() const { return charge_metrics_; }
    const std::vector<double>& charge_values() const { return charge_values_; }
    // Bus voltage (V) at the shunt. Only the INA228s have it. Its own family
    // because a GraphArea carries one unit, and because it must never join
    // power_: power_for_device() returns the max over that family and feeds the
    // engine its watts, so a 3.3 would quietly become "3.3 W" on a card whose
    // real draw is lower, corrupting every fps/W figure.
    const std::vector<MetricInfo>& voltage_metrics() const { return voltage_metrics_; }
    const std::vector<double>& voltage_values() const { return voltage_values_; }
    // Current (A) through the shunt. Its own family for the same reason as
    // voltage: one unit per graph, and it must never join power_.
    const std::vector<MetricInfo>& current_metrics() const { return current_metrics_; }
    const std::vector<double>& current_values() const { return current_values_; }
    // Whole-system power (W) from an inline supply meter — the POWER-Z on the
    // IQ-9075's USB-C input. Its own family, separate from power_, for two
    // reasons. It is a different *quantity*: board total (CPU + GPU + NSP +
    // DRAM + peripherals) rather than one card's draw, so averaging it with
    // per-card watts is meaningless. And it is an order of magnitude larger —
    // 22 W against a Hailo's 0.85 — so sharing power_'s axis would flatten
    // every card trace into the bottom pixel row.
    //
    // Keeping it out of power_ also keeps it out of power_for_device(), which
    // returns the max over that family and feeds the engine its watts: board
    // power landing there would inflate every fps/W and mJ/frame by ~10x.
    const std::vector<MetricInfo>& syspower_metrics() const { return syspower_metrics_; }
    const std::vector<double>& syspower_values() const { return syspower_values_; }
    // The supply meter's own bus voltage and current, kept out of voltage_ and
    // current_ for the same reason syspower_ is kept out of power_: those two
    // families are the *accelerator* rails, and a 19.9 V board input sharing an
    // axis with a 3.3 V card rail would flatten the sag that graph exists to
    // show. Same quantity, different subject.
    const std::vector<MetricInfo>& sysvoltage_metrics() const { return sysvoltage_metrics_; }
    const std::vector<double>& sysvoltage_values() const { return sysvoltage_values_; }
    const std::vector<MetricInfo>& syscurrent_metrics() const { return syscurrent_metrics_; }
    const std::vector<double>& syscurrent_values() const { return syscurrent_values_; }
    // Air velocity (m/s), from an FS3000 on an RA6M5 probe board. Its own family
    // because it is its own unit; and it belongs to no card — the airflow over
    // a heatsink is not a rail — so the RA6M5's board probe carries it rather
    // than the INA228 probe that folds onto a card.
    const std::vector<MetricInfo>& airflow_metrics() const { return airflow_metrics_; }
    const std::vector<double>& airflow_values() const { return airflow_values_; }
    // The fan an RA6M5 board drives: speed (rpm) and duty (%). Its own family
    // because neither unit is airflow's; the Airflow graph shows them anyway
    // (MainWindow scales them onto its axis), but the values here and in the
    // CSV are the real ones.
    const std::vector<MetricInfo>& fan_metrics() const { return fan_metrics_; }
    const std::vector<double>& fan_values() const { return fan_values_; }
    // The RA6M5's on-board thermal control loop: its mode, its state, the
    // setpoint and what it is doing about it.
    //
    // CSV-ONLY, deliberately — this family has no graph section, exactly like
    // charge_. Two of its members are enumerations and the rest carry three
    // different units, and a GraphArea carries one unit and one formatter. The
    // place these belong is the log, beside the card's real die temperature,
    // where the board's estimate can be scored against the truth after a run.
    const std::vector<MetricInfo>& ctl_metrics() const { return ctl_metrics_; }
    const std::vector<double>& ctl_values() const { return ctl_values_; }

protected:
    std::vector<MetricInfo> temp_metrics_;
    std::vector<double> temp_values_;
    std::vector<MetricInfo> power_metrics_;
    std::vector<double> power_values_;
    std::vector<MetricInfo> freq_metrics_;
    std::vector<double> freq_values_;
    std::vector<MetricInfo> energy_metrics_;
    std::vector<double> energy_values_;
    std::vector<MetricInfo> charge_metrics_;
    std::vector<double> charge_values_;
    std::vector<MetricInfo> voltage_metrics_;
    std::vector<double> voltage_values_;
    std::vector<MetricInfo> current_metrics_;
    std::vector<double> current_values_;
    std::vector<MetricInfo> syspower_metrics_;
    std::vector<double> syspower_values_;
    std::vector<MetricInfo> sysvoltage_metrics_;
    std::vector<double> sysvoltage_values_;
    std::vector<MetricInfo> syscurrent_metrics_;
    std::vector<double> syscurrent_values_;
    std::vector<MetricInfo> airflow_metrics_;
    std::vector<double> airflow_values_;
    std::vector<MetricInfo> fan_metrics_;
    std::vector<double> fan_values_;
    std::vector<MetricInfo> ctl_metrics_;
    std::vector<double> ctl_values_;
    std::string bdf_;
    std::string note_;
    std::string color_alias_;
};

// Discovers every supported device and presents their metrics as two flat,
// stable lists (temperature and power). The lists are fixed after discover();
// poll() only refreshes the aligned value vectors.
class Probes {
public:
    // Detect devices and build the metric lists. `notes` (optional) collects
    // human-readable diagnostics about what was / wasn't found.
    void discover(std::vector<std::string>* notes = nullptr);

    // Optional sink for notes raised *after* discovery — a probe's `note_` can
    // change while running (the Axelera collector being enabled, a card falling
    // back to bootloader). Called from poll() only when the text actually
    // changes, so it fires once per transition rather than every second.
    //
    // A plain std::function keeps Probes GTK-free and free of any dependency on
    // the logger; MainWindow binds it to both g_message and Logger::note.
    void set_note_sink(std::function<void(const std::string&)> sink) {
        note_sink_ = std::move(sink);
    }
    // "A benchmark is running" — set by the owner from its own tick.
    //
    // Probes are otherwise passive readers, but ONE of them writes device
    // state: AxeleraProbe raises the Metis log level so temperatures appear.
    // Its trigger is "firmware is present but no core_temps", which fires the
    // instant a benchmark's axr_device_connect() finishes uploading firmware —
    // the single worst moment to open a second handle on that card. Measured
    // 2026-09-20: the write landed one second after the upload, the kernel
    // logged two firmware reloads inside that second, the PCIe link dropped
    // and the card fell back to bootloader.
    //
    // Defaults to false, so a host that never sets it (mb-powermon-gui has no
    // engine) behaves exactly as before.
    void set_busy(bool busy);
    void poll();

    const std::vector<MetricInfo>& temp_metrics() const { return temp_metrics_; }
    const std::vector<double>& temp_values() const { return temp_values_; }
    const std::vector<MetricInfo>& power_metrics() const { return power_metrics_; }
    const std::vector<double>& power_values() const { return power_values_; }
    // Core clock (MHz). Follows discovery order, like temperature. Empty for
    // cards whose SDK exposes no clock — today Hailo and Axelera.
    const std::vector<MetricInfo>& freq_metrics() const { return freq_metrics_; }
    const std::vector<double>& freq_values() const { return freq_values_; }
    // Accumulated energy (J) / charge (C) from the INA228 shunts. Both follow
    // the alias ordering, like power, so a folded reading lands adjacent to its
    // card's other metrics. Empty on a host with no shunts.
    const std::vector<MetricInfo>& energy_metrics() const { return energy_metrics_; }
    const std::vector<double>& energy_values() const { return energy_values_; }
    const std::vector<MetricInfo>& charge_metrics() const { return charge_metrics_; }
    const std::vector<double>& charge_values() const { return charge_values_; }
    // Bus voltage (V), alias-ordered like power. Empty on a host with no shunts.
    const std::vector<MetricInfo>& voltage_metrics() const { return voltage_metrics_; }
    const std::vector<double>& voltage_values() const { return voltage_values_; }
    // Current (A), alias-ordered like power. Empty with no shunts.
    const std::vector<MetricInfo>& current_metrics() const { return current_metrics_; }
    const std::vector<double>& current_values() const { return current_values_; }
    // Whole-system power (W) from an inline supply meter; see DeviceProbe for
    // why this is not part of the power family.
    const std::vector<MetricInfo>& syspower_metrics() const { return syspower_metrics_; }
    const std::vector<double>& syspower_values() const { return syspower_values_; }
    // Supply-meter voltage and current; see DeviceProbe.
    const std::vector<MetricInfo>& sysvoltage_metrics() const { return sysvoltage_metrics_; }
    const std::vector<double>& sysvoltage_values() const { return sysvoltage_values_; }
    const std::vector<MetricInfo>& syscurrent_metrics() const { return syscurrent_metrics_; }
    const std::vector<double>& syscurrent_values() const { return syscurrent_values_; }
    // Air velocity (m/s) from RA6M5 probe boards. Empty without one.
    const std::vector<MetricInfo>& airflow_metrics() const { return airflow_metrics_; }
    const std::vector<double>& airflow_values() const { return airflow_values_; }
    // Fan speed (rpm) and duty (%) from RA6M5 boards; see DeviceProbe.
    const std::vector<MetricInfo>& fan_metrics() const { return fan_metrics_; }
    const std::vector<double>& fan_values() const { return fan_values_; }
    // The on-board control loop's status. CSV-only — no graph section, like
    // charge_; see DeviceProbe for why.
    const std::vector<MetricInfo>& ctl_metrics() const { return ctl_metrics_; }
    const std::vector<double>& ctl_values() const { return ctl_values_; }
    // Whether any probe can drive a fan, and the command to do it. Applies to
    // every such probe; returns true if at least one accepted it.
    bool has_fan_control() const;
    bool set_fan_duty(double pct);
    // Whether any board runs a control loop, how to configure it, and what it
    // is doing. `fan_control_status()` returns the first board that answers.
    bool has_fan_loop() const;
    bool set_fan_control(FanMode m, FanTc tc, double target);
    FanControlStatus fan_control_status() const;

    int device_count() const { return static_cast<int>(devices_.size()); }

    // Max power across every metric belonging to the named device ("Hailo",
    // "DeepX", ...), matching the legend's per-device "max"; NaN if that device
    // reports no power. This is what pairs a benchmark's FPS with the watts of
    // the card that produced it, for the FPS/W and Frames/J graphs.
    double power_for_device(const std::string& device_name) const;

private:
    void flatten();
    // Emission order that keeps a mapped INA228 immediately before the
    // accelerator it names, so a folded reading joins that card's contiguous
    // run (the legend groups by runs of MetricInfo::device — see Probes.cpp).
    // Shared by every folded family, which is what lines the INA228 cells up in
    // the same legend column across graphs.
    std::vector<size_t> alias_device_order() const;
    // Accelerator index a PCIe-mapped INA228 should fold its reading onto, or -1.
    int pcie_merge_target(size_t k) const;

    std::vector<std::unique_ptr<DeviceProbe>> devices_;
    std::vector<MetricInfo> temp_metrics_, power_metrics_, freq_metrics_;
    std::vector<double> temp_values_, power_values_, freq_values_;
    std::vector<MetricInfo> syspower_metrics_, sysvoltage_metrics_,
        syscurrent_metrics_;
    std::vector<double> syspower_values_, sysvoltage_values_, syscurrent_values_;
    std::vector<MetricInfo> energy_metrics_, charge_metrics_, voltage_metrics_,
        current_metrics_;
    std::vector<double> energy_values_, charge_values_, voltage_values_,
        current_values_;
    std::vector<MetricInfo> airflow_metrics_;
    std::vector<double> airflow_values_;
    std::vector<MetricInfo> fan_metrics_;
    std::vector<double> fan_values_;
    std::vector<MetricInfo> ctl_metrics_;
    std::vector<double> ctl_values_;
    // devices_ indices, emission order for every folded family.
    std::vector<size_t> dev_order_;
    std::function<void(const std::string&)> note_sink_;
    std::vector<std::string> last_notes_;   // per device, to detect transitions
};
