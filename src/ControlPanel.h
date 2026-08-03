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
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/notebook.h>
#include <gtkmm/spinbutton.h>

#include <string>
#include <vector>

#include "Bench.h"
#include "Catalog.h"

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

    // Which of the vendors' two inference APIs to drive.
    ApiMode api_mode() const;

    // Flip the button between Start and Stop and lock the selection while a
    // run is in flight (changing models mid-run would mix two measurements).
    void set_running(bool running);

    void set_status(const std::string& text);

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

    const Catalog& catalog_;

    Gtk::Notebook notebook_;
    Gtk::ListBox model_list_, pipeline_list_;
    std::vector<SubjectRow*> rows_;  // every row, both tabs, for repainting

    Gtk::CheckButton max_speed_{"Max speed"};
    Gtk::SpinButton fps_spin_;

    Gtk::CheckButton sync_radio_{"Sync API"};
    Gtk::CheckButton async_radio_{"Async API"};

    Gtk::CheckButton* accel_check_[kAccelCount] = {nullptr, nullptr, nullptr, nullptr};
    // What the user last asked for, per card, independent of whether the
    // current subject happens to support it. A checkbox forced off because a
    // model has no build for that card comes back on when one does, instead of
    // silently staying off.
    bool accel_wanted_[kAccelCount] = {true, true, true, true};
    bool syncing_accels_ = false;  // guards the programmatic set_active()

    Gtk::Button run_button_{"Start benchmark"};
    Gtk::Label status_;

    sigc::signal<void()> sig_start_stop_;
    bool running_ = false;

    // Widget-signal connections, severed in the destructor. Members are
    // destroyed in reverse declaration order, so the list boxes die *before*
    // notebook_; tearing the notebook down then emits switch_page, whose
    // handler reads the already-dead list box — `assertion 'GTK_IS_LIST_BOX
    // (box)' failed`, then a segfault on exit.
    std::vector<sigc::connection> conns_;
};
