#pragma once
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>

#include <string>

namespace acorn {

// ShortcutsDialog -- the SHORTCUT-REGISTRY paradigm's GTK consumer, and the
// second instance of the dialog-lifetime stone AboutWindow names.
//
// Two paradigms in one window:
//
//   1. It renders by WALKING core::shortcut_registry() -- it hand-lists nothing.
//      The same list App wires accelerators from, so a key and its advertised
//      row cannot drift. That is the whole point of the registry: declare a
//      shortcut once, and both consumers read it.
//
//   2. Custom Gtk::Window, NOT GtkShortcutsWindow -- the stock widget is
//      deprecated (GTK 4.18, removed in GTK 5, along with ShortcutsSection/
//      Group/ShortcutLabel and set_help_overlay). The same call AboutWindow
//      makes about Gtk::AboutDialog, for the same reason: a custom window is
//      the forward path.
//
// Hide-on-close singleton (CANON: "Lifetime shape is design"), like AboutWindow:
// Shell builds it once and re-presents it, so its named children never
// re-register.
class ShortcutsDialog : public Gtk::Window {
public:
    ShortcutsDialog();
    ~ShortcutsDialog() override;
    void show(Gtk::Window& parent);

private:
    // Grid helpers (each returns the next free row).
    int add_heading(Gtk::Grid& grid, const std::string& title, int row);
    int add_row(Gtk::Grid& grid, const std::string& keys, const std::string& desc, int row);
    int add_spacer(Gtk::Grid& grid, int row);

    Gtk::Box            m_root{Gtk::Orientation::VERTICAL};
    Gtk::ScrolledWindow m_scroll;
    Gtk::Grid           m_grid;
    Gtk::Button         m_btn_close{"Close"};
};

}  // namespace acorn
