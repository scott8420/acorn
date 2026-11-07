#pragma once
#include <gtkmm/window.h>

// ─────────────────────────────────────────────────────────────────────────────
// AboutWindow -- the DIALOG-LIFETIME exemplar (a stone the README backlog named:
// "hide-on-close vs transient dialog lifetime"). It carries two paradigms at
// once so a single running window exercises both:
//
//   1. Custom window, NOT Gtk::AboutDialog. That stock dialog is DEPRECATED in
//      GTK4 -- a custom Gtk::Window is the forward path, and it's what you'd
//      reach for anyway the moment the About box wants app-specific styling.
//
//   2. Hide-on-close SINGLETON lifetime (CANON: "Lifetime shape is design").
//      Shell holds one, built once and re-presented; the X hides it, it never
//      destructs mid-session, so its named children never re-register -- no
//      self-rebuild registry collision, no leak. This is the born-modern shape;
//      the older heap-allocated self-deleting dialog is the legacy-rescue shape,
//      not the default.
//
// The hero logo is a symbolic icon resolved from the compiled-in gresource
// bundle (the RESOURCE-PIPELINE paradigm -- App::on_activate registers it), so
// the window also exercises resource resolution + theme recolour.
//
// It is deliberately minimal: this is a paradigm seed, not a product About box,
// so there's no branded chrome (credits, licence, links) -- just enough to prove
// the lifetime and the resource icon. See docs/resources-and-dialog-lifetime.md.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class AboutWindow : public Gtk::Window {
public:
    explicit AboutWindow(Gtk::Window& parent);
    ~AboutWindow() override;
};

}  // namespace acorn
