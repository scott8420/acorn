#include "Shell.hpp"
#include "Log.hpp"

#include <gdk/gdkkeysyms.h>
#include <glibmm/main.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>

// Shell_records.cpp -- the RECORDS category (s006c): editing in Browse.
//
// The record panel sits beside the cards / grid. Pick a record and its fields
// are boxes; Enter, or leaving a box, saves that one field as ONE step through
// the door (verbs::set_field -- "Edit “age”", Ctrl+Z undoes it). Escape puts
// the box back. Yes-No fields are check boxes and save on the click. Key
// fields, blobs, and every field of a table Acorn can't write are shown, not
// boxed. New Record turns the panel into a blank record with an Add button
// (verbs::new_record); Delete Record removes the picked one (Ctrl+Z brings it
// back, key and all).
//
// Nothing here writes SQL; it only asks m_journal.

namespace acorn {

namespace {

Gtk::Label* plain(const std::string& s, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    l->set_wrap(true);
    l->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    if (css) l->add_css_class(css);
    return l;
}

bool is_yes(const db::Value& v) {
    if (auto i = v.as_int()) return *i != 0;
    if (const auto* t = v.as_text()) return *t == "t" || *t == "true" || *t == "1";
    return false;
}

}  // namespace

bool Shell::browse_writable() const {  // records: Acorn-made + a table with a key
    const auto* t = browse_info();
    return m_writable && m_journal && t && db::Store::why_not_writable(*t).empty();
}

void Shell::browse_note(const std::string& text, const char* kind) {  // records: under the Browse title
    m_browse_note.set_text(text);
    m_browse_note.set_visible(!text.empty());
    m_browse_note.remove_css_class("ok");
    m_browse_note.remove_css_class("error");
    if (kind && *kind) m_browse_note.add_css_class(kind);
}

const db::Value* Shell::browse_value(const std::string& column) const {  // records: the picked field
    if (m_browse_sel_index < 0 || static_cast<std::size_t>(m_browse_sel_index) >= m_browse_data.rows.size())
        return nullptr;
    const auto& row = m_browse_data.rows[static_cast<std::size_t>(m_browse_sel_index)];
    for (std::size_t i = 0; i < m_browse_data.columns.size() && i < row.size(); ++i)
        if (m_browse_data.columns[i] == column) return &row[i];
    return nullptr;
}

void Shell::paint_record() {  // records: the panel
    ++m_record_gen;   // first: anything the old panel fires from here on is stale
    const unsigned gen = m_record_gen;
    m_record_inputs.clear();
    const db::Table* t = browse_info();
    const bool picked = m_browse_sel_index >= 0 &&
                        static_cast<std::size_t>(m_browse_sel_index) < m_browse_data.rows.size();
    // s010a: the Form IS the record's editor -- the panel only for a new one.
    if (!t || (!picked && !m_record_new) || (m_browse_view == BrowseView::Form && !m_record_new)) {
        m_record_scroll.set_visible(false);
        m_record_scroll.unset_child();
        return;
    }
    const bool can = browse_writable();
    const bool fresh = m_record_new && can;
    const std::string row_id = picked && !fresh ? m_browse_sel : std::string{};

    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_name("shell.browse.record_panel");
    std::string head = fresh ? "New record" : db::card_title(*t, m_browse_data.rows[static_cast<std::size_t>(m_browse_sel_index)]);
    auto title = plain(head, "acorn-card-title");
    box->append(*title);
    if (fresh)
        box->append(*plain("Fill in what you know, then Add. Empty fields take their default.", "acorn-status"));
    else if (!can)
        box->append(*plain("Read-only -- see the line under Browse.", "acorn-status"));
    else
        box->append(*plain("Enter or leaving a field saves it; Esc puts it back. Ctrl+Z undoes a save.", "acorn-status"));

    auto g = Gtk::make_managed<Gtk::Grid>();
    g->set_column_spacing(10);
    g->set_row_spacing(6);
    int r = 0;
    for (const auto& col : t->columns) {
        const bool key = col.pk_position > 0;
        auto name = plain(col.name + (key ? "  (key)" : ""), "acorn-field-name");
        name->set_valign(Gtk::Align::CENTER);
        name->set_wrap(false);
        g->attach(*name, 0, r);

        const db::Value* v = fresh ? nullptr : browse_value(col.name);
        const std::string shown = v ? db::cell_text(*v, col.type) : std::string{};
        Gtk::Widget* w = nullptr;

        const bool boxed = can && col.type != db::FieldType::Container &&
                           (fresh ? !(key && col.auto_increment) : !key);
        if (!boxed) {
            std::string s = shown;
            if (fresh && key && col.auto_increment) s = "(assigned)";
            auto l = plain(s.empty() && !fresh ? "" : s, s.empty() || fresh ? "acorn-null" : nullptr);
            l->set_selectable(true);
            if (key && can && !fresh) l->set_tooltip_text("The record's key -- Acorn finds the record by it, so it is not changed here");
            w = l;
        } else if (col.type == db::FieldType::Boolean) {
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_name("record.field." + col.name);
            if (fresh || !v || v->is_null()) cb->set_inconsistent(fresh || (v && v->is_null()));
            if (v && !v->is_null()) cb->set_active(is_yes(*v));
            if (!fresh) {
                const std::string colname = col.name;
                cb->signal_toggled().connect([this, cb, row_id, colname, gen] {
                    if (m_browse_painting || gen != m_record_gen) return;
                    cb->set_inconsistent(false);
                    commit_field(row_id, colname, cb->get_active() ? "Yes" : "No");
                });
            } else {
                cb->signal_toggled().connect([cb] { cb->set_inconsistent(false); });
                m_record_inputs.emplace_back(col.name, cb);
            }
            w = cb;
        } else {
            auto e = Gtk::make_managed<Gtk::Entry>();
            e->set_name("record.field." + col.name);
            e->add_css_class("acorn-field-entry");
            e->set_hexpand(true);
            e->set_width_chars(8);
            e->set_text(shown);
            if (v && v->is_null()) e->set_placeholder_text("empty");
            if (fresh) {
                if (col.default_sql) e->set_placeholder_text("default: " + *col.default_sql);
                else if (!col.nullable) e->set_placeholder_text("required");
                m_record_inputs.emplace_back(col.name, e);
                e->signal_activate().connect([this] { on_add_record(); });
            } else {
                wire_field_entry(e, row_id, col.name, &m_record_gen, gen);
            }
            w = e;
        }
        w->set_hexpand(true);
        g->attach(*w, 1, r);
        ++r;
    }
    box->append(*g);

    if (fresh) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_halign(Gtk::Align::END);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->signal_clicked().connect([this] {
            m_record_new = false;
            browse_note("", "");
            if (m_browse_view == BrowseView::Form) { load_browse_page(); return; }   // s010a: back to a record
            paint_record();
            sync_undo();
        });
        auto add = Gtk::make_managed<Gtk::Button>("Add");
        add->add_css_class("acorn-primary");
        add->set_name("shell.browse.add_record");
        add->signal_clicked().connect([this] { on_add_record(); });
        row->append(*cancel);
        row->append(*add);
        box->append(*row);
    }

    m_browse_painting = true;
    m_record_scroll.set_child(*box);
    m_record_scroll.set_visible(true);
    m_browse_painting = false;
    if (fresh) {
        for (auto& [c, w] : m_record_inputs)
            if (dynamic_cast<Gtk::Entry*>(w)) { w->grab_focus(); break; }
    }
}

void Shell::wire_field_entry(Gtk::Entry* e, const std::string& row_id, const std::string& column,
                             const unsigned* counter, unsigned gen) {  // records: Enter / leave saves; Esc puts back
    const std::string colname = column;
    e->signal_activate().connect([this, e, row_id, colname, counter, gen] {
        if (gen != *counter) return;
        commit_field(row_id, colname, e->get_text());
    });
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect([this, e, row_id, colname, counter, gen] {
        if (m_browse_painting || gen != *counter) return;
        commit_field(row_id, colname, e->get_text());
    });
    e->add_controller(focus);
    // The box has no undo of its own (GTK's would take Ctrl+Z
    // before the window hears it, and leaving the box would then
    // save the half-undone text). Instead: Esc -- or Ctrl+Z with
    // typing pending -- puts the box back to what is saved; Ctrl+Z
    // with nothing pending undoes the step (win.undo).
    e->set_enable_undo(false);
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this, e, colname, counter, gen](guint keyval, guint, Gdk::ModifierType state) -> bool {
            if (gen != *counter) return false;
            const bool ctrl_z = (keyval == GDK_KEY_z || keyval == GDK_KEY_Z) &&
                (state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK &&
                (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType::SHIFT_MASK;
            if (keyval != GDK_KEY_Escape && !ctrl_z) return false;
            const auto* t2 = browse_info();
            const auto* c2 = t2 ? t2->column(colname) : nullptr;
            const auto* v2 = browse_value(colname);
            const std::string saved = c2 && v2 ? db::cell_text(*v2, c2->type) : std::string{};
            if (ctrl_z && std::string(e->get_text()) == saved) {   // nothing pending: undo the step
                activate_action("win.undo");
                return true;
            }
            e->set_text(saved);
            e->set_position(-1);
            return true;
        },
        false);
    // CAPTURE: the box's inner text widget would otherwise take the
    // key first (s006c drive: Ctrl+Z never bubbled out of it).
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    e->add_controller(keys);
}

void Shell::commit_field(const std::string& row, const std::string& column, const std::string& typed) {  // records: -> set_field
    if (!m_journal || row.empty() || !browse_writable()) return;
    const db::Table* t = browse_info();
    const db::Column* c = t ? t->column(column) : nullptr;
    if (!c) return;
    // Only the record the panel was painted for, and only a real change: a
    // box left alone, or NULL shown as "", is not a step.
    if (row != m_browse_sel) return;
    if (const auto* now = browse_value(column); now && db::cell_text(*now, c->type) == typed) return;

    auto v = db::parse_field(typed, *c);
    if (!v) {
        browse_note("Not saved: " + v.error() + ".", "error");
        return;
    }
    auto r = db::verbs::set_field(*m_journal, row, column, *v);
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: edit {} '{}': {}", db::Store::describe(row), column, r ? "saved" : r.error().message);
    if (r) browse_note("Saved “" + column + "”. Ctrl+Z undoes it.", "ok");
    else   browse_note("Not saved: " + r.error().message + ".", "error");
    // Re-read after this handler returns (the box that called may be the one
    // a repaint would destroy). The panel stays: its box holds the focus.
    // A refusal repaints the panel too, so the box shows what is really there.
    const bool keep = r.has_value();
    Glib::signal_idle().connect_once([this, keep] { load_browse_page(keep); });
}

void Shell::on_new_record() {  // records: the panel, blank
    if (!browse_writable()) return;
    m_record_new = true;
    m_browse_sel.clear();
    m_browse_sel_index = -1;
    browse_note("", "");
    paint_browse();   // drop the highlight
    paint_record();
    sync_undo();
}

void Shell::on_add_record() {  // records: Add -> new_record
    if (!m_record_new || !browse_writable()) return;
    const db::Table* t = browse_info();
    std::vector<std::string> cols;
    std::vector<db::Value> vals;
    for (auto& [name, w] : m_record_inputs) {
        const db::Column* c = t->column(name);
        if (!c) continue;
        if (auto* cb = dynamic_cast<Gtk::CheckButton*>(w)) {
            if (cb->get_inconsistent()) continue;   // untouched: the default
            cols.push_back(name);
            vals.emplace_back(std::int64_t{cb->get_active() ? 1 : 0});
            continue;
        }
        auto* e = dynamic_cast<Gtk::Entry*>(w);
        if (!e || e->get_text().empty()) continue;  // empty: the default
        auto v = db::parse_field(e->get_text(), *c);
        if (!v) { browse_note("Not added: " + v.error() + ".", "error"); e->grab_focus(); return; }
        cols.push_back(name);
        vals.push_back(*v);
    }
    auto r = db::verbs::new_record(*m_journal, t->name, cols, vals);
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: new record in '{}': {}", t->name, r ? *r : r.error().message);
    if (!r) {
        browse_note("Not added: " + r.error().message + ".", "error");
        return;
    }
    m_record_new = false;
    m_browse_sel = *r;
    // In key order the new record is last: show its page.
    if (m_browse_req.sort.empty() && !m_browse_req.descending) {
        auto p = db::read_page(*m_conn, *t, {0, 1});
        if (p) m_browse_req.offset = ((p->total - 1) / m_browse_req.limit) * m_browse_req.limit;
    }
    browse_note("Added a record to “" + t->name + "”. Ctrl+Z takes it away.", "ok");
    Glib::signal_idle().connect_once([this] { load_browse_page(); });
}

void Shell::on_delete_record() {  // records: -> delete_record
    if (!browse_writable() || m_browse_sel.empty() || m_record_new) return;
    const std::string id = m_browse_sel;
    auto r = db::verbs::delete_record(*m_journal, id);
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: delete {}: {}", db::Store::describe(id), r ? "deleted" : r.error().message);
    if (r) {
        browse_note("Deleted " + db::Store::describe(id) + ". Ctrl+Z brings it back.", "ok");
        // s010a: the Form moves on to the next record (the one before, at the end).
        const int i = m_browse_sel_index;
        const auto n = static_cast<int>(m_browse_data.ids.size());
        m_browse_sel.clear();
        m_browse_sel_index = -1;
        if (m_browse_view == BrowseView::Form && i >= 0 && n > 1)
            m_browse_sel = m_browse_data.ids[static_cast<std::size_t>(i + 1 < n ? i + 1 : i - 1)];
    } else {
        browse_note("Not deleted: " + r.error().message + ".", "error");
    }
    load_browse_page();
}

}  // namespace acorn
