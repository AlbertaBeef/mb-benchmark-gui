// The left-hand benchmark controls: what to run, how fast, and on which cards.
//
// Two tabs — individual models and multi-inference pipelines — each a
// single-selection list. One subject is benchmarked at a time, on every enabled
// accelerator that supports it, which is what keeps the per-card numbers
// meaningful: two models sharing a card would split its throughput and its
// watts, and neither FPS/W nor frames/joule would mean anything per model.
#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/notebook.h>
#include <gtkmm/spinbutton.h>

#include <string>
#include <vector>

#include "Automation.h"
#include "Bench.h"
#include "Catalog.h"
#include "GraphArea.h"

class ControlPanel : public Gtk::Box {
public:
    explicit ControlPanel(const Catalog& catalog);
    ~ControlPanel() override;

    // Fired when the Start / Stop button is pressed.
    sigc::signal<void()>& signal_start_stop() { return sig_start_stop_; }

    // The currently selected subject resolved into one item per enabled,
    // supported accelerator. Empty when nothing runnable is selected.
    std::vector<BenchItem> selection() const;

    // Target frame rate, or <= 0 for "max speed".
    double target_fps() const;

    // Which inference API a given card should drive. Per card: the SDKs are not
    // symmetric, so a single global choice offered modes that do not exist.
    // Cards without both modes always report the one they have.
    ApiMode api_mode(Accel a) const;

    // How many frames a given card keeps in flight. Per card: the mechanism
    // differs per vendor and so does the useful range (Axelera's only knob is a
    // boolean, so it tops out at 2).
    int depth(Accel a) const;

    // Push an automation step's settings into the controls, so `selection()`
    // then builds exactly the run the plan asked for. Deliberately drives the
    // *widgets* rather than a parallel state: the panel keeps showing what is
    // actually running, and there is still only one source of truth.
    void apply_automation_settings(const AutomationSettings& s);

    // Select a subject by its catalog id, for --automation. Returns false if
    // no row has that id or no card can run it. Ids rather than display names:
    // ids are what a config file can carry without quoting.
    bool select_subject_by_id(const std::string& id, bool is_pipeline);

    // How every graph's axis top should respond to its data.
    GraphArea::RangeMode range_mode() const;

    // Which cards' traces to draw — any subset, all ticked by default.
    // Bit i (in Accel order) set means that card is shown. Filters the *graphs*
    // only: the legends keep reading live, and the Inference/Accelerators
    // checkboxes still decide what actually runs.
    unsigned graph_accel_mask() const;
    // Fired when the Range radios change, so the graphs re-scale immediately
    // rather than at the next tick.
    sigc::signal<void()>& signal_range_mode_changed() { return sig_range_mode_; }
    // Fired when the graph accelerator filter changes.
    sigc::signal<void()>& signal_graph_filter_changed() { return sig_graph_filter_; }

    // Per-accelerator settings from the card tabs.
    int memryx_freq_mhz() const;   // MPU clock to request before a run
    int axelera_cores() const;     // AIPU cores to claim on the Metis
    int qualcomm_nsps() const;     // Hexagon NSPs to claim on the SoC
    std::string qualcomm_perf_mode() const;  // HTP DCVS mode
    std::string qualcomm_backend() const;    // QNN backend library to dlopen

    // Flip the button between Start and Stop and lock the selection while a
    // run is in flight (changing models mid-run would mix two measurements).
    void set_running(bool running);

    void set_status(const std::string& text);

    // Keeps Start disabled while a previous run's workers are still being
    // joined — starting again then would fight the old run for the device.
    void set_busy(bool busy);

    // Re-read which artifacts are on disk and repaint the rows. Called as
    // downloads land, so a model becomes selectable the moment it arrives.
    void refresh_availability();

private:
    class SubjectRow;

    void populate(Gtk::ListBox& list, const std::vector<BenchSubject>& subjects);
    const BenchSubject* current_subject() const;
    // Grey out accelerators the selected subject has no compiled model for.
    void refresh_accel_sensitivity();
    void select_first_runnable();
    // Depth means nothing on a card running Sync (one frame by definition), so
    // grey that card's spin button when its own API radio says Sync.
    void refresh_depth_sensitivity();

    const Catalog& catalog_;

    Gtk::Notebook notebook_;
    Gtk::ListBox model_list_, pipeline_list_;
    std::vector<SubjectRow*> rows_;  // every row, both tabs, for repainting

    Gtk::CheckButton max_speed_{"Max"};
    Gtk::SpinButton fps_spin_;
    Gtk::SpinButton* depth_spin_[kAccelCount] = {};  // one per tab

    // Graphs / Range. Max is the default: it is the behaviour the graphs have
    // had since the headroom rule landed, and the one that never clips.
    Gtk::CheckButton range_fixed_{"Fixed"};
    Gtk::CheckButton range_max_{"Max"};
    Gtk::CheckButton range_dynamic_{"Dynamic"};

    // Graphs / Accelerators: one independent checkbox per card, in Accel order.
    // Independent, not a radio group — the point is comparing several cards on
    // one plot, so any subset has to be selectable.
    Gtk::CheckButton* graph_accel_[kAccelCount] = {nullptr, nullptr, nullptr,
                                                   nullptr, nullptr};

    // Per-card API radios, in Accel order. Only populated for cards where the
    // vendor ships both a blocking and an async inference API
    // (accel_has_both_api_modes()); the rest get a static label instead, so the
    // UI never offers a mode the runtime does not have.
    Gtk::CheckButton* api_sync_[kAccelCount] = {};
    Gtk::CheckButton* api_async_[kAccelCount] = {};

    // Per-accelerator controls, one tab each. Empty scaffolding for now — the
    // enable/disable checkboxes live in the Inference frame, since they choose
    // what a run targets rather than configuring a card.
    Gtk::Notebook accel_notebook_;
    Gtk::ComboBoxText memryx_freq_;   // MemryX tab: MPU clock
    Gtk::SpinButton axelera_cores_;   // Axelera tab: AIPU cores (1-4)
    Gtk::SpinButton qualcomm_nsps_;      // Qualcomm tab: Hexagon NSPs (1-2)
    Gtk::ComboBoxText qualcomm_perf_;    // Qualcomm tab: HTP DCVS mode
    Gtk::ComboBoxText qualcomm_backend_; // Qualcomm tab: HTP / GPU / CPU

    Gtk::CheckButton* accel_check_[kAccelCount] = {nullptr, nullptr, nullptr,
                                                   nullptr, nullptr};
    // What the user last asked for, per card, independent of whether the
    // current subject happens to support it. A checkbox forced off because a
    // model has no build for that card comes back on when one does, instead of
    // silently staying off.
    // NB every entry must be spelled out: a short initializer leaves the tail
    // value-initialized to *false*, which would silently disable a new card
    // everywhere without any other symptom.
    bool accel_wanted_[kAccelCount] = {true, true, true, true, true};

    // The control state as it was before automation first touched it. Every
    // step is applied as baseline + [automation] defaults + that step's
    // overrides, so a setting one step states cannot leak into the next.
    AutomationSettings auto_baseline_;
    bool auto_baseline_valid_ = false;
    bool syncing_accels_ = false;  // guards the programmatic set_active()

    Gtk::Button run_button_{"Start benchmark"};
    Gtk::Label status_;

    sigc::signal<void()> sig_start_stop_;
    sigc::signal<void()> sig_range_mode_;
    sigc::signal<void()> sig_graph_filter_;
    bool running_ = false;
    bool busy_ = false;   // workers from a previous run still shutting down

    // Widget-signal connections, severed in the destructor. Members are
    // destroyed in reverse declaration order, so the list boxes die *before*
    // notebook_; tearing the notebook down then emits switch_page, whose
    // handler reads the already-dead list box — `assertion 'GTK_IS_LIST_BOX
    // (box)' failed`, then a segfault on exit.
    std::vector<sigc::connection> conns_;
};
