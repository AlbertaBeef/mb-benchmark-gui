#include "ControlPanel.h"

#include <gtkmm/adjustment.h>
#include <gtkmm/frame.h>
#include <gtkmm/grid.h>
#include <gtkmm/scrolledwindow.h>

#include <glibmm/markup.h>
#include <pangomm/layout.h>

namespace {
constexpr int kDefaultFps = 30;
constexpr int kMaxFps = 1000;
}  // namespace

// A list row that remembers which subject it stands for. The third line lists
// the cards this subject can run on; one still downloading is shown in
// parentheses so a half-populated models tree reads as "not yet" rather than
// "not supported".
class ControlPanel::SubjectRow : public Gtk::ListBoxRow {
public:
    explicit SubjectRow(const BenchSubject& s) : subject(&s) {
        // One line per entry: name, what it is, which cards can run it. Fixed
        // character widths on the first two keep the columns aligned down the
        // list rather than ragged.
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        box->set_margin_top(3);
        box->set_margin_bottom(3);
        box->set_margin_start(6);
        box->set_margin_end(6);

        auto* name = Gtk::make_managed<Gtk::Label>();
        name->set_markup("<b>" + Glib::Markup::escape_text(s.name) + "</b>");
        name->set_xalign(0.0);
        name->set_width_chars(24);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        box->append(*name);

        auto* detail = Gtk::make_managed<Gtk::Label>();
        detail->set_markup("<small>" + Glib::Markup::escape_text(s.detail) +
                           "</small>");
        detail->set_xalign(0.0);
        detail->set_width_chars(30);
        detail->set_ellipsize(Pango::EllipsizeMode::END);
        detail->add_css_class("dim-label");
        box->append(*detail);

        cards_ = Gtk::make_managed<Gtk::Label>();
        cards_->set_xalign(0.0);
        cards_->set_hexpand(true);
        cards_->set_ellipsize(Pango::EllipsizeMode::END);
        cards_->add_css_class("dim-label");
        box->append(*cards_);

        set_child(*box);
        refresh();
    }

    // Repaint the card line from the subject's current readiness.
    void refresh() {
        std::string on;
        for (int i = 0; i < kAccelCount; ++i) {
            if (!subject->configured(accel_at(i))) continue;
            if (!on.empty()) on += " · ";
            const std::string nm = accel_name(accel_at(i));
            on += subject->ready[i] ? nm : "(" + nm + ")";
        }
        if (on.empty()) on = "no build configured";
        cards_->set_markup("<small>" + Glib::Markup::escape_text(on) + "</small>");
        // Selectable only once at least one card can actually run it.
        bool any = false;
        for (int i = 0; i < kAccelCount; ++i)
            if (subject->runnable(accel_at(i))) any = true;
        set_sensitive(any);
    }

    const BenchSubject* subject;

private:
    Gtk::Label* cards_ = nullptr;
};

ControlPanel::ControlPanel(const Catalog& catalog)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 8), catalog_(catalog) {
    set_margin(4);
    // 640 is the *minimum*; the panel fills whatever the divider gives it and
    // its contents stay flush left. Spelled out rather than left to defaults.
    set_size_request(640, -1);
    set_halign(Gtk::Align::FILL);
    set_valign(Gtk::Align::FILL);

    // ---- what to run ----
    populate(model_list_, catalog_.models());
    populate(pipeline_list_, catalog_.pipelines());

    auto wrap = [](Gtk::ListBox& list) -> Gtk::Widget& {
        auto* sw = Gtk::make_managed<Gtk::ScrolledWindow>();
        sw->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        sw->set_child(list);
        sw->set_vexpand(true);
        return *sw;
    };
    notebook_.append_page(wrap(model_list_), "Models");
    notebook_.append_page(wrap(pipeline_list_), "Pipelines");
    notebook_.set_vexpand(true);
    append(notebook_);

    auto on_selection_changed = [this](Gtk::ListBoxRow*) {
        refresh_accel_sensitivity();
    };
    conns_.push_back(model_list_.signal_row_selected().connect(on_selection_changed));
    conns_.push_back(pipeline_list_.signal_row_selected().connect(on_selection_changed));
    conns_.push_back(notebook_.signal_switch_page().connect(
        [this](Gtk::Widget*, guint) { refresh_accel_sensitivity(); }));

    // ---- inference: how to drive it, and how fast ----
    {
        auto* frame = Gtk::make_managed<Gtk::Frame>("Inference");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        box->set_margin(8);

        // Matching label widths so the two rows' controls line up.
        constexpr int kLabelChars = 12;
        auto row_label = [](const char* text) {
            auto* l = Gtk::make_managed<Gtk::Label>(text);
            l->set_xalign(0.0);
            l->set_width_chars(kLabelChars);
            return l;
        };

        // --- API ---
        auto* api_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        api_row->append(*row_label("API"));
        async_radio_.set_group(sync_radio_);
        // Async by default: it is what the hardware can actually do (4-5x on
        // Hailo and DeepX), so it is the fairer first impression of a card.
        async_radio_.set_active(true);
        sync_radio_.set_tooltip_text(
            "One frame at a time, waiting for each — what a latency-bound "
            "real-time pipeline does. Native on Hailo, DeepX and Axelera; "
            "emulated on MemryX, whose SDK has no blocking call.");
        async_radio_.set_tooltip_text(
            "Keep several frames in flight — peak throughput. Native on Hailo "
            "(run_async), DeepX (RunAsync/Wait) and MemryX (connect_stream); "
            "Axelera has no async API, so it falls back to the runtime's own "
            "double buffering.");
        api_row->append(sync_radio_);
        api_row->append(async_radio_);
        box->append(*api_row);

        // --- threads (in-flight depth) ---
        // Sits under API because it only means anything in Async mode: it is the
        // number of frames each card keeps outstanding. Greyed out in Sync,
        // where the depth is 1 by definition.
        auto* threads_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        threads_row->append(*row_label("Threads"));
        threads_spin_.set_adjustment(Gtk::Adjustment::create(4, 1, 8, 1, 1));
        threads_spin_.set_numeric(true);
        threads_spin_.set_tooltip_text(
            "How many frames each card keeps in flight in Async mode — DeepX "
            "job depth, MemryX stream depth, Hailo's async queue (its own "
            "reported queue size is the ceiling). Axelera has no async API, so "
            "this does not apply to it; use its AIPU cores control instead.\n\n"
            "Ignored in Sync mode, where exactly one frame is outstanding.");
        threads_row->append(threads_spin_);
        box->append(*threads_row);
        conns_.push_back(sync_radio_.signal_toggled().connect(
            [this] { threads_spin_.set_sensitive(!sync_radio_.get_active()); }));

        // --- frame rate ---
        auto* rate_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        rate_row->append(*row_label("Frame Rate"));
        max_speed_.set_active(true);
        max_speed_.set_tooltip_text(
            "Run the inference loop flat out — the frame rate reported is the "
            "card's ceiling for this model.");
        rate_row->append(max_speed_);

        auto* target = Gtk::make_managed<Gtk::Label>("Fixed");
        target->set_xalign(0.0);
        rate_row->append(*target);
        fps_spin_.set_adjustment(
            Gtk::Adjustment::create(kDefaultFps, 1, kMaxFps, 1, 10));
        fps_spin_.set_numeric(true);
        fps_spin_.set_sensitive(false);
        fps_spin_.set_tooltip_text(
            "Pace the loop to this rate. A card that cannot reach it simply "
            "runs as fast as it can.");
        rate_row->append(fps_spin_);
        rate_row->append(*Gtk::make_managed<Gtk::Label>("fps"));
        max_speed_.signal_toggled().connect(
            [this] { fps_spin_.set_sensitive(!max_speed_.get_active()); });
        box->append(*rate_row);

        // --- which cards to run on ---
        // A Grid, not a Box: wraps past four, same rule as the graph legends.
        auto* accel_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        accel_row->append(*row_label("Accelerators"));
        auto* accel_grid = Gtk::make_managed<Gtk::Grid>();
        accel_grid->set_column_spacing(16);
        accel_grid->set_row_spacing(2);
        for (int i = 0; i < kAccelCount; ++i) {
            const Accel a = accel_at(i);
            auto* cb = Gtk::make_managed<Gtk::CheckButton>(accel_name(a));
            const bool usable = accel_compiled_in(a);
            accel_wanted_[i] = usable;
            cb->set_active(usable);
            if (!usable) {
                cb->set_sensitive(false);
                cb->set_tooltip_text(accel_unavailable_reason(a));
            }
            cb->signal_toggled().connect([this, cb, i] {
                if (!syncing_accels_) accel_wanted_[i] = cb->get_active();
            });
            accel_check_[i] = cb;
            accel_grid->attach(*cb, i % 4, i / 4, 1, 1);
        }
        accel_row->append(*accel_grid);
        box->append(*accel_row);

        frame->set_child(*box);
        append(*frame);
    }

    // ---- Graphs ----
    {
        auto* frame = Gtk::make_managed<Gtk::Frame>("Graphs");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        box->set_margin(8);

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto* lbl = Gtk::make_managed<Gtk::Label>("Range");
        lbl->set_xalign(0.0);
        lbl->set_width_chars(12);   // same as the Inference rows, so they align
        row->append(*lbl);

        range_max_.set_group(range_fixed_);
        range_dynamic_.set_group(range_fixed_);
        range_max_.set_active(true);   // current behaviour is the default

        range_fixed_.set_tooltip_text(
            "Leave every axis at its resting top — 100 °C for temperature, the "
            "per-graph floor elsewhere. Readings above it are clipped and draw "
            "flat along the top edge. This is what the graphs did originally.");
        range_max_.set_tooltip_text(
            "Start at the resting top and grow to 10 % above the highest "
            "reading once the data reaches it. The axis never shrinks back, so "
            "successive runs stay comparable on one scale.");
        range_dynamic_.set_tooltip_text(
            "Scale to the data in both directions — always 10 % above the "
            "highest reading in the window, however small. Fills the plot, but "
            "the scale moves as the data does, so two runs are not directly "
            "comparable by eye.");

        row->append(range_fixed_);
        row->append(range_max_);
        row->append(range_dynamic_);
        box->append(*row);

        for (auto* b : {&range_fixed_, &range_max_, &range_dynamic_}) {
            conns_.push_back(b->signal_toggled().connect([this, b] {
                if (b->get_active()) sig_range_mode_.emit();
            }));
        }

        // --- which cards' traces to draw ---
        // Independent checkboxes: any subset. Untick a card to take it out of
        // the plots (and out of the axis calculation) without stopping it.
        auto* filt = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto* flbl = Gtk::make_managed<Gtk::Label>("Accelerators");
        flbl->set_xalign(0.0);
        flbl->set_width_chars(12);
        filt->append(*flbl);
        for (int i = 0; i < kAccelCount; ++i) {
            const Accel a = accel_at(i);
            auto* b = Gtk::make_managed<Gtk::CheckButton>(accel_name(a));
            b->set_active(true);   // everything shown by default
            b->set_tooltip_text(
                std::string("Draw ") + accel_name(a) +
                " on the graphs. Unticking hides its traces — including the "
                "history already on screen — and drops it from the axis "
                "calculation, so the remaining cards fill the plot. The card "
                "keeps running either way; its legend value keeps updating.");
            filt->append(*b);
            graph_accel_[i] = b;
            conns_.push_back(b->signal_toggled().connect(
                [this] { sig_graph_filter_.emit(); }));
        }
        box->append(*filt);

        frame->set_child(*box);
        append(*frame);
    }

    // ---- per-accelerator controls, one tab each ----
    // No enclosing Frame: the tab strip already delimits it, and the checkboxes
    // that used to need the "Accelerators" heading now live under Inference.
    {
        for (int i = 0; i < kAccelCount; ++i) {
            const Accel a = accel_at(i);
            auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            page->set_margin(8);
            auto* note = Gtk::make_managed<Gtk::Label>();
            const char* why = accel_unavailable_reason(a);
            note->set_markup(
                "<small>" +
                Glib::Markup::escape_text(
                    *why ? std::string(accel_name(a)) + ": " + why
                         : std::string("No ") + accel_name(a) +
                               "-specific controls yet.") +
                "</small>");
            note->set_xalign(0.0);
            note->set_wrap(true);
            note->add_css_class("dim-label");

            // Per-card controls. Only two cards have anything to configure
            // today; the rest keep the placeholder note alone.
            if (a == Accel::MemryX) {
                auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* lbl = Gtk::make_managed<Gtk::Label>("Core frequency");
                lbl->set_xalign(0.0);
                row->append(*lbl);
                for (int mhz : {200, 300, 400, 450, 500, 600, 700, 750, 800, 850})
                    memryx_freq_.append(std::to_string(mhz));
                memryx_freq_.set_active_text("600");  // 14 TOPS default
                memryx_freq_.set_tooltip_text(
                    "MPU clock requested before the run starts. 600 is the "
                    "14 TOPS default, 850 the 20 TOPS mode — which the part "
                    "cannot hold thermally for long.");
                row->append(memryx_freq_);
                row->append(*Gtk::make_managed<Gtk::Label>("MHz"));
                page->append(*row);
            } else if (a == Accel::Axelera) {
                auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* lbl = Gtk::make_managed<Gtk::Label>("AIPU cores");
                lbl->set_xalign(0.0);
                row->append(*lbl);
                // Default 2, conservatively. Claiming all four cores wedges
                // this card: all four MSI vectors time out, the PCIe link drops
                // and the Metis falls back to bootloader — recoverable only by
                // a full power-off. 3 has been seen to work but sits one step
                // from the cliff, so the shipped default stays at 2 and raising
                // it is a deliberate act. See "Known issues" in the README.
                axelera_cores_.set_adjustment(Gtk::Adjustment::create(2, 1, 4, 1, 1));
                axelera_cores_.set_numeric(true);
                axelera_cores_.set_tooltip_text(
                    "AIPU cores this model claims (num_sub_devices). The Metis "
                    "has four; one core leaves the others idle at 50 MHz, "
                    "visible in the Frequency graph.\n\n"
                    "WARNING: 4 has been observed to wedge the card — all four "
                    "MSI vectors time out and the PCIe link drops, needing a "
                    "power-off to recover. 3 works but is one step from that; "
                    "the default is 2.");
                row->append(axelera_cores_);
                page->append(*row);
            }

            page->append(*note);
            accel_notebook_.append_page(*page, accel_name(a));
        }
        append(accel_notebook_);
    }

    // ---- run ----
    run_button_.add_css_class("suggested-action");
    run_button_.signal_clicked().connect([this] { sig_start_stop_.emit(); });
    append(run_button_);

    status_.set_xalign(0.0);
    status_.set_wrap(true);
    status_.add_css_class("dim-label");
    append(status_);

    select_first_runnable();
    refresh_accel_sensitivity();
}

// Sever every widget-signal connection while all the widgets are still alive.
// The destructor body runs before any member is destroyed, which is the only
// window in which this is safe.
ControlPanel::~ControlPanel() {
    for (auto& c : conns_) c.disconnect();
    conns_.clear();
}

void ControlPanel::populate(Gtk::ListBox& list,
                            const std::vector<BenchSubject>& subjects) {
    list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    for (const auto& s : subjects) {
        auto* row = Gtk::make_managed<SubjectRow>(s);
        rows_.push_back(row);
        list.append(*row);
    }
    if (subjects.empty()) {
        auto* note = Gtk::make_managed<Gtk::Label>(
            "Nothing in the catalog.\nCheck config/models.conf — it is what "
            "populates these lists.");
        note->set_wrap(true);
        note->set_margin(12);
        note->add_css_class("dim-label");
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*note);
        row->set_selectable(false);
        list.append(*row);
    }
}

const BenchSubject* ControlPanel::current_subject() const {
    const Gtk::ListBox& list =
        notebook_.get_current_page() == 1 ? pipeline_list_ : model_list_;
    const auto* row =
        dynamic_cast<const SubjectRow*>(list.get_selected_row());
    return row ? row->subject : nullptr;
}

void ControlPanel::refresh_availability() {
    for (auto* r : rows_) r->refresh();
    if (!current_subject()) select_first_runnable();
    refresh_accel_sensitivity();
}

// Pick the first row that at least one card can actually run, so a partly
// downloaded models tree still opens on something usable.
void ControlPanel::select_first_runnable() {
    for (Gtk::ListBox* list : {&model_list_, &pipeline_list_}) {
        for (int i = 0;; ++i) {
            auto* row = dynamic_cast<SubjectRow*>(list->get_row_at_index(i));
            if (!row) break;
            if (!row->get_sensitive()) continue;
            list->select_row(*row);
            notebook_.set_current_page(list == &pipeline_list_ ? 1 : 0);
            return;
        }
    }
}

void ControlPanel::refresh_accel_sensitivity() {
    const BenchSubject* s = current_subject();
    syncing_accels_ = true;
    for (int i = 0; i < kAccelCount; ++i) {
        const Accel a = accel_at(i);
        const bool compiled = accel_compiled_in(a);
        const bool has_model = s && s->runnable(a);
        const bool usable = compiled && has_model && !running_;
        accel_check_[i]->set_sensitive(usable);
        accel_check_[i]->set_active(compiled && has_model && accel_wanted_[i]);
        if (!compiled) {
            accel_check_[i]->set_tooltip_text(accel_unavailable_reason(a));
        } else if (!has_model) {
            const bool pending = s && s->configured(a);
            accel_check_[i]->set_tooltip_text(
                !s ? "Select a model first"
                   : pending ? "Model not downloaded yet"
                             : "No build configured for this card");
        } else {
            accel_check_[i]->set_tooltip_text("");
        }
    }
    syncing_accels_ = false;
}

std::vector<BenchItem> ControlPanel::selection() const {
    std::vector<BenchItem> out;
    const BenchSubject* s = current_subject();
    if (!s) return out;
    for (int i = 0; i < kAccelCount; ++i) {
        const Accel a = accel_at(i);
        if (!accel_check_[i]->get_active()) continue;
        if (!accel_compiled_in(a) || !s->runnable(a)) continue;
        BenchItem it;
        it.label = s->name;
        it.accel = a;
        it.members = s->members[i];
        if (a == Accel::MemryX) it.freq_mhz = memryx_freq_mhz();
        it.threads = threads();
        if (a == Accel::Axelera) it.cores = axelera_cores();
        out.push_back(std::move(it));
    }
    return out;
}

double ControlPanel::target_fps() const {
    return max_speed_.get_active() ? 0.0 : fps_spin_.get_value();
}

ApiMode ControlPanel::api_mode() const {
    return async_radio_.get_active() ? ApiMode::Async : ApiMode::Sync;
}

int ControlPanel::memryx_freq_mhz() const {
    const std::string s = memryx_freq_.get_active_text();
    try { return std::stoi(s); } catch (...) { return 0; }  // 0 = leave alone
}

GraphArea::RangeMode ControlPanel::range_mode() const {
    if (range_fixed_.get_active()) return GraphArea::RangeMode::Fixed;
    if (range_dynamic_.get_active()) return GraphArea::RangeMode::Dynamic;
    return GraphArea::RangeMode::Max;
}

unsigned ControlPanel::graph_accel_mask() const {
    unsigned m = 0;
    for (int i = 0; i < kAccelCount; ++i)
        if (!graph_accel_[i] || graph_accel_[i]->get_active()) m |= 1u << i;
    return m;
}

int ControlPanel::threads() const {
    return static_cast<int>(threads_spin_.get_value());
}

int ControlPanel::axelera_cores() const {
    return static_cast<int>(axelera_cores_.get_value());
}

void ControlPanel::set_running(bool running) {
    running_ = running;
    run_button_.set_label(running ? "Stop benchmark" : "Start benchmark");
    run_button_.remove_css_class(running ? "suggested-action" : "destructive-action");
    run_button_.add_css_class(running ? "destructive-action" : "suggested-action");
    notebook_.set_sensitive(!running);
    sync_radio_.set_sensitive(!running);
    async_radio_.set_sensitive(!running);
    max_speed_.set_sensitive(!running);
    fps_spin_.set_sensitive(!running && !max_speed_.get_active());
    memryx_freq_.set_sensitive(!running);
    axelera_cores_.set_sensitive(!running);
    threads_spin_.set_sensitive(!running && !sync_radio_.get_active());
    refresh_accel_sensitivity();
}

void ControlPanel::set_busy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    run_button_.set_sensitive(!busy);
}

void ControlPanel::set_status(const std::string& text) {
    status_.set_markup(text);
}
