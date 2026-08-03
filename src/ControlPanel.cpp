#include "ControlPanel.h"

#include <gtkmm/adjustment.h>
#include <gtkmm/frame.h>
#include <gtkmm/scrolledwindow.h>

#include <glibmm/markup.h>

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
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
        box->set_margin_top(4);
        box->set_margin_bottom(4);
        box->set_margin_start(6);
        box->set_margin_end(6);

        auto* name = Gtk::make_managed<Gtk::Label>();
        name->set_markup("<b>" + Glib::Markup::escape_text(s.name) + "</b>");
        name->set_xalign(0.0);
        box->append(*name);

        auto* detail = Gtk::make_managed<Gtk::Label>();
        detail->set_markup("<small>" + Glib::Markup::escape_text(s.detail) +
                           "</small>");
        detail->set_xalign(0.0);
        detail->add_css_class("dim-label");
        box->append(*detail);

        cards_ = Gtk::make_managed<Gtk::Label>();
        cards_->set_xalign(0.0);
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
    set_size_request(320, -1);

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

    // ---- frame rate ----
    {
        auto* frame = Gtk::make_managed<Gtk::Frame>("Frame rate");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        box->set_margin(8);

        max_speed_.set_active(true);
        max_speed_.set_tooltip_text(
            "Run the inference loop flat out — the frame rate reported is the "
            "card's ceiling for this model.");
        box->append(max_speed_);

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* lbl = Gtk::make_managed<Gtk::Label>("Target");
        lbl->set_xalign(0.0);
        row->append(*lbl);
        fps_spin_.set_adjustment(
            Gtk::Adjustment::create(kDefaultFps, 1, kMaxFps, 1, 10));
        fps_spin_.set_numeric(true);
        fps_spin_.set_sensitive(false);
        fps_spin_.set_tooltip_text(
            "Pace the loop to this rate. A card that cannot reach it simply "
            "runs as fast as it can.");
        row->append(fps_spin_);
        row->append(*Gtk::make_managed<Gtk::Label>("fps"));
        box->append(*row);

        max_speed_.signal_toggled().connect(
            [this] { fps_spin_.set_sensitive(!max_speed_.get_active()); });

        frame->set_child(*box);
        append(*frame);
    }

    // ---- inference API ----
    {
        auto* frame = Gtk::make_managed<Gtk::Frame>("Inference API");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        box->set_margin(8);
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
        box->append(sync_radio_);
        box->append(async_radio_);

        auto* note = Gtk::make_managed<Gtk::Label>();
        note->set_markup(
            "<small>Not every SDK offers both; the frame-rate legend "
            "flags a card whose mode is emulated.</small>");
        note->set_xalign(0.0);
        note->set_wrap(true);
        note->add_css_class("dim-label");
        note->set_margin_top(4);
        box->append(*note);

        frame->set_child(*box);
        append(*frame);
    }

    // ---- accelerators ----
    {
        auto* frame = Gtk::make_managed<Gtk::Frame>("Accelerators");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        box->set_margin(8);
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
            box->append(*cb);
        }
        frame->set_child(*box);
        append(*frame);
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
    refresh_accel_sensitivity();
}

void ControlPanel::set_status(const std::string& text) {
    status_.set_markup(text);
}
