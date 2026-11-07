#include "Shell.hpp"
#include "core/Recents.hpp"
#include "Log.hpp"

#include <gtkmm/label.h>
#include <gtkmm/editable.h>
#include <gtkmm/textview.h>
#include <glibmm/main.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>

#include <filesystem>

// Shell_helpers.cpp -- HELPERS. Reusable orchestration called from zones,
// bindings, and handlers: the recents seam (path resolution, persist, and the
// two-surface repaint).

namespace acorn {

std::string Shell::recents_file() const {  // helper: XDG path (UI-side resolution)
    // The UI resolves the XDG dir (Glib here) and hands the core a plain path --
    // the core stays GTK-free (docs/two-layer-seam.md).
    return (std::filesystem::path(Glib::get_user_data_dir()) / "acorn" / "recent.json")
        .string();
}

void Shell::note_recent(const std::string& path) {  // helper: push + persist + repaint
    if (path.empty()) return;
    core::recents_add(m_recents, path);
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
    rebuild_recents_page();
}

void Shell::rebuild_recents_menu() {  // helper: repaint the submenu
    if (!m_recents_menu) return;
    m_recents_menu->remove_all();

    // Display name = folder basename; identical basenames get their parent dir
    // appended so they're distinguishable (Curvz's disambiguation).
    std::vector<std::string> names(m_recents.size());
    for (std::size_t i = 0; i < m_recents.size(); ++i)
        names[i] = std::filesystem::path(m_recents[i]).filename().string();
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j)
            if (names[i] == names[j]) {
                const std::string pi =
                    std::filesystem::path(m_recents[i]).parent_path().filename().string();
                const std::string pj =
                    std::filesystem::path(m_recents[j]).parent_path().filename().string();
                if (!pi.empty()) names[i] += "  (" + pi + ")";
                if (!pj.empty()) names[j] += "  (" + pj + ")";
            }

    for (std::size_t i = 0; i < m_recents.size(); ++i) {
        auto item = Gio::MenuItem::create(names[i], "");
        item->set_action_and_target(
            "recents.open", Glib::Variant<Glib::ustring>::create(m_recents[i]));
        m_recents_menu->append_item(item);
    }

    auto clear_section = Gio::Menu::create();
    clear_section->append("Clear recent", "recents.clear");
    m_recents_menu->append_section("", clear_section);
    if (m_recents_clear) m_recents_clear->set_enabled(!m_recents.empty());
}

void Shell::rebuild_recents_page() {  // helper: repaint the live list page
    // Clear existing rows.
    while (auto* row = m_recents_list.get_row_at_index(0))
        m_recents_list.remove(*row);

    if (m_recents.empty()) {
        auto* empty = Gtk::make_managed<Gtk::Label>("(nothing remembered yet)");
        empty->set_xalign(0.0f);
        empty->set_margin(12);
        m_recents_list.append(*empty);
        return;
    }
    for (const auto& p : m_recents) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(p);
        lbl->set_xalign(0.0f);
        lbl->set_margin(8);
        lbl->set_selectable(true);
        m_recents_list.append(*lbl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// s046 -- the undo stone's helpers.
// ─────────────────────────────────────────────────────────────────────────────

// One repaint per idle, AFTER the writes that caused it. A verb can write
// several records (a move writes two); repainting per write would rebuild the
// list mid-verb, and a row rebuilt under a live click or a focused row is torn
// down (jot s045: "open UI after the rebuild you caused").
void Shell::queue_stones_repaint() {  // helper
    if (m_repaint_queued) return;
    m_repaint_queued = true;
    Glib::signal_idle().connect_once([this]() {
        m_repaint_queued = false;
        repaint_stones();
    });
}

// The list and the detail fields, from the model. Setting a field here is not a
// user choice: m_painting keeps the field's own signal from writing it back.
void Shell::repaint_stones() {  // helper
    m_painting = true;
    while (auto* c = m_stone_list.get_first_child()) m_stone_list.remove(*c);
    Gtk::ListBoxRow* chosen = nullptr;
    for (const auto& id : m_stones.ids()) {
        const auto v = m_stones.get(id);
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_name(id);   // the row carries its stone's id
        auto label = Gtk::make_managed<Gtk::Label>(
            v->name + "   ·   size " + std::to_string(v->size));
        label->set_xalign(0.0f);
        label->set_margin(6);
        row->set_child(*label);
        m_stone_list.append(*row);
        if (id == m_sel) chosen = row;
    }
    if (chosen) m_stone_list.select_row(*chosen);
    else        m_stone_list.unselect_all();

    const auto v = m_sel.empty() ? std::nullopt : m_stones.get(m_sel);
    m_stone_name.set_sensitive(v.has_value());
    m_stone_size.set_sensitive(v.has_value());
    // Never overwrite a field the user is typing in.
    if (!m_stone_name.has_focus() || !v) m_stone_name.set_text(v ? v->name : std::string{});
    if (!m_journal.in_gesture()) m_stone_size.set_value(v ? v->size : 1);
    m_painting = false;

    if (m_keys_to_list) {
        m_keys_to_list = false;
        if (chosen) chosen->grab_focus();
        else        m_stone_list.grab_focus();
    }
}

// Select a stone (and, with `keys`, give its row the keyboard) once the
// repaint the last write queued has run.
void Shell::select_stone(const std::string& id, bool keys) {  // helper
    m_sel = id;
    m_keys_to_list = m_keys_to_list || keys;
    queue_stones_repaint();
}

// Undo / Redo say what they would do, and grey when there is nothing: the
// actions (so buttons and menu items follow), the button tooltips, and the
// menu's labels -- "Undo Delete “Jade”" names the step before you take it.
void Shell::update_undo_ui() {  // helper
    if (m_act_undo) m_act_undo->set_enabled(m_journal.can_undo());
    if (m_act_redo) m_act_redo->set_enabled(m_journal.can_redo());
    const std::string u = m_journal.undo_label(), r = m_journal.redo_label();
    m_undo_button.set_tooltip_text(u.empty() ? "Nothing to undo" : "Undo " + u + "  (Ctrl+Z)");
    m_redo_button.set_tooltip_text(r.empty() ? "Nothing to redo" : "Redo " + r + "  (Ctrl+Shift+Z)");
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(u.empty() ? "Undo" : "Undo " + u, "win.undo");
        m_edit_menu->append(r.empty() ? "Redo" : "Redo " + r, "win.redo");
    }
}

// Is the keyboard in something that types? Then Ctrl+Z is that box's own,
// whether or not it had anything to undo.
bool Shell::focus_is_text() const {  // helper
    for (const Gtk::Widget* w = get_focus(); w; w = w->get_parent())
        if (dynamic_cast<const Gtk::TextView*>(w) || dynamic_cast<const Gtk::Editable*>(w)) return true;
    return false;
}

}  // namespace acorn
