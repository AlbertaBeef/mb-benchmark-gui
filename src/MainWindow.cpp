#include "MainWindow.h"

#include <glib.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gdkmm/display.h>
#include <gdkmm/texture.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <pangomm/layout.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stylecontext.h>

#include <cmath>
#include <cstdio>

#include "util.h"

namespace {
constexpr int kIntervalMs = 1000;
constexpr int kSpanSeconds = 60;
constexpr int kHistory = kSpanSeconds + 1;
constexpr double kTempAxisMax = 100.0;  // °C

const char* kTitle = "NPU Benchmarking GUI";

std::string fmt_temp(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f°C", v);
    return b;
}
std::string fmt_power(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.2f W", v);
    return b;
}
std::string fmt_temp_axis(double v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f°C", v);
    return b;
}
std::string fmt_freq(double v) {
    if (std::isnan(v)) return "—";
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f MHz", v);
    return b;
}
std::string fmt_fps(double v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}
std::string fmt_eff(double v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}
}  // namespace

MainWindow::MainWindow() {
    set_title(kTitle);
    // Width tracks the Paned position: the controls take 680 (was 340), so the
    // window grows by the same 340 to leave the graph column the ~1060 it had
    // before. Height is unchanged — the graph column scrolls, and two of the six
    // sections ship collapsed.
    set_default_size(1740, 960);

    // Teal title bar (brand Primary), white title text — same chrome as the
    // sibling power monitor, so the two read as one family of tools.
    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    auto* title = Gtk::make_managed<Gtk::Label>(kTitle);
    title->add_css_class("title");
    header->set_title_widget(*title);

    auto* about_btn = Gtk::make_managed<Gtk::Button>();
    about_btn->set_icon_name("help-about-symbolic");
    about_btn->set_tooltip_text("About");
    about_btn->add_css_class("flat");
    about_btn->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_about));
    header->pack_end(*about_btn);
    set_titlebar(*header);

    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        "headerbar { background: #64A19D; box-shadow: none; }"
        "headerbar label.title { color: #FFFFFF; font-weight: bold; }"
        // Round the bottom corners the way GNOME's own windows do (System
        // Monitor, and anything libadwaita-based). Plain GTK4's Adwaita rounds
        // only the top two on a client-side-decorated window. The radius has to
        // go on the `decoration` node — that is what actually shapes a CSD
        // window and its shadow — and on the window itself, so the background
        // it paints is clipped to the same curve.
        "window.csd decoration {"
        "  border-bottom-left-radius: 12px;"
        "  border-bottom-right-radius: 12px;"
        "}"
        "window.csd {"
        "  border-bottom-left-radius: 12px;"
        "  border-bottom-right-radius: 12px;"
        "}"
        // The paned and its scroller reach the window edge; if either paints an
        // opaque background it squares the corner off again.
        "window.csd paned,"
        "window.csd scrolledwindow { background: transparent; }"
        ".mb-about selection { background-color: transparent; color: inherit; }");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Before any worker thread exists: setenv() must not race with getenv().
    prepare_backend_environment();

    std::vector<std::string> notes;
    probes_.discover(&notes);
    catalog_.discover(&notes);
    for (const auto& n : notes) g_message("%s", n.c_str());

    device_palette_ = util::make_palette(std::max(1, probes_.device_count()));

    // A device with a color_alias (a mapped INA228) reuses the swatch of the
    // accelerator it names, so the external shunt trace matches its card.
    {
        const int nd = probes_.device_count();
        std::vector<std::string> dname(nd), dalias(nd);
        auto index_devs = [&](const std::vector<MetricInfo>& ms) {
            for (const auto& m : ms)
                if (m.device >= 0 && m.device < nd) {
                    dname[m.device] = m.device_name;
                    if (!m.color_alias.empty()) dalias[m.device] = m.color_alias;
                }
        };
        index_devs(probes_.temp_metrics());
        index_devs(probes_.power_metrics());
        index_devs(probes_.freq_metrics());

        // Fixed colour per accelerator, before the alias pass — a mapped
        // INA228 then inherits its card's colour rather than a palette slot.
        for (int i = 0; i < nd; ++i) {
            Gdk::RGBA c;
            if (util::device_accent(dname[i], c)) device_palette_[i] = c;
        }
        for (int i = 0; i < nd; ++i) {
            if (dalias[i].empty()) continue;
            for (int j = 0; j < nd; ++j)
                if (j != i && dname[j] == dalias[i]) {
                    device_palette_[i] = device_palette_[j];
                    break;
                }
        }
        // Same fixed colours for the benchmark graphs, so a card's frame-rate
        // trace matches its power trace — and still draws correctly even if
        // Probes never discovered that card.
        for (int a = 0; a < kAccelCount; ++a) {
            if (!util::device_accent(accel_name(accel_at(a)), accel_color_[a])) {
                accel_color_[a] = util::make_palette(kAccelCount)[a];
            }
        }
    }
    last_time_us_ = g_get_monotonic_time();

    // ---- layout: controls | graphs ----
    auto* paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    paned->set_position(680);
    set_child(*paned);

    controls_ = Gtk::make_managed<ControlPanel>(catalog_);
    controls_->signal_start_stop().connect(
        sigc::mem_fun(*this, &MainWindow::on_start_stop));
    paned->set_start_child(*controls_);
    paned->set_resize_start_child(false);
    // Never allocate the controls less than their minimum: GTK4's default
    // shrink-start-child lets the handle squeeze a child below its size
    // request, and a widget rendered under its minimum anchors its contents
    // unpredictably instead of staying flush left.
    paned->set_shrink_start_child(false);

    auto* graphs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    graphs->set_margin(12);
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(*graphs);
    paned->set_end_child(*scroller);
    paned->set_resize_end_child(true);

    // Order is deliberate: the two telemetry graphs first (they are live even
    // with no benchmark running), then what a run produces — rate, then the two
    // efficiency views derived from rate and power.
    graphs->append(make_section(
        "Power (W)",
        build_metric_section(probes_.power_metrics(),
                             colors_for(probes_.power_metrics()),
                             /*fixed_temp_axis=*/false, fmt_power, power_graph_,
                             power_values_,
                             "No power source available. On the M.2 cards power "
                             "comes only from the Hailo firmware session or the "
                             "MemryX SDK; otherwise it takes an external meter "
                             "(INA228). Without watts, the efficiency graphs "
                             "below stay at zero.",
                             &power_max_labels_)));

    graphs->append(make_section(
        "Temperature (°C)",
        build_metric_section(probes_.temp_metrics(),
                             colors_for(probes_.temp_metrics()),
                             /*fixed_temp_axis=*/true, fmt_temp, temp_graph_,
                             temp_values_, "No temperature sensors detected.",
                             &temp_avg_labels_)));

    // Clock, right after temperature because the two are read together: a
    // frequency that sags while a die heats is thermal throttling, and seeing
    // them adjacent is the whole point. Collapsed by default — it is diagnostic
    // rather than something to watch every run.
    graphs->append(make_section(
        "Frequency (MHz)",
        build_metric_section(probes_.freq_metrics(),
                             colors_for(probes_.freq_metrics()),
                             /*fixed_temp_axis=*/false, fmt_freq, freq_graph_,
                             freq_values_,
                             "No accelerator here reports a core clock. All "
                             "four can: Hailo via the extended device "
                             "information, DeepX per NPU via dxrt-cli, MemryX "
                             "per chip via the SDK, and Axelera per AI core via "
                             "axcmd --clock-all-actual.",
                             &freq_avg_labels_, /*min_axis_max=*/1000.0),
        /*expanded=*/false));

    fps_ = build_accel_section(fmt_fps, /*min_axis_max=*/30.0);
    graphs->append(make_section("Frame Rate (fps)", *fps_.root));

    eff_ = build_accel_section(fmt_eff, /*min_axis_max=*/5.0);
    graphs->append(make_section("Efficiency (fps/W)", *eff_.root));

    energy_ = build_accel_section(fmt_eff, /*min_axis_max=*/5.0);
    graphs->append(make_section("Energy (mJ/frame — lower is better)",
                                *energy_.root, /*expanded=*/false));

    // Pull anything the catalog knows a source for but that isn't on disk yet.
    // Runs on its own thread; rows light up as artifacts land.
    fetcher_.start(catalog_);
    poll_downloads();

    tick_conn_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &MainWindow::on_tick), kIntervalMs);
}

MainWindow::~MainWindow() {
    // Stop the tick first: it reads widgets and the engine, and must not fire
    // into a window that is already coming apart.
    tick_conn_.disconnect();
    // Then join every worker before the widgets they report into go away.
    fetcher_.stop();
    engine_.stop();
}

// Reflect download progress in the status line, and re-scan the models tree
// whenever an artifact lands so its row becomes selectable straight away.
void MainWindow::poll_downloads() {
    const auto st = fetcher_.snapshot();
    if (st.finished_since_last) {
        catalog_.refresh_presence();
        controls_->refresh_availability();
    }
    if (engine_.active()) return;  // a running benchmark owns the status line

    if (st.running) {
        controls_->set_status(
            "Downloading models — " + std::to_string(st.done) + " of " +
            std::to_string(st.total) + "\n<small>" +
            Glib::Markup::escape_text(st.current) + "</small>");
    } else if (!st.errors.empty()) {
        controls_->set_status(
            "Idle — " + std::to_string(st.errors.size()) +
            " model(s) could not be downloaded; see the log.");
    } else {
        controls_->set_status("Idle — select a model and press Start.");
    }
}

Gtk::Expander& MainWindow::make_section(const char* title, Gtk::Widget& content,
                                        bool expanded) {
    auto* exp = Gtk::make_managed<Gtk::Expander>();
    auto* lbl = Gtk::make_managed<Gtk::Label>();
    lbl->set_markup(std::string("<b>") + title + "</b>");
    exp->set_label_widget(*lbl);
    exp->set_expanded(expanded);
    exp->set_margin_top(4);
    // vexpand has to *track* the expanded state, not merely be sampled once: an
    // expanded section shares the column's vertical space, a collapsed one must
    // claim none. Seeding it here and binding below keeps a section that starts
    // collapsed able to grow when the user opens it — the bug this replaced.
    exp->set_vexpand(expanded);
    exp->property_expanded().signal_changed().connect(
        [exp] { exp->set_vexpand(exp->get_expanded()); });
    auto* pad = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    pad->set_margin_top(6);
    pad->set_margin_start(4);
    pad->set_vexpand(true);
    pad->append(content);
    exp->set_child(*pad);
    return *exp;
}

std::vector<Gdk::RGBA> MainWindow::colors_for(
    const std::vector<MetricInfo>& metrics) const {
    std::vector<Gdk::RGBA> out;
    out.reserve(metrics.size());
    for (const auto& m : metrics) {
        int d = (m.device >= 0 &&
                 m.device < static_cast<int>(device_palette_.size()))
                    ? m.device
                    : 0;
        out.push_back(device_palette_.empty() ? util::rgb(0.5, 0.5, 0.5)
                                              : device_palette_[d]);
    }
    return out;
}

MainWindow::AccelSection MainWindow::build_accel_section(
    std::function<std::string(double)> value_fmt, double min_axis_max) {
    AccelSection sec;

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_vexpand(true);

    auto* graph = Gtk::make_managed<GraphArea>(kHistory, kSpanSeconds);
    graph->set_percent_mode(false);
    graph->set_min_axis_max(min_axis_max);
    graph->set_value_formatter(value_fmt);
    std::vector<Gdk::RGBA> colors(accel_color_, accel_color_ + kAccelCount);
    graph->set_series(colors);
    box->append(*graph);
    sec.graph = graph;

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(3);
    grid->set_column_spacing(20);
    grid->set_halign(Gtk::Align::START);

    for (int i = 0; i < kAccelCount; ++i) {
        auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

        const Gdk::RGBA c = accel_color_[i];
        auto* swatch = Gtk::make_managed<Gtk::DrawingArea>();
        swatch->set_content_width(16);
        swatch->set_content_height(12);
        swatch->set_valign(Gtk::Align::CENTER);
        swatch->set_draw_func([c](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            cr->rectangle(0.5, 0.5, w - 1, h - 1);
            cr->set_source_rgb(c.get_red(), c.get_green(), c.get_blue());
            cr->fill_preserve();
            Gdk::RGBA ink = util::neutral::ink();
            cr->set_source_rgba(ink.get_red(), ink.get_green(), ink.get_blue(), 0.3);
            cr->set_line_width(1.0);
            cr->stroke();
        });
        cell->append(*swatch);

        auto* name = Gtk::make_managed<Gtk::Label>(accel_name(accel_at(i)));
        name->set_xalign(0.0);
        name->set_width_chars(8);
        cell->append(*name);

        auto* val = Gtk::make_managed<Gtk::Label>("0");
        val->set_xalign(1.0);
        val->set_width_chars(7);
        sec.value[i] = val;
        cell->append(*val);

        // All the cards on one row (wrapping past four), so the legend is a
        // single compact strip under each graph rather than a tall block.
        grid->attach(*cell, i % 4, i / 4, 1, 1);
    }
    box->append(*grid);
    sec.root = box;
    return sec;
}

Gtk::Widget& MainWindow::build_metric_section(
    const std::vector<MetricInfo>& metrics, const std::vector<Gdk::RGBA>& colors,
    bool temp_axis, std::function<std::string(double)> value_fmt,
    GraphArea*& graph_out, std::vector<Gtk::Label*>& value_labels_out,
    const char* empty_note, std::vector<AggEntry>* agg_out, double min_axis_max) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_vexpand(true);

    const int n = static_cast<int>(metrics.size());

    auto* graph = Gtk::make_managed<GraphArea>(kHistory, kSpanSeconds);
    graph->set_percent_mode(false);
    if (temp_axis) {
        graph->set_fixed_max(kTempAxisMax);
        graph->set_value_formatter(fmt_temp_axis);
    } else {
        // Auto-scaling with a floor. The formatter passed in decides the unit,
        // so this same branch serves both the power and the frequency graphs.
        graph->set_min_axis_max(min_axis_max);
        graph->set_value_formatter(value_fmt);
    }
    graph->set_series(std::vector<Gdk::RGBA>(colors.begin(), colors.begin() + n));
    box->append(*graph);
    graph_out = graph;

    if (n == 0) {
        auto* note = Gtk::make_managed<Gtk::Label>(empty_note);
        note->set_xalign(0.0);
        note->set_wrap(true);
        note->add_css_class("dim-label");
        note->set_margin_top(4);
        box->append(*note);
        return *box;
    }

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(3);
    grid->set_column_spacing(20);
    grid->set_halign(Gtk::Align::START);

    value_labels_out.assign(n, nullptr);
    int i = 0, row = 0;
    while (i < n) {
        const int dev = metrics[i].device;
        const std::string& dname = metrics[i].device_name;
        const std::string& bdf = metrics[i].bdf;

        auto* dn = Gtk::make_managed<Gtk::Label>();
        std::string prefix = bdf.empty() ? "" : bdf + "  ";
        dn->set_markup(prefix + "<b>" + Glib::Markup::escape_text(dname) + "</b>");
        dn->set_xalign(0.0);
        dn->set_margin_end(6);
        grid->attach(*dn, 0, row, 1, 1);

        int col = 1;
        const int agg_start = i;
        Gtk::Label* agg_label = nullptr;
        if (agg_out) {
            agg_label = Gtk::make_managed<Gtk::Label>("—");
            agg_label->set_xalign(0.0);
            agg_label->set_margin_end(6);
            agg_label->add_css_class("dim-label");
            grid->attach(*agg_label, col, row, 1, 1);
            ++col;
        }

        while (i < n && metrics[i].device == dev) {
            auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

            Gdk::RGBA c = colors[i];
            auto* swatch = Gtk::make_managed<Gtk::DrawingArea>();
            swatch->set_content_width(16);
            swatch->set_content_height(12);
            swatch->set_valign(Gtk::Align::CENTER);
            swatch->set_draw_func(
                [c](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                    cr->rectangle(0.5, 0.5, w - 1, h - 1);
                    cr->set_source_rgb(c.get_red(), c.get_green(), c.get_blue());
                    cr->fill_preserve();
                    Gdk::RGBA ink = util::neutral::ink();
                    cr->set_source_rgba(ink.get_red(), ink.get_green(),
                                        ink.get_blue(), 0.3);
                    cr->set_line_width(1.0);
                    cr->stroke();
                });
            cell->append(*swatch);

            std::string lbl = metrics[i].label;
            if (lbl.rfind(dname + " ", 0) == 0) lbl = lbl.substr(dname.size() + 1);
            auto* name = Gtk::make_managed<Gtk::Label>(lbl);
            name->set_xalign(0.0);
            name->set_width_chars(4);
            cell->append(*name);

            auto* val = Gtk::make_managed<Gtk::Label>("—");
            val->set_xalign(1.0);
            val->set_width_chars(6);
            value_labels_out[i] = val;
            cell->append(*val);

            grid->attach(*cell, col, row, 1, 1);
            ++col;
            ++i;
        }
        if (agg_out) agg_out->push_back({agg_label, agg_start, i - agg_start});
        ++row;
    }
    box->append(*grid);
    return *box;
}

void MainWindow::on_start_stop() {
    if (engine_.active()) {
        engine_.stop();
        controls_->set_running(false);
        controls_->set_status("Stopped.");
        return;
    }

    const auto items = controls_->selection();
    if (items.empty()) {
        controls_->set_status(
            "Nothing to run — select a model and enable at least one "
            "accelerator that has it compiled.");
        return;
    }

    // Deliberately no graph reset: the traces are a continuous record across
    // start/stop, so successive runs can be compared against each other on the
    // same axis instead of each one wiping the last.

    engine_.start(items, controls_->target_fps(), controls_->api_mode());
    controls_->set_running(true);

    std::string on;
    for (const auto& it : items) {
        if (!on.empty()) on += ", ";
        on += accel_name(it.accel);
    }
    const double t = controls_->target_fps();
    const ApiMode mode = controls_->api_mode();
    run_status_ = "Running <b>" + Glib::Markup::escape_text(items[0].label) +
                  "</b> on " + Glib::Markup::escape_text(on) + " · " +
                  (t > 0 ? fmt_fps(t) + " fps target" : "max speed") + " · " +
                  api_mode_name(mode);

    // A card whose SDK has no native call for this mode must still say so —
    // this used to ride in the frame-rate legend, which no longer carries text.
    // Without it the chart would silently imply a parity the APIs don't offer.
    for (const auto& it : items) {
        const char* emu = api_mode_note(it.accel, mode);
        if (!*emu) continue;
        run_status_ += "\n<small>" + Glib::Markup::escape_text(accel_name(it.accel)) +
                       ": " + Glib::Markup::escape_text(emu) + "</small>";
    }
    controls_->set_status(run_status_);
}

bool MainWindow::on_tick() {
    const std::int64_t now = g_get_monotonic_time();
    const double dt = std::max(1e-3, (now - last_time_us_) / 1e6);
    last_time_us_ = now;

    poll_downloads();
    probes_.poll();

    // Watts per accelerator, for the efficiency figures. NaN = no sensor.
    double watts[kAccelCount];
    for (int i = 0; i < kAccelCount; ++i) {
        watts[i] = probes_.power_for_device(accel_name(accel_at(i)));
    }

    engine_.sample(dt, watts);

    // Fold the engine's per-run series onto the fixed per-accelerator series.
    // An accelerator with no active run stays at 0 — an idle card really is
    // doing zero frames per second.
    std::vector<double> fps(kAccelCount, 0.0);
    std::vector<double> eff(kAccelCount, 0.0);
    std::vector<double> jpf(kAccelCount, 0.0);  // millijoules per frame
    // Failures used to live in the frame-rate legend; that text is gone, so
    // collect them for the status line instead. A card silently sitting at 0
    // with no explanation is how a broken model looks exactly like a slow one.
    std::vector<std::string> problems;

    for (const auto& s : engine_.series()) {
        const int a = accel_index(s.accel);
        fps[a] = s.fps;
        eff[a] = s.fps_per_w;
        jpf[a] = s.joules_per_frame * 1000.0;  // SI joules -> mJ for display
        if (s.state == BenchEngine::State::Failed) {
            problems.push_back(std::string(accel_name(s.accel)) + ": " + s.error);
        } else if (s.state == BenchEngine::State::Loading) {
            problems.push_back(std::string(accel_name(s.accel)) + ": loading…");
        }
    }

    if (engine_.active()) {
        std::string text = run_status_;
        for (const auto& p : problems) {
            text += "\n<small>" + Glib::Markup::escape_text(p) + "</small>";
        }
        controls_->set_status(text);
    }

    fps_.graph->push(fps);
    eff_.graph->push(eff);
    energy_.graph->push(jpf);
    for (int i = 0; i < kAccelCount; ++i) {
        fps_.value[i]->set_text(fmt_fps(fps[i]));
        eff_.value[i]->set_text(fmt_eff(eff[i]));
        energy_.value[i]->set_text(fmt_eff(jpf[i]));
    }

    const auto& tv = probes_.temp_values();
    if (!tv.empty() && static_cast<int>(tv.size()) == temp_graph_->series_count()) {
        temp_graph_->push(tv);
        for (size_t i = 0; i < temp_values_.size() && i < tv.size(); ++i)
            temp_values_[i]->set_text(fmt_temp(tv[i]));
        for (const auto& a : temp_avg_labels_) {
            double sum = 0.0;
            int cnt = 0;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(tv.size()); ++k) {
                if (!std::isnan(tv[k])) { sum += tv[k]; ++cnt; }
            }
            a.label->set_text(cnt ? "avg " + fmt_temp(sum / cnt) : "avg —");
        }
    }

    const auto& fv = probes_.freq_values();
    if (!fv.empty() && static_cast<int>(fv.size()) == freq_graph_->series_count()) {
        freq_graph_->push(fv);
        for (size_t i = 0; i < freq_values_.size() && i < fv.size(); ++i)
            freq_values_[i]->set_text(fmt_freq(fv[i]));
        for (const auto& a : freq_avg_labels_) {
            double sum = 0.0;
            int cnt = 0;
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(fv.size()); ++k) {
                if (!std::isnan(fv[k])) { sum += fv[k]; ++cnt; }
            }
            a.label->set_text(cnt ? "avg " + fmt_freq(sum / cnt) : "avg —");
        }
    }

    const auto& pv = probes_.power_values();
    if (!pv.empty() && static_cast<int>(pv.size()) == power_graph_->series_count()) {
        power_graph_->push(pv);
        for (size_t i = 0; i < power_values_.size() && i < pv.size(); ++i)
            power_values_[i]->set_text(fmt_power(pv[i]));
        for (const auto& a : power_max_labels_) {
            double best = std::nan("");
            for (int k = a.start;
                 k < a.start + a.count && k < static_cast<int>(pv.size()); ++k) {
                if (!std::isnan(pv[k]) && (std::isnan(best) || pv[k] > best))
                    best = pv[k];
            }
            a.label->set_text(std::isnan(best) ? "max —" : "max " + fmt_power(best));
        }
    }

    return true;
}

void MainWindow::on_about() {
    if (!about_ready_) {
        about_ready_ = true;
        about_dialog_.add_css_class("mb-about");
        about_dialog_.set_transient_for(*this);
        about_dialog_.set_modal(true);
        about_dialog_.set_hide_on_close(true);
        about_dialog_.set_program_name(kTitle);
        about_dialog_.set_version("0.01");
        about_dialog_.set_comments(
            "Benchmarks edge-AI NPUs and charts the frame rate, power and "
            "energy efficiency they achieve.");
        about_dialog_.set_copyright("© 2026 Mario Bergeron");
        about_dialog_.set_license_type(Gtk::License::APACHE_2_0);
        about_dialog_.set_website("https://mariobergeron.com");
        about_dialog_.set_website_label("mariobergeron.com");
        try {
            about_dialog_.set_logo(Gdk::Texture::create_from_resource(
                "/com/mariobergeron/mbbenchmark/M_benchmarking.png"));
        } catch (const Glib::Error& e) {
            g_warning("about logo: %s", e.what());
        }
    }
    about_dialog_.set_visible(true);
}
