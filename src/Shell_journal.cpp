#include "Shell.hpp"
#include "NewTableWindow.hpp"
#include "DesignTableWindow.hpp"
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
    m_new_record_action    = add_action("new-record",    sigc::mem_fun(*this, &Shell::on_new_record));     // s006c
    m_delete_record_action = add_action("delete-record", sigc::mem_fun(*this, &Shell::on_delete_record));  // s006c
    m_design_table_action  = add_action("design-table",  sigc::mem_fun(*this, &Shell::on_design_table));   // s007b
    m_find_action          = add_action("find",          sigc::mem_fun(*this, &Shell::start_find));        // s013a
    m_show_all_action      = add_action("show-all",      sigc::mem_fun(*this, &Shell::show_all));          // s013a
    m_print_action         = add_action("print",         sigc::mem_fun(*this, &Shell::print_shortcut));    // s026a

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
    m_marks.clear();   // s008b: marks belong to the connection
    m_found.clear();   // s013a: so does a found set
    m_found_table.clear();
    m_finding = false;
    if (m_conn && e) {
        m_journal = std::make_unique<db::Journal>(*m_conn, e->dialect(), e->caps());
        // s020c: an import writes on the worker -- its changes are synced
        // when it's done (long_done), never from the worker's thread.
        m_journal->set_on_change([this] { if (std::this_thread::get_id() == m_ui_thread) sync_undo(); });
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
    const db::Table* shown = m_table_sel.empty() ? nullptr : m_schema.table(m_table_sel);   // s007b
    if (m_design_table_action) m_design_table_action->set_enabled(cw && shown && shown->kind == db::Table::Kind::Table);
    const bool rw = m_journal && browse_writable();   // s006c
    if (m_new_record_action) m_new_record_action->set_enabled(rw);
    if (m_delete_record_action) m_delete_record_action->set_enabled(rw && !m_browse_sel.empty() && !m_record_new && !m_finding);
    if (m_find_action) m_find_action->set_enabled(m_conn && browse_info());              // s013a: reads only
    if (m_show_all_action) m_show_all_action->set_enabled(found_here() || m_finding);
    m_browse_new.set_tooltip_text(rw ? "Add a record to this table -- fill it in beside, then Add"
                                     : "This table can't take changes here (see the line under Browse)");
    m_new_table_btn.set_tooltip_text(
        cw ? "Make a new, empty table -- the SQL is shown before it runs"
           : "For now Acorn changes only database files it made itself (New Database File, Ctrl+N)");
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(cu ? "Undo " + m_journal->undo_label() : std::string("Undo"), "win.undo");
        m_edit_menu->append(cr ? "Redo " + m_journal->redo_label() : std::string("Redo"), "win.redo");
        m_edit_menu->append("New Table…", "win.new-table");
        m_edit_menu->append("Design Table…", "win.design-table");
        m_edit_menu->append("New Record", "win.new-record");
        m_edit_menu->append("Delete Record", "win.delete-record");
        m_edit_menu->append("Find…", "win.find");          // s013a
        m_edit_menu->append("Show All", "win.show-all");
        m_edit_menu->append("Print…", "win.print");        // s026a
    }
}

void Shell::journal_note(const std::string& text, const char* kind) {  // journal: under the Tables title
    m_journal_note.set_text(text);
    m_journal_note.set_visible(!text.empty());
    m_journal_note.remove_css_class("ok");
    m_journal_note.remove_css_class("error");
    if (kind && *kind) m_journal_note.add_css_class(kind);
    if (!text.empty()) set_status(text, kind);   // the Connect page says it too
    browse_note(text, kind);                      // s006c: and Browse
    graph_note(text, kind);                       // s008b: and the Graph
    layouts_note(text, kind);                     // s010b: and Layouts
    accounts_note(text, kind);                    // s019b: and Accounts
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
    refresh_marks();   // s008b: a fixed record drops its mark
    if (!show.empty()) m_table_sel = show;
    show_schema();
    rebuild_table_list();
    rebuild_browse_pick();   // s006b: Browse follows (the shown table may have gone)
    paint_layouts();         // s010b: and Layouts (an undone layout step, a gone field)
    // Give the keyboard back: the table that vanished may have had it. (Only
    // on Tables -- in Browse the focus stays where the user is.)
    if (m_stack.get_visible_child_name() == "tables") {
        if (auto* row = m_table_list.get_selected_row()) row->grab_focus();
        else m_table_list.grab_focus();
    }
}

bool Shell::focus_is_text() const {  // journal: a text box keeps its own Ctrl+Z
    auto* f = get_focus();
    // s006c: a record field whose text is what was saved has nothing of its own
    // to undo -- Ctrl+Z there undoes the last STEP. With typing pending, the
    // box keeps Ctrl+Z (your typing first, then the step).
    for (const Gtk::Widget* w = f; w; w = w->get_parent()) {
        if (!w->has_css_class("acorn-field-entry")) continue;
        const auto* e = dynamic_cast<const Gtk::Editable*>(w);
        const std::string name = w->get_name();
        const std::string col = name.rfind("record.field.", 0) == 0 ? name.substr(13) : std::string{};
        const auto* t = browse_info();
        const auto* c = t ? t->column(col) : nullptr;
        const auto* v = browse_value(col);
        if (e && c && v && std::string(e->get_text()) == db::cell_text(*v, c->type)) return false;
        return true;
    }
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

void Shell::on_design_table() {  // journal: the designer -> verbs::design_table (s007b)
    if (!m_journal || !m_writable || !m_conn_engine) return;
    const db::Table* t = m_schema.table(m_table_sel);
    if (!t || t->kind != db::Table::Kind::Table) return;
    // The live picture, with its row count, read through the store.
    m_journal->store().forget_schema();
    auto got = m_journal->store().get(db::Store::table_id(t->name));
    if (m_journal->store().failed() || !got) {
        const std::string why = m_journal->store().failed() ? m_journal->store().error().message : "it is not there any more";
        m_journal->store().clear_failed();
        journal_note("Can't design “" + t->name + "”: " + why + ".", "error");
        return;
    }
    if (!m_design_win) m_design_win = std::make_unique<DesignTableWindow>(*this);
    const std::string name = t->name;
    m_design_win->start(m_conn_engine->dialect(), std::get<db::TableDef>(*got),
        [this](const db::TableDef& w) { return db::verbs::preview(*m_journal, w); },
        [this](const db::TableDef& w) { return db::verbs::design_losses(*m_journal, w); },
        [this, name](const db::TableDef& w) -> std::expected<void, db::Error> {
            const auto before = m_journal->size();
            const bool could_redo = m_journal->can_redo();
            auto r = db::verbs::design_table(*m_journal, w);
            if (auto lg = log::get(log::Area::App))
                lg->info("journal: design '{}': {}", name, r ? m_journal->undo_label() : r.error().message);
            if (r) {
                reload_schema(name);
                const bool stepped = m_journal->size() != before || could_redo != m_journal->can_redo();
                if (stepped) journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
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
    if (auto a = name.find("“"), b = name.rfind("”");
        r && name.rfind("New Table", 0) == 0 && a != std::string::npos && b > a)
        show = name.substr(a + 3, b - a - 3);
    reload_schema(show);
    if (r) journal_note("Redid " + *r + ".", "ok");
    else   journal_note("Not redone: " + r.error().message + ".", "error");
}

}  // namespace acorn
