#include "Shell.hpp"
#include "NewTableWindow.hpp"
#include "Log.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtkmm/editable.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/text.h>
#include <gtkmm/textview.h>

// Shell_journal.cpp -- the JOURNAL category (s005b). The UI side of the door:
// it ASKS db::Journal to make a change, undo or redo one, and repaints what
// the database now says. Nothing here writes SQL.
//
// The keys (docs/undo.md -- the part that bit jot):
//   * Ctrl+Z / Ctrl+Shift+Z are NOT accelerators: an accel fires before the
//     focused widget and would take undo away from every text box. They are
//     doc-only rows in the shortcuts registry.
//   * The WINDOW hears them in the BUBBLE phase -- only keys nothing else took
//     -- and steps aside when the focus is in a text box.
//   * activate_action("win.undo"): the full name ("undo" alone does nothing).

namespace acorn {

void Shell::bind_journal() {  // journal: win.undo / win.redo / win.new-table + the window's Ctrl+Z
    m_undo_action      = add_action("undo",      sigc::mem_fun(*this, &Shell::on_undo));
    m_redo_action      = add_action("redo",      sigc::mem_fun(*this, &Shell::on_redo));
    m_new_table_action = add_action("new-table", sigc::mem_fun(*this, &Shell::on_new_table));

    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) -> bool {
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK |
                                       Gdk::ModifierType::ALT_MASK);
            if ((mods & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType::CONTROL_MASK) return false;
            if ((mods & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType::ALT_MASK) return false;
            if (keyval != GDK_KEY_z && keyval != GDK_KEY_Z) return false;
            if (focus_is_text()) return false;   // the text box keeps its own undo
            const bool shift = (mods & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
            activate_action(shift ? "win.redo" : "win.undo");
            return true;
        },
        false);
    add_controller(keys);
    sync_undo();
}

void Shell::attach_journal(db::Engine* e) {  // journal: a new connection -> a fresh journal
    m_conn_engine = e;
    m_journal.reset();
    if (m_conn && e) {
        m_journal = std::make_unique<db::Journal>(*m_conn, e->dialect(), e->caps());
        m_journal->set_on_change([this] { sync_undo(); });
    }
    journal_note("", "");
    sync_undo();
}

void Shell::sync_undo() {  // journal: re-label + grey Undo / Redo / New Table
    const bool cu = m_journal && m_journal->can_undo();
    const bool cr = m_journal && m_journal->can_redo();
    const bool cw = m_journal && m_writable;
    if (m_undo_action) m_undo_action->set_enabled(cu);
    if (m_redo_action) m_redo_action->set_enabled(cr);
    if (m_new_table_action) m_new_table_action->set_enabled(cw);
    m_new_table_btn.set_tooltip_text(
        cw ? "Make a new, empty table -- the SQL is shown before it runs"
           : "For now Acorn changes only database files it made itself (New Database File, Ctrl+N)");
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(cu ? "Undo " + m_journal->undo_label() : std::string("Undo"), "win.undo");
        m_edit_menu->append(cr ? "Redo " + m_journal->redo_label() : std::string("Redo"), "win.redo");
        m_edit_menu->append("New Table…", "win.new-table");
    }
}

void Shell::journal_note(const std::string& text, const char* kind) {  // journal: under the Tables title
    m_journal_note.set_text(text);
    m_journal_note.set_visible(!text.empty());
    m_journal_note.remove_css_class("ok");
    m_journal_note.remove_css_class("error");
    if (kind && *kind) m_journal_note.add_css_class(kind);
    if (!text.empty()) set_status(text, kind);   // the Connect page says it too
}

void Shell::reload_schema(const std::string& show) {  // journal: re-read, repaint, select `show`
    // On the UI thread: in s005b only a local file Acorn made is writable, so
    // this is a few milliseconds. A server joins the door with s006's worker.
    if (!m_conn) return;
    auto s = m_conn->introspect();
    if (!s) {
        journal_note("Could not re-read the database: " + s.error().message, "error");
        return;
    }
    m_schema = std::move(*s);
    if (!show.empty()) m_table_sel = show;
    show_schema();
    rebuild_table_list();
    // Give the keyboard back: the table that vanished may have had it.
    if (auto* row = m_table_list.get_selected_row()) row->grab_focus();
    else m_table_list.grab_focus();
}

bool Shell::focus_is_text() const {  // journal: a text box keeps its own Ctrl+Z
    auto* f = get_focus();
    return dynamic_cast<const Gtk::Text*>(f) || dynamic_cast<const Gtk::TextView*>(f) ||
           dynamic_cast<const Gtk::Editable*>(f);
}

void Shell::on_new_table() {  // journal: the New Table window -> Journal::run
    if (!m_journal || !m_writable || !m_conn_engine) return;
    m_stack.set_visible_child("tables");
    if (!m_new_table_win) m_new_table_win = std::make_unique<NewTableWindow>(*this);
    const char q = m_conn->quote_ident("x").front();
    m_new_table_win->start(m_conn_engine->dialect(), q, m_schema,
        [this](const db::TableDef& t) -> std::expected<void, db::Error> {
            auto r = db::verbs::new_table(*m_journal, t);
            if (auto lg = log::get(log::Area::App))
                lg->info("journal: new table '{}': {}", t.name, r ? "made" : r.error().message);
            if (r) {
                reload_schema(t.name);
                journal_note("Made table “" + t.name + "”. Ctrl+Z undoes it.", "ok");
            }
            return r;
        });
}

void Shell::on_undo() {  // journal: Ctrl+Z
    if (!m_journal || !m_journal->can_undo()) return;
    auto r = m_journal->undo();
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: undo: {}", r ? *r : r.error().message);
    reload_schema("");
    if (r) journal_note("Undid " + *r + ".", "ok");
    else   journal_note("Not undone: " + r.error().message + ".", "error");
}

void Shell::on_redo() {  // journal: Ctrl+Shift+Z
    if (!m_journal || !m_journal->can_redo()) return;
    const std::string name = m_journal->redo_label();
    auto r = m_journal->redo();
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: redo: {}", r ? *r : r.error().message);
    // Select what came back: the label is New Table “name”.
    std::string show;
    if (auto a = name.find("“"), b = name.rfind("”"); r && a != std::string::npos && b > a)
        show = name.substr(a + 3, b - a - 3);
    reload_schema(show);
    if (r) journal_note("Redid " + *r + ".", "ok");
    else   journal_note("Not redone: " + r.error().message + ".", "error");
}

}  // namespace acorn
