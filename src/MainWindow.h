// Top-level window: benchmark controls on the left, a stack of scrolling
// time-series graphs on the right.
//
// The three benchmark graphs (frame rate, efficiency, energy) carry one series
// per accelerator, always — the same series in the same colors as the Power and
// Temperature graphs below them. A card that isn't running contributes 0, which
// is both true and what keeps a trace's color meaning one card for the whole
// session.
#pragma once

#include <gdkmm/rgba.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/expander.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Bench.h"
#include "Automation.h"
#include "Logger.h"
#include "Catalog.h"
#include "ControlPanel.h"
#include "Fetcher.h"
#include "GraphArea.h"
#include "Probes.h"

class MainWindow : public Gtk::ApplicationWindow {
public:
    // `plan` is disabled unless --automation named a file.
    explicit MainWindow(AutomationPlan plan = {});
    ~MainWindow() override;

private:
    bool on_tick();
    // Poll the downloader and refresh the lists as artifacts land.
    void poll_downloads();
    void on_start_stop();

    // One state-machine step for unattended automation, called once per tick.
    // Deliberately driven from the existing 1 Hz tick rather than its own
    // timer: it has to observe exactly the engine state the UI is showing, and
    // a second timer could fire in the window between stop() and the workers
    // actually being joined.
    void tick_automation();
    void advance_automation_step();
    void tick_automation_end();
    void on_about();

    // --- telemetry sections (one row per device, as in mb-powermon-gui) ---

    // A per-device aggregate label spanning metrics [start, start+count) in the
    // aligned value vector: mean for temperature, max for power.
    // One device's legend row on a telemetry graph: its name label, its
    // aggregate label and its per-metric cells, so the Graphs filter can hide
    // the whole row. Keyed by device_name because that is what a metric carries
    // and what a folded INA228 inherits.
    struct LegendRow {
        std::string device;
        std::vector<Gtk::Widget*> widgets;
    };

    struct AggEntry {
        Gtk::Label* label;
        int start;
        int count;
    };

    Gtk::Widget& build_metric_section(const std::vector<MetricInfo>& metrics,
                                      const std::vector<Gdk::RGBA>& colors,
                                      bool fixed_temp_axis,
                                      std::function<std::string(double)> value_fmt,
                                      GraphArea*& graph_out,
                                      std::vector<Gtk::Label*>& value_labels_out,
                                      const char* empty_note,
                                      std::vector<AggEntry>* agg_out = nullptr,
                                      std::vector<LegendRow>* rows_out = nullptr,
                                      double min_axis_max = 10.0);

    // --- benchmark sections (one series per accelerator) ---

    struct AccelSection {
        Gtk::Widget* root = nullptr;  // graph + legend, to hand to make_section
        GraphArea* graph = nullptr;
        Gtk::Label* value[kAccelCount] = {};
        // The legend cell (swatch + name + value) for each card, so the Graphs
        // filter can hide the entry along with the trace.
        Gtk::Widget* cell[kAccelCount] = {};
    };

    // Push the control panel's Range choice onto every graph.
    void apply_range_mode();
    // Show the cards ticked in Graphs / Accelerators by hiding the rest on
    // every graph. Retroactive, and it re-scales the axes.
    void apply_graph_filter();

    AccelSection build_accel_section(std::function<std::string(double)> value_fmt,
                                     double min_axis_max);

    std::vector<Gdk::RGBA> colors_for(const std::vector<MetricInfo>& metrics) const;
    Gtk::Expander& make_section(const char* title, Gtk::Widget& content,
                                bool expanded = true);

    Probes probes_;
    Catalog catalog_;
    Fetcher fetcher_;
    BenchEngine engine_;
    ControlPanel* controls_ = nullptr;

    std::vector<Gdk::RGBA> device_palette_;
    Gdk::RGBA accel_color_[kAccelCount];

    sigc::connection tick_conn_;  // disconnected before teardown
    // Base text of the running-benchmark status line; per-tick failures are
    // appended to it, since the frame-rate legend no longer carries them.
    std::string run_status_;
    // CSV log, always on. Opened after probes_.discover() (the header needs the
    // metric lists) and closed in the destructor.
    Logger log_;
    // The items the current run actually started with. BenchEngine does not
    // expose BenchItem after start() — Worker is private and incomplete, and
    // Series carries no config — so the configuration has to be stashed here to
    // be loggable. Cleared on stop.
    std::vector<BenchItem> run_items_;

    // Automation state. `auto_phase_s_` counts seconds inside the current phase
    // (running or idling); `auto_owns_run_` distinguishes a run automation
    // started from one the user started by hand, which it must never cut short.
    AutomationPlan auto_plan_;
    std::size_t auto_step_ = 0;      // index into auto_plan_.steps
    bool auto_first_ = true;         // nothing has been started yet
    bool auto_done_ = false;         // a non-looping plan finished
    double auto_phase_s_ = 0.0;
    bool auto_owns_run_ = false;
    // Seconds spent waiting for a stop to complete. A worker wedged inside a
    // vendor SDK call may never return, and automation cannot start anything
    // while it still holds the device — so this exists purely to notice and
    // *say so* rather than idle forever looking like it is still working.
    double auto_stopping_s_ = 0.0;
    bool auto_stalled_ = false;
    // Seconds counted since the plan finished, for the `end` delay before the
    // app closes itself.
    double auto_end_s_ = 0.0;
    // Set when automation has asked to close. on_tick() bails out immediately
    // on seeing it — see tick_automation_end() for why the close cannot happen
    // inline.
    bool closing_ = false;
    // Last error string seen per card, so a persistent failure is logged once
    // rather than every tick.
    std::string last_error_[kAccelCount];
    std::string last_bench_state_;
    // How many Fetcher errors have been logged, so each is written once.
    std::size_t logged_fetch_errors_ = 0;
    std::int64_t last_time_us_ = 0;
    Gtk::AboutDialog about_dialog_;
    bool about_ready_ = false;

    AccelSection fps_, eff_, energy_;

    GraphArea* power_graph_ = nullptr;
    std::vector<Gtk::Label*> power_values_;
    std::vector<AggEntry> power_max_labels_;
    std::vector<LegendRow> power_rows_;

    GraphArea* temp_graph_ = nullptr;
    std::vector<Gtk::Label*> temp_values_;
    std::vector<AggEntry> temp_avg_labels_;
    std::vector<LegendRow> temp_rows_;

    GraphArea* freq_graph_ = nullptr;
    std::vector<Gtk::Label*> freq_values_;
    std::vector<AggEntry> freq_avg_labels_;
    std::vector<LegendRow> freq_rows_;
};
