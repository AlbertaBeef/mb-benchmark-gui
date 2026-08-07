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
namespace {

// Depth means something different on every card — say which, at the control.
const char* depth_tooltip(Accel a) {
    switch (a) {
        case Accel::Hailo:
            return "Frames in flight: the requested async queue depth. HailoRT's "
                   "own reported queue size is a hard ceiling, so asking for "
                   "more than it will queue is clamped, not an error.\n\n"
                   "Greyed out in Sync, where the depth is 1 by definition.";
        case Accel::DeepX:
            return "Frames in flight: outstanding RunAsync jobs. Measured on "
                   "ResNet-50: 1 -> 401 fps, 4 -> 1078, 8 -> 1051 — depth pays "
                   "until the device saturates and nothing after.\n\n"
                   "Greyed out in Sync, where the depth is 1 by definition.";
        case Accel::MemryX:
            return "Frames in flight on the connect_stream pipeline, applied as "
                   "a permit count in the input callback.\n\n"
                   "Greyed out in Sync, which uses the blocking MxAcclMT::run() "
                   "instead — one frame by definition.";
        case Accel::Axelera:
            return "The Metis's only buffering knob is the runtime's boolean "
                   "double_buffer property, which overlaps the next frame's DMA "
                   "with the current run — so depth is 1 (off) or 2 (on), and "
                   "there is nothing deeper to ask for.\n\n"
                   "Always live: libaxruntime has no async API, so this is the "
                   "card's only concurrency control besides AIPU cores.";
        case Accel::Qualcomm:
            return "Engines in flight per NSP, each on its own host thread. "
                   "Measured on OSNet: 1.43x at depth 4, saturating by 8.\n\n"
                   "Always live: QNN has no async graph-execute, so this is "
                   "host-side pipelining and the card's concurrency knob.";
    }
    return "";
}

}  // namespace

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

    // Repaint the card line from the subject's current readiness. Cards this
    // build has no backend for are left out entirely — naming a model's DeepX
    // artifact on a host that cannot drive a DeepX is noise, and it made every
    // row look far better supported than it is.
    void refresh() {
        std::string on;
        for (int i = 0; i < kAccelCount; ++i) {
            const Accel a = accel_at(i);
            if (!accel_present(a) || !subject->configured(a)) continue;
            if (!on.empty()) on += " · ";
            const std::string nm = accel_name(a);
            on += subject->ready[i] ? nm : "(" + nm + ")";
        }
        if (on.empty()) on = "no build for this host";
        cards_->set_markup("<small>" + Glib::Markup::escape_text(on) + "</small>");
        // Selectable only once a card that is actually here can run it —
        // `runnable()` alone would light up a row whose only artifact belongs to
        // an absent card, and pressing Start would then do nothing.
        bool any = false;
        for (int i = 0; i < kAccelCount; ++i) {
            const Accel a = accel_at(i);
            if (accel_present(a) && subject->runnable(a)) any = true;
        }
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
        // Absent cards get no checkbox at all, so `col` tracks the visible ones
        // rather than the enum index — otherwise hiding a card would leave a
        // hole in the row.
        int col = 0;
        for (int i = 0; i < kAccelCount; ++i) {
            const Accel a = accel_at(i);
            if (!accel_present(a)) {
                accel_check_[i] = nullptr;
                accel_wanted_[i] = false;
                continue;
            }
            auto* cb = Gtk::make_managed<Gtk::CheckButton>(accel_name(a));
            accel_wanted_[i] = true;
            cb->set_active(true);
            cb->signal_toggled().connect([this, cb, i] {
                if (!syncing_accels_) accel_wanted_[i] = cb->get_active();
            });
            accel_check_[i] = cb;
            accel_grid->attach(*cb, col % kAccelCount, col / kAccelCount, 1, 1);
            ++col;
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
            if (!accel_present(a)) { graph_accel_[i] = nullptr; continue; }
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
            // No tab for a card this build has no backend for.
            if (!accel_present(a)) { depth_spin_[i] = nullptr; continue; }
            auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            page->set_margin(8);
            // Only shown on cards with nothing beyond API/Depth. The
            // "unavailable" wording this used to carry is gone with the greyed
            // tabs — an absent card has no tab to explain itself on.
            auto* note = Gtk::make_managed<Gtk::Label>();
            note->set_markup("<small>" +
                             Glib::Markup::escape_text(
                                 std::string("No ") + accel_name(a) +
                                 "-specific controls yet.") +
                             "</small>");
            note->set_xalign(0.0);
            note->set_wrap(true);
            note->add_css_class("dim-label");

            // API mode, first row of every tab. Radios where the vendor ships
            // both a blocking and an async inference API; a plain label where it
            // ships only one, so the UI never offers a mode that does not exist.
            {
                auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* lbl = Gtk::make_managed<Gtk::Label>("API");
                lbl->set_xalign(0.0);
                row->append(*lbl);
                if (accel_has_both_api_modes(a)) {
                    auto* sy = Gtk::make_managed<Gtk::CheckButton>("Sync");
                    auto* as = Gtk::make_managed<Gtk::CheckButton>("Async");
                    as->set_group(*sy);
                    as->set_active(true);   // async is the default everywhere
                    sy->set_tooltip_text(
                        "One frame at a time, waiting for each — what a "
                        "latency-bound real-time pipeline does.");
                    as->set_tooltip_text(
                        "Several frames in flight — peak throughput. Depth is "
                        "the Depth control just below.");
                    row->append(*sy);
                    row->append(*as);
                    api_sync_[i] = sy;
                    api_async_[i] = as;
                    for (auto* b : {sy, as}) {
                        conns_.push_back(b->signal_toggled().connect(
                            [this] { refresh_depth_sensitivity(); }));
                    }
                } else {
                    auto* only = Gtk::make_managed<Gtk::Label>(
                        accel_sole_api_mode_note(a));
                    only->set_xalign(0.0);
                    only->set_wrap(true);
                    only->add_css_class("dim-label");
                    row->append(*only);
                }
                page->append(*row);
            }

            // Depth — every backend has some form of it, but the mechanism and
            // the useful range differ, so the range is per card rather than one
            // shared 1-8. On a card with both API modes it is greyed out in
            // Sync, where the depth is 1 by definition; on a card with only a
            // blocking API it is that card's *only* concurrency knob and stays
            // live (Axelera's double_buffer, Qualcomm's engines per NSP).
            {
                auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* lbl = Gtk::make_managed<Gtk::Label>("Depth");
                lbl->set_xalign(0.0);
                row->append(*lbl);
                auto* sp = Gtk::make_managed<Gtk::SpinButton>();
                const int max_depth = (a == Accel::Axelera) ? 2 : 8;
                const int def_depth = (a == Accel::Axelera) ? 2 : 4;
                sp->set_adjustment(
                    Gtk::Adjustment::create(def_depth, 1, max_depth, 1, 1));
                sp->set_numeric(true);
                sp->set_tooltip_text(depth_tooltip(a));
                row->append(*sp);
                depth_spin_[i] = sp;
                page->append(*row);
            }

            // Per-card controls. Only some cards have anything else to
            // configure; the rest keep the placeholder note alone.
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
            } else if (a == Accel::Qualcomm) {
                // NSPs. Unlike Axelera's cores there is no cliff here — nothing
                // wedges — so the default is the maximum. Measured on OSNet:
                // 1556 fps on one NSP, 3571 on two (2.30x).
                auto* nrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* nlbl = Gtk::make_managed<Gtk::Label>("NSPs");
                nlbl->set_xalign(0.0);
                nrow->append(*nlbl);
                qualcomm_nsps_.set_adjustment(Gtk::Adjustment::create(2, 1, 2, 1, 1));
                qualcomm_nsps_.set_numeric(true);
                qualcomm_nsps_.set_tooltip_text(
                    "Hexagon NSPs this model claims. QCS9075 has two, each with "
                    "its own 8 MB VTCM, addressed as QNN device ids 0 and 1 — "
                    "this SoC's real concurrency primitive.\n\n"
                    "Measured on OSNet x1.0 in burst: 1556 fps on one NSP, "
                    "3571 on two. The runner clamps this to however many the "
                    "backend actually reports.");
                nrow->append(qualcomm_nsps_);
                page->append(*nrow);

                // Performance mode. Not a cosmetic knob: the spread across
                // modes is ~1.8x, so a Qualcomm frame rate without a stated
                // mode is not comparable to anything.
                auto* prow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* plbl = Gtk::make_managed<Gtk::Label>("Performance");
                plbl->set_xalign(0.0);
                prow->append(*plbl);
                for (const char* m : {"burst", "sustained_high_performance",
                                      "high_performance", "balanced", "power_saver"})
                    qualcomm_perf_.append(m);
                qualcomm_perf_.set_active_text("burst");
                qualcomm_perf_.set_tooltip_text(
                    "HTP DCVS mode, applied at load. The NPU runs under a "
                    "governor, and left alone a short inference can finish "
                    "before it ramps.\n\n"
                    "Measured on OSNet across two NSPs: burst 3381, "
                    "sustained_high_performance 2818, balanced 2384, "
                    "power_saver 1886 fps. Use burst for peak figures and "
                    "sustained_high_performance for steady-state — burst "
                    "thermally throttles on a passively-cooled board.");
                prow->append(qualcomm_perf_);
                page->append(*prow);

                // Backend. The SoC has three compute units and the shim can
                // dlopen all of them, but a context binary is built for one:
                // measured here, libQnnGpu.so rejects an HTP .bin with
                // GPU_ERROR_INVALID_VERSION and libQnnCpu.so reports no devices
                // at all. Offered anyway so a GPU/CPU-compiled artifact would
                // just work — the failure is loud and names the real cause.
                auto* brow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                auto* blbl = Gtk::make_managed<Gtk::Label>("Backend");
                blbl->set_xalign(0.0);
                brow->append(*blbl);
                qualcomm_backend_.append("HTP (Hexagon NPU)");
                qualcomm_backend_.append("GPU (Adreno)");
                qualcomm_backend_.append("CPU (reference)");
                qualcomm_backend_.set_active(0);
                qualcomm_backend_.set_tooltip_text(
                    "Which QNN backend library to load the model onto.\n\n"
                    "The catalog ships HTP context binaries, and a context "
                    "binary is compiled for one backend: selecting GPU or CPU "
                    "with these artifacts fails at load (the GPU reports "
                    "\"Context deserialization verification failure\", the CPU "
                    "backend reports no devices). They are offered so a GPU- or "
                    "CPU-compiled artifact dropped into the models tree runs "
                    "without a code change.");
                brow->append(qualcomm_backend_);
                page->append(*brow);
            }

            // Cards with real controls do not need the placeholder telling the
            // user there are none.
            if (a != Accel::MemryX && a != Accel::Axelera && a != Accel::Qualcomm) {
                page->append(*note);
            }
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
        // A hidden card has no widget, so every checkbox reaching here belongs
        // to a card this build can drive — the old "SDK not found" tooltip case
        // is gone with it, and greying now means only "no artifact".
        if (!accel_check_[i]) continue;
        const bool has_model = s && s->runnable(a);
        accel_check_[i]->set_sensitive(has_model && !running_);
        accel_check_[i]->set_active(has_model && accel_wanted_[i]);
        if (!has_model) {
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
        if (!accel_check_[i] || !accel_check_[i]->get_active()) continue;
        if (!accel_present(a) || !s->runnable(a)) continue;
        BenchItem it;
        it.label = s->name;
        it.accel = a;
        it.members = s->members[i];
        if (a == Accel::MemryX) it.freq_mhz = memryx_freq_mhz();
        it.depth = depth(a);
        it.api_mode = api_mode(a);
        if (a == Accel::Axelera) it.cores = axelera_cores();
        if (a == Accel::Qualcomm) {
            it.nsps = qualcomm_nsps();
            it.perf_mode = qualcomm_perf_mode();
            it.qnn_backend = qualcomm_backend();
        }
        out.push_back(std::move(it));
    }
    return out;
}

double ControlPanel::target_fps() const {
    return max_speed_.get_active() ? 0.0 : fps_spin_.get_value();
}

ApiMode ControlPanel::api_mode(Accel a) const {
    const int i = accel_index(a);
    // A card with only one mode always reports it, whatever the caller asked.
    if (!accel_has_both_api_modes(a))
        return api_mode_is_native(a, ApiMode::Sync) ? ApiMode::Sync : ApiMode::Async;
    if (i >= 0 && i < kAccelCount && api_sync_[i] && api_sync_[i]->get_active())
        return ApiMode::Sync;
    return ApiMode::Async;
}

// Depth is meaningless on a card running Sync (one frame by definition), so
// each card's spin button follows its own API radio.
void ControlPanel::refresh_depth_sensitivity() {
    for (int i = 0; i < kAccelCount; ++i) {
        if (!depth_spin_[i]) continue;
        const Accel a = accel_at(i);
        // Live unless this card is running Sync with a real choice of modes.
        const bool meaningful =
            !accel_has_both_api_modes(a) || api_mode(a) == ApiMode::Async;
        depth_spin_[i]->set_sensitive(!running_ && meaningful);
    }
}

int ControlPanel::memryx_freq_mhz() const {
    const std::string s = memryx_freq_.get_active_text();
    try { return std::stoi(s); } catch (...) { return 0; }  // 0 = leave alone
}

int ControlPanel::qualcomm_nsps() const {
    return static_cast<int>(qualcomm_nsps_.get_value());
}

std::string ControlPanel::qualcomm_perf_mode() const {
    const std::string s = qualcomm_perf_.get_active_text();
    // Empty would mean "leave the governor alone", which is never what the
    // combo is trying to say — fall back to the default it displays.
    return s.empty() ? "burst" : s;
}

std::string ControlPanel::qualcomm_backend() const {
    // The combo shows what the unit *is*; the runner needs the library name.
    switch (qualcomm_backend_.get_active_row_number()) {
        case 1: return "libQnnGpu.so";
        case 2: return "libQnnCpu.so";
        default: break;
    }
    return "libQnnHtp.so";
}

GraphArea::RangeMode ControlPanel::range_mode() const {
    if (range_fixed_.get_active()) return GraphArea::RangeMode::Fixed;
    if (range_dynamic_.get_active()) return GraphArea::RangeMode::Dynamic;
    return GraphArea::RangeMode::Max;
}

unsigned ControlPanel::graph_accel_mask() const {
    unsigned m = 0;
    for (int i = 0; i < kAccelCount; ++i) {
        // Two distinct reasons the pointer can be null, and they mean opposite
        // things: a hidden card never gets a checkbox and must stay out of the
        // plots, while a present card is briefly null during construction and
        // should default to visible.
        if (!accel_present(accel_at(i))) continue;
        if (!graph_accel_[i] || graph_accel_[i]->get_active()) m |= 1u << i;
    }
    return m;
}

int ControlPanel::depth(Accel a) const {
    const int i = accel_index(a);
    if (i < 0 || i >= kAccelCount || !depth_spin_[i]) return 1;
    // Sync means exactly one frame outstanding — that is what Sync *is* — so a
    // dual-mode card reports 1 regardless of where its spin button sits.
    if (accel_has_both_api_modes(a) && api_mode(a) == ApiMode::Sync) return 1;
    return static_cast<int>(depth_spin_[i]->get_value());
}

int ControlPanel::axelera_cores() const {
    return static_cast<int>(axelera_cores_.get_value());
}

void ControlPanel::set_running(bool running) {
    running_ = running;
    run_button_.set_label(running ? "Stop benchmark" : "Start benchmark");
    run_button_.remove_css_class(running ? "suggested-action" : "destructive-action");
    run_button_.add_css_class(running ? "destructive-action" : "suggested-action");
    // Both flags gate the selection, and both paths must agree: set_running(false)
    // fires the instant Stop is pressed, while set_busy(true) only arrives on the
    // next 1 Hz tick — and set_busy early-returns when its flag is unchanged, so
    // an unconditional re-enable here would never be undone.
    notebook_.set_sensitive(!running && !busy_);
    // Per-card API radios: lock them all while a run is in flight, same as the
    // other per-card settings.
    for (int i = 0; i < kAccelCount; ++i) {
        if (api_sync_[i]) api_sync_[i]->set_sensitive(!running);
        if (api_async_[i]) api_async_[i]->set_sensitive(!running);
    }
    max_speed_.set_sensitive(!running);
    fps_spin_.set_sensitive(!running && !max_speed_.get_active());
    memryx_freq_.set_sensitive(!running);
    axelera_cores_.set_sensitive(!running);
    qualcomm_nsps_.set_sensitive(!running);
    qualcomm_perf_.set_sensitive(!running);
    qualcomm_backend_.set_sensitive(!running);
    refresh_depth_sensitivity();
    refresh_accel_sensitivity();
}

void ControlPanel::set_busy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    run_button_.set_sensitive(!busy);
    // Lock the selection too, not just Start. A previous run's workers are still
    // draining, so an edit made now cannot be acted on until they are gone —
    // leaving the lists live invites a change the panel will not honour, and
    // makes a stall look like it was caused by the edit.
    notebook_.set_sensitive(!busy && !running_);
}

void ControlPanel::set_status(const std::string& text) {
    status_.set_markup(text);
}
