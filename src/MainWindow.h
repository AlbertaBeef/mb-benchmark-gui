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
#include "Catalog.h"
#include "ControlPanel.h"
#include "Fetcher.h"
#include "GraphArea.h"
#include "Probes.h"

class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow();
    ~MainWindow() override;

private:
    bool on_tick();
    // Poll the downloader and refresh the lists as artifacts land.
    void poll_downloads();
    void on_start_stop();
    void on_about();

    // --- telemetry sections (one row per device, as in mb-powermon-gui) ---

    // A per-device aggregate label spanning metrics [start, start+count) in the
    // aligned value vector: mean for temperature, max for power.
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
                                      std::vector<AggEntry>* agg_out = nullptr);

    // --- benchmark sections (one series per accelerator) ---

    struct AccelSection {
        Gtk::Widget* root = nullptr;  // graph + legend, to hand to make_section
        GraphArea* graph = nullptr;
        Gtk::Label* value[kAccelCount] = {};
    };

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
    std::int64_t last_time_us_ = 0;
    Gtk::AboutDialog about_dialog_;
    bool about_ready_ = false;

    AccelSection fps_, eff_, energy_;

    GraphArea* power_graph_ = nullptr;
    std::vector<Gtk::Label*> power_values_;
    std::vector<AggEntry> power_max_labels_;

    GraphArea* temp_graph_ = nullptr;
    std::vector<Gtk::Label*> temp_values_;
    std::vector<AggEntry> temp_avg_labels_;
};
