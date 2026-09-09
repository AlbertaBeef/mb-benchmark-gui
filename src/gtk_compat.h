// One source tree, two toolkits: gtkmm-4.0 and gtkmm-3.0 (3.24).
//
// Shared verbatim with ../mb-powermon-gui — byte-identical, like GraphArea.{h,cpp}
// and util.h. `cmp` it after any change; a `cp` in either direction is the
// correct way to port one.
//
// It keys off gtkmm's own GTKMM_MAJOR_VERSION rather than a CMake define,
// deliberately: the sibling project stays on gtkmm-4 and defines no such
// option, and this file has to compile there unchanged.
//
// The shape to preserve: **under gtkmm-4 every helper below is a one-line
// forwarder to the identical call.** That is what keeps the gtkmm-4 hosts safe —
// a reviewer can confirm the GTK4 build is unchanged by reading this file alone,
// without auditing the ~200 call sites that use it. Keep it that way: never let
// the gtkmm-4 branch of a helper do anything the old call site did not.
//
// Why helpers rather than #if at the call sites: there are ~200 differences
// across MainWindow.cpp and ControlPanel.cpp, and inline #if blocks would make
// both files unreadable. There is no #if outside this header.
#pragma once

#include <gtkmm/widget.h>          // pulls gtkmmconfig.h -> GTKMM_MAJOR_VERSION

#if GTKMM_MAJOR_VERSION >= 4
#  define MB_GTKMM4 1
#else
#  define MB_GTKMM4 0
#endif

// Cross-check against what CMake selected. Catches a stale build directory
// pointing at the other toolkit's headers, which otherwise fails much later
// with a wall of unrelated errors.
#if defined(MB_GTKMM_MAJOR) && (MB_GTKMM_MAJOR != GTKMM_MAJOR_VERSION)
#  error "gtk_compat.h: CMake selected a different gtkmm than is on the include path"
#endif

#include <gtkmm/aboutdialog.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/paned.h>
#include <gtkmm/stylecontext.h>
#include <pangomm/layout.h>
#include <cairomm/context.h>

#if MB_GTKMM4
#  include <gdkmm/display.h>
#  include <gdkmm/texture.h>
#else
#  include <gdkmm/pixbuf.h>
#  include <gdkmm/screen.h>
#  include <gtkmm/radiobutton.h>
#endif

#include <functional>
#include <utility>

namespace gtkc {

// ---------------------------------------------------------------------------
// Enumerators. Scoped enums in gtkmm-4; unscoped *and differently spelled* in
// gtkmm-3 (Gtk::ORIENTATION_VERTICAL, not Gtk::Orientation::VERTICAL), so C++11
// qualified access does not bridge them and an alias is needed either way.
// ---------------------------------------------------------------------------
namespace Orientation {
#if MB_GTKMM4
inline constexpr Gtk::Orientation HORIZONTAL = Gtk::Orientation::HORIZONTAL;
inline constexpr Gtk::Orientation VERTICAL   = Gtk::Orientation::VERTICAL;
#else
inline constexpr Gtk::Orientation HORIZONTAL = Gtk::ORIENTATION_HORIZONTAL;
inline constexpr Gtk::Orientation VERTICAL   = Gtk::ORIENTATION_VERTICAL;
#endif
}  // namespace Orientation

namespace Align {
#if MB_GTKMM4
inline constexpr Gtk::Align START  = Gtk::Align::START;
inline constexpr Gtk::Align END    = Gtk::Align::END;
inline constexpr Gtk::Align CENTER = Gtk::Align::CENTER;
inline constexpr Gtk::Align FILL   = Gtk::Align::FILL;
#else
inline constexpr Gtk::Align START  = Gtk::ALIGN_START;
inline constexpr Gtk::Align END    = Gtk::ALIGN_END;
inline constexpr Gtk::Align CENTER = Gtk::ALIGN_CENTER;
inline constexpr Gtk::Align FILL   = Gtk::ALIGN_FILL;
#endif
}  // namespace Align

namespace PolicyType {
#if MB_GTKMM4
inline constexpr Gtk::PolicyType NEVER     = Gtk::PolicyType::NEVER;
inline constexpr Gtk::PolicyType AUTOMATIC = Gtk::PolicyType::AUTOMATIC;
inline constexpr Gtk::PolicyType ALWAYS    = Gtk::PolicyType::ALWAYS;
#else
inline constexpr Gtk::PolicyType NEVER     = Gtk::POLICY_NEVER;
inline constexpr Gtk::PolicyType AUTOMATIC = Gtk::POLICY_AUTOMATIC;
inline constexpr Gtk::PolicyType ALWAYS    = Gtk::POLICY_ALWAYS;
#endif
}  // namespace PolicyType

namespace SelectionMode {
#if MB_GTKMM4
inline constexpr Gtk::SelectionMode NONE   = Gtk::SelectionMode::NONE;
inline constexpr Gtk::SelectionMode SINGLE = Gtk::SelectionMode::SINGLE;
inline constexpr Gtk::SelectionMode BROWSE = Gtk::SelectionMode::BROWSE;
#else
inline constexpr Gtk::SelectionMode NONE   = Gtk::SELECTION_NONE;
inline constexpr Gtk::SelectionMode SINGLE = Gtk::SELECTION_SINGLE;
inline constexpr Gtk::SelectionMode BROWSE = Gtk::SELECTION_BROWSE;
#endif
}  // namespace SelectionMode

namespace EllipsizeMode {
#if MB_GTKMM4
inline constexpr Pango::EllipsizeMode END = Pango::EllipsizeMode::END;
#else
inline constexpr Pango::EllipsizeMode END = Pango::ELLIPSIZE_END;
#endif
}  // namespace EllipsizeMode

// Cairo's toy font API: scoped enums in cairomm-1.16 (gtkmm-4), unscoped in
// cairomm-1.0 (gtkmm-3).
#if MB_GTKMM4
inline constexpr auto FONT_SLANT_NORMAL  = Cairo::ToyFontFace::Slant::NORMAL;
inline constexpr auto FONT_WEIGHT_NORMAL = Cairo::ToyFontFace::Weight::NORMAL;
#else
inline constexpr auto FONT_SLANT_NORMAL  = Cairo::FONT_SLANT_NORMAL;
inline constexpr auto FONT_WEIGHT_NORMAL = Cairo::FONT_WEIGHT_NORMAL;
#endif

// ---------------------------------------------------------------------------
// Box. append() is called ~74 times but Gtk::Box is only *named* 23 times, so a
// thin subclass costs 23 edits where a free function would cost 74 — and every
// `->append(x)` stays byte-identical.
// ---------------------------------------------------------------------------
#if MB_GTKMM4
using Box = Gtk::Box;
#else
class Box : public Gtk::Box {
public:
    using Gtk::Box::Box;

    // GTK4's append() adds at the end and defers to the child's own
    // hexpand/vexpand for extra space. GTK3's GtkBox consults
    // gtk_widget_compute_expand() in addition to the pack-time `expand` flag,
    // and re-reads it on every allocation — so expand=false is the faithful
    // translation, *not* a snapshot of the child's flags here.
    //
    // That distinction is load-bearing. make_section() in MainWindow.cpp binds a
    // section's vexpand to its Expander's `expanded` property, so the value
    // changes long after packing; a snapshot would freeze it and a collapsed
    // section would keep its space. Verified on gtkmm 3.24.2 by driving
    // gtk_box_size_allocate() directly: with expand=false the vexpand child took
    // 510/600 px and 710/800 px, and swapping vexpand between two children moved
    // the space with it.
    void append(Gtk::Widget& child) {
        pack_start(child, /*expand=*/false, /*fill=*/true, /*padding=*/0);
    }
};
#endif

// ---------------------------------------------------------------------------
// Radio grouping. GTK4 deleted Gtk::RadioButton and made a grouped CheckButton
// the radio; GTK3 keeps the separate class. join_group() has the same
// "join this other button" shape as GTK4's set_group(), so the two line up.
//
// Ordering note for callers: a lone GTK3 RadioButton is active in its own
// group, so the existing join-then-set_active(true) order in ControlPanel.cpp
// is correct and must not be reversed.
// ---------------------------------------------------------------------------
#if MB_GTKMM4
using RadioButton = Gtk::CheckButton;
inline void join_radio_group(RadioButton& b, RadioButton& group_source) {
    b.set_group(group_source);
}
#else
using RadioButton = Gtk::RadioButton;
inline void join_radio_group(RadioButton& b, RadioButton& group_source) {
    b.join_group(group_source);
}
#endif

// ---------------------------------------------------------------------------
// DrawingArea. A free function, so GraphArea keeps deriving from
// Gtk::DrawingArea with its private draw(cr, w, h) signature untouched — which
// is what lets GraphArea.h stay byte-identical with the sibling.
// ---------------------------------------------------------------------------
using DrawFunc =
    std::function<void(const Cairo::RefPtr<Cairo::Context>&, int, int)>;

inline void set_draw_func(Gtk::DrawingArea& area, DrawFunc fn) {
#if MB_GTKMM4
    area.set_draw_func(std::move(fn));
#else
    // GTK3's ::draw handler is given neither dimension and must ask the widget.
    // The connection is owned by that widget's own signal, so `p` cannot outlive
    // what it points at.
    Gtk::DrawingArea* p = &area;
    area.signal_draw().connect(
        [p, fn = std::move(fn)](const Cairo::RefPtr<Cairo::Context>& cr) -> bool {
            fn(cr, p->get_allocated_width(), p->get_allocated_height());
            return true;   // handled; do not run the default handler
        });
#endif
}

// GTK4 sizes a DrawingArea through its own content properties; GTK3 uses the
// ordinary widget size request. Pass -1 for "unset".
inline void set_content_size(Gtk::DrawingArea& area, int w, int h) {
#if MB_GTKMM4
    if (w >= 0) area.set_content_width(w);
    if (h >= 0) area.set_content_height(h);
#else
    area.set_size_request(w, h);
#endif
}

// ---------------------------------------------------------------------------
// Containers. GTK4 gave each single-child container its own set_child();
// GTK3 has one Gtk::Container::add(). Templated because GTK4 has no common base
// carrying set_child().
// ---------------------------------------------------------------------------
template <typename Parent>
inline void set_child(Parent& parent, Gtk::Widget& child) {
#if MB_GTKMM4
    parent.set_child(child);
#else
    parent.add(child);
#endif
}

inline void list_append(Gtk::ListBox& list, Gtk::Widget& row) {
#if MB_GTKMM4
    list.append(row);
#else
    list.add(row);
#endif
}

// GTK4 split pack1/pack2 into a child setter plus two property setters. Folding
// them back into one call keeps the resize/shrink choice stated once, at the
// call site, where the reasoning about it lives.
inline void paned_pack1(Gtk::Paned& paned, Gtk::Widget& child,
                        bool resize, bool shrink) {
#if MB_GTKMM4
    paned.set_start_child(child);
    paned.set_resize_start_child(resize);
    paned.set_shrink_start_child(shrink);
#else
    paned.pack1(child, resize, shrink);
#endif
}

inline void paned_pack2(Gtk::Paned& paned, Gtk::Widget& child,
                        bool resize, bool shrink) {
#if MB_GTKMM4
    paned.set_end_child(child);
    paned.set_resize_end_child(resize);
    paned.set_shrink_end_child(shrink);
#else
    paned.pack2(child, resize, shrink);
#endif
}

// ---------------------------------------------------------------------------
// HeaderBar
// ---------------------------------------------------------------------------
inline void header_set_title_widget(Gtk::HeaderBar& bar, Gtk::Widget& title) {
#if MB_GTKMM4
    bar.set_title_widget(title);
#else
    bar.set_custom_title(title);
#endif
}

// GTK3's HeaderBar defaults show-close-button to FALSE where GTK4 shows the
// window controls by default. Without this a set_titlebar() window comes up
// with no close, minimize or maximize button at all.
inline void header_show_window_controls(Gtk::HeaderBar& bar) {
#if MB_GTKMM4
    (void)bar;   // already the default
#else
    bar.set_show_close_button(true);
#endif
}

// ---------------------------------------------------------------------------
// CSS
// ---------------------------------------------------------------------------
// gtkmm-3's load_from_data() *throws* on a parse error where gtkmm-4 only warns.
// Swallowing it keeps a stylesheet the gtkmm-4 hosts tolerate from aborting the
// gtkmm-3 build at startup.
inline void load_css(const Glib::RefPtr<Gtk::CssProvider>& provider,
                     const Glib::ustring& data) {
#if MB_GTKMM4
    provider->load_from_data(data);
#else
    try {
        provider->load_from_data(data);
    } catch (const Glib::Error& e) {
        g_warning("CSS: %s", e.what().c_str());
    }
#endif
}

inline void add_css_provider_for_default(
    const Glib::RefPtr<Gtk::CssProvider>& provider, guint priority) {
#if MB_GTKMM4
    Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(),
                                                provider, priority);
#else
    Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(),
                                               provider, priority);
#endif
}

inline void add_css_class(Gtk::Widget& w, const char* name) {
#if MB_GTKMM4
    w.add_css_class(name);
#else
    w.get_style_context()->add_class(name);
#endif
}

inline void remove_css_class(Gtk::Widget& w, const char* name) {
#if MB_GTKMM4
    w.remove_css_class(name);
#else
    w.get_style_context()->remove_class(name);
#endif
}

// ---------------------------------------------------------------------------
// Widget odds and ends
// ---------------------------------------------------------------------------
inline void set_margin(Gtk::Widget& w, int margin) {
#if MB_GTKMM4
    w.set_margin(margin);
#else
    w.set_margin_top(margin);
    w.set_margin_bottom(margin);
    w.set_margin_start(margin);
    w.set_margin_end(margin);
#endif
}

inline void label_set_wrap(Gtk::Label& l, bool wrap = true) {
#if MB_GTKMM4
    l.set_wrap(wrap);
#else
    l.set_line_wrap(wrap);
#endif
}

inline void button_set_icon_name(Gtk::Button& b, const char* icon_name) {
#if MB_GTKMM4
    b.set_icon_name(icon_name);
#else
    b.set_image(*Gtk::make_managed<Gtk::Image>(icon_name, Gtk::ICON_SIZE_BUTTON));
    b.set_always_show_image(true);
#endif
}

// ---------------------------------------------------------------------------
// AboutDialog
// ---------------------------------------------------------------------------
// GTK4 carries images as Gdk::Texture (a Gdk::Paintable); GTK3 as Gdk::Pixbuf.
// Throws Glib::Error in both, so an existing try/catch at the call site stands.
inline void about_set_logo_from_resource(Gtk::AboutDialog& d,
                                         const char* resource_path) {
#if MB_GTKMM4
    d.set_logo(Gdk::Texture::create_from_resource(resource_path));
#else
    d.set_logo(Gdk::Pixbuf::create_from_resource(resource_path));
#endif
}

// Apache-2.0 is not settable by enum on gtkmm-3. GTK 3.24.20 declares
// GTK_LICENSE_APACHE_2_0 in gtkaboutdialog.h, but the shipped library's
// gtk_about_dialog_set_license_type() still asserts
// `license_type <= GTK_LICENSE_AGPL_3_0_ONLY` — so casting to it compiles,
// then fails at runtime with a Gtk-CRITICAL and leaves the licence unset
// (observed on Ubuntu 20.04's 3.24.20). Name it as custom text instead.
inline void about_set_license_apache2(Gtk::AboutDialog& d) {
#if MB_GTKMM4
    d.set_license_type(Gtk::License::APACHE_2_0);
#else
    d.set_license("Apache License 2.0");
    d.set_license_type(Gtk::LICENSE_CUSTOM);
#endif
}

inline void about_dialog_hide_on_close(Gtk::AboutDialog& d) {
#if MB_GTKMM4
    d.set_hide_on_close(true);
#else
    // GTK3's AboutDialog is a Gtk::Dialog, and gtkmm 3.24.2 has no
    // set_hide_on_close(). Its Close button emits ::response, and the
    // window-manager button emits ::delete-event whose *default handler
    // destroys the widget*. The dialog is a MainWindow member, so letting that
    // run would leave the second About press driving freed memory. Returning
    // true from the delete-event handler suppresses the default destroy.
    d.signal_response().connect([&d](int) { d.hide(); });
    d.signal_delete_event().connect(
        [&d](GdkEventAny*) {
            d.hide();
            return true;
        },
        false);
#endif
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// GTK4 widgets are visible by default; GTK3 widgets are not. Call this once the
// tree is built and *before* any code that deliberately hides part of it — in
// MainWindow that is apply_graph_filter(), which hides the legend entries for
// cards this build has no backend for.
//
// show_all_children(), not show_all(): the latter would map the toplevel from
// inside the constructor, before Gtk::Application presents it.
template <typename W>
inline void show_all_children(W& w) {
#if MB_GTKMM4
    (void)w;   // nothing to do; GTK4 children start visible
#else
    w.show_all_children();
#endif
}

// GTK4's make_window_and_run<W>() constructs the window, registers it, runs, and
// destroys it. gtkmm-3 has no equivalent template, so the whole statement
// differs rather than one token — hence a function template rather than an alias.
template <typename W, typename... Args>
inline int run_window(const Glib::RefPtr<Gtk::Application>& app,
                      int argc, char** argv, Args&&... args) {
#if MB_GTKMM4
    return app->make_window_and_run<W>(argc, argv, std::forward<Args>(args)...);
#else
    W window(std::forward<Args>(args)...);
    return app->run(window, argc, argv);
#endif
}

}  // namespace gtkc
