#include "ShortcutsDialog.hpp"
#include "core/Shortcuts.hpp"
#include "Registry.hpp"

#include <gtkmm/label.h>
#include <gtkmm/separator.h>

namespace acorn {

ShortcutsDialog::ShortcutsDialog() {
    set_name("shell.shortcuts");
    add_css_class("acorn");          // s020a: the skin on every window
    add_css_class("acorn-dialog");
    registry::add("shell.shortcuts", this);

    set_title("Keyboard Shortcuts");
    set_modal(false);
    set_resizable(true);
    set_default_size(520, 640);
    set_hide_on_close(true);   // built once by MainWindow, reused on every open

    // Walk the pure registry, emitting a heading whenever the section changes.
    // The registry is authored section (A-Z) -> row, so a linear walk groups
    // correctly (the selftest guards that ordering). This is the same list
    // Controller_bindings wires accelerators from.
    m_grid.set_margin(12);
    m_grid.set_column_spacing(16);
    m_grid.set_row_spacing(0);
    m_grid.set_column_homogeneous(false);

    int r = 0;
    std::string cur_section;
    bool first = true;
    for (const auto& s : core::shortcut_registry()) {
        if (first || s.section != cur_section) {
            if (!first) r = add_spacer(m_grid, r);
            r = add_heading(m_grid, s.section, r);
            cur_section = s.section;
            first = false;
        }
        r = add_row(m_grid, s.display_keys(), s.description, r);
    }

    m_scroll.set_child(m_grid);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_expand(true);

    m_btn_close.set_name("shell.shortcuts.close");
    m_btn_close.set_halign(Gtk::Align::END);
    m_btn_close.set_margin(8);
    m_btn_close.signal_clicked().connect([this]() { set_visible(false); });

    m_root.append(m_scroll);
    m_root.append(m_btn_close);
    set_child(m_root);
}

ShortcutsDialog::~ShortcutsDialog() {
    registry::remove(this);
}

void ShortcutsDialog::show(Gtk::Window& parent) {
    set_transient_for(parent);
    present();
}

int ShortcutsDialog::add_heading(Gtk::Grid& grid, const std::string& title, int row) {
    auto* lbl = Gtk::make_managed<Gtk::Label>(title);
    lbl->set_halign(Gtk::Align::START);
    lbl->set_margin_top(12);
    lbl->set_margin_bottom(2);
    lbl->add_css_class("heading");
    grid.attach(*lbl, 0, row, 2, 1);
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_bottom(4);
    grid.attach(*sep, 0, row + 1, 2, 1);
    return row + 2;
}

int ShortcutsDialog::add_row(Gtk::Grid& grid, const std::string& keys,
                             const std::string& desc, int row) {
    auto* key_lbl = Gtk::make_managed<Gtk::Label>(keys);
    key_lbl->set_halign(Gtk::Align::START);
    key_lbl->set_valign(Gtk::Align::START);
    key_lbl->set_margin_start(8);
    key_lbl->set_margin_end(16);
    key_lbl->set_margin_top(2);
    key_lbl->set_margin_bottom(2);
    key_lbl->add_css_class("monospace");

    auto* desc_lbl = Gtk::make_managed<Gtk::Label>(desc);
    desc_lbl->set_halign(Gtk::Align::START);
    desc_lbl->set_xalign(0.0f);
    desc_lbl->set_wrap(true);
    desc_lbl->set_margin_top(2);
    desc_lbl->set_margin_bottom(2);

    grid.attach(*key_lbl, 0, row);
    grid.attach(*desc_lbl, 1, row);
    return row + 1;
}

int ShortcutsDialog::add_spacer(Gtk::Grid& grid, int row) {
    auto* lbl = Gtk::make_managed<Gtk::Label>("");
    lbl->set_margin_top(4);
    grid.attach(*lbl, 0, row, 2, 1);
    return row + 1;
}

}  // namespace acorn
