#include "Shell.hpp"
#include "Log.hpp"
#include "db/Browse.hpp"

#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>

// Shell_calcs.cpp -- CALCULATION FIELDS (s014a). A calculation lives in the
// solution (core::Calc): a named expression over a table's fields that Acorn
// works out for each record as Browse shows it -- never written to the
// database, so it works on a read-only connection too.
//   * the Tables area grows a Calculations section under the columns: each
//     calculation with its expression and the first record's value; New /
//     Edit open an editor in place (name, expression, a live check that says
//     what's wrong in words or shows the first record's value); Save is one
//     step (verbs::save_calc) on the same Ctrl+Z list; the trash is one step.
//   * Cards and Grid show a table's calculations after its fields; a layout
//     may show one like a field (Add Field lists them), drawn as text in the
//     Form and the List -- a calculation is never a box to type in.

namespace acorn {

namespace {

std::string curly(const std::string& s) { return "“" + s + "”"; }

Gtk::Label* cell(const std::string& text, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    if (css) l->add_css_class(css);
    return l;
}

constexpr const char* kFunctions = "Length  Upper  Lower  Left  Right  Trim  If  Round  Today";

}  // namespace

// ── values ──────────────────────────────────────────────────────────────────

std::map<std::string, core::CalcOutcome> Shell::calcs_of_row(std::size_t r) const {  // calcs: a page row's calculations
    const db::Table* t = browse_info();
    if (!t || r >= m_browse_data.rows.size()) return {};
    return db::row_calcs(*t, m_browse_data.rows[r], m_solution.calcs_of(t->name), core::today_iso());
}

std::optional<core::CalcOutcome> Shell::browse_calc(const std::string& name) const {  // calcs: the painted record's calculation
    const db::Table* t = browse_info();
    if (!t || !m_solution.calc(t->name, name)) return std::nullopt;
    const int at = m_paint_row >= 0 ? m_paint_row : m_browse_sel_index;   // as browse_value
    if (at < 0) return core::CalcOutcome{};
    auto all = calcs_of_row(static_cast<std::size_t>(at));
    if (auto it = all.find(name); it != all.end()) return it->second;
    return core::CalcOutcome{};
}

Gtk::Label* Shell::calc_label(const core::CalcOutcome& o, int max_chars) {  // calcs: a value shown, "?" with its words
    std::string one = core::calc_text(o);
    for (auto& ch : one) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    auto l = cell(one, one.empty() ? "acorn-null" : nullptr);
    l->add_css_class("acorn-calc-value");
    l->set_ellipsize(Pango::EllipsizeMode::END);
    l->set_max_width_chars(max_chars);
    l->set_single_line_mode(true);
    if (!o.ok()) {
        l->add_css_class("acorn-calc-error");
        l->set_tooltip_text("Can't work this out: " + o.error);
    } else if (static_cast<int>(one.size()) > max_chars) {
        l->set_tooltip_text(one);
    }
    return l;
}

std::vector<std::string> Shell::fields_and_calcs(const std::string& table) const {  // calcs: what a layout may show
    auto out = fields_of(table);
    for (const auto& [name, c] : m_solution.calcs_of(table)) out.push_back(name);
    return out;
}

void Shell::refresh_calc_labels() {  // calcs: a field saved in place -- worked out again
    std::map<int, std::map<std::string, core::CalcOutcome>> rows;
    for (const auto& s : m_calc_shown) {
        if (s.gen != m_view_gen || s.row < 0) continue;
        if (!rows.contains(s.row)) rows[s.row] = calcs_of_row(static_cast<std::size_t>(s.row));
        const auto& o = rows[s.row][s.name];
        const std::string shown = core::calc_text(o);
        s.label->set_text(shown);
        if (shown.empty()) s.label->add_css_class("acorn-null"); else s.label->remove_css_class("acorn-null");
        if (o.ok()) s.label->remove_css_class("acorn-calc-error"); else s.label->add_css_class("acorn-calc-error");
        s.label->set_tooltip_text(o.ok() ? std::string{} : "Can't work this out: " + o.error);
    }
}

// ── the Tables area's section ───────────────────────────────────────────────

void Shell::calc_section(const db::Table& t) {  // calcs: the Calculations section (list or editor)
    if (m_calc_table != t.name) {   // another table: the editor closes
        m_calc_table = t.name;
        m_calc_editing.clear();
        m_calc_new = false;
    }
    const bool editing = m_calc_new || !m_calc_editing.empty();
    if (!editing) { m_calc_typed_name.clear(); m_calc_typed_expr.clear(); }
    // An undo took away the calculation being changed: the editor closes.
    if (!m_calc_editing.empty() && !m_solution.calc(t.name, m_calc_editing)) {
        m_calc_editing.clear();
        return calc_section(t);
    }

    const bool can = m_journal != nullptr;
    const auto& calcs = m_solution.calcs_of(t.name);
    const std::string table = t.name;

    // The first record, for the values beside each calculation (a SELECT).
    std::optional<db::Row> first;
    if (m_conn) {
        auto p = db::read_page(*m_conn, t, {0, 1});
        if (p && !p->rows.empty()) first = p->rows.front();
    }
    const auto values = first ? db::row_calcs(t, *first, calcs, core::today_iso())
                              : std::map<std::string, core::CalcOutcome>{};

    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_name("tables.calcs");
    Gtk::Entry* focus = nullptr;   // the editor's box to type in -- once it is in the window

    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_column_spacing(14);
    grid->set_row_spacing(5);
    int r = 0;
    if (!calcs.empty()) {
        for (int c = 0; const char* h : {"Name", "Worked out as", first ? "First record" : ""})
            grid->attach(*cell(h, "acorn-th"), c++, r);
        ++r;
    }
    for (const auto& [name, c] : calcs) {
        if (name == m_calc_editing) continue;   // its editor is below
        grid->attach(*cell(name, "acorn-calc-name"), 0, r);
        auto ex = cell("= " + c.expr, "acorn-mono");
        ex->set_ellipsize(Pango::EllipsizeMode::END);
        ex->set_max_width_chars(48);
        ex->set_hexpand(true);
        ex->set_tooltip_text(c.expr);
        grid->attach(*ex, 1, r);
        if (first) {
            auto it = values.find(name);
            grid->attach(*calc_label(it != values.end() ? it->second : core::CalcOutcome{}, 28), 2, r);
        }
        auto edit = Gtk::make_managed<Gtk::Button>();
        edit->set_icon_name("document-edit-symbolic");
        edit->add_css_class("flat");
        edit->set_name("tables.calc.edit." + name);
        edit->set_tooltip_text("Change " + curly(name));
        edit->set_sensitive(can && !editing);
        edit->signal_clicked().connect([this, name, c, table] {
            m_calc_editing = name;
            m_calc_new = false;
            m_calc_typed_name = name;
            m_calc_typed_expr = c.expr;
            if (const db::Table* tt = m_schema.table(table)) calc_section(*tt);
        });
        grid->attach(*edit, 3, r);
        auto del = Gtk::make_managed<Gtk::Button>();
        del->set_icon_name("user-trash-symbolic");
        del->add_css_class("flat");
        del->set_name("tables.calc.delete." + name);
        del->set_tooltip_text("Delete " + curly(name) + " -- Ctrl+Z brings it back");
        del->set_sensitive(can && !editing);
        del->signal_clicked().connect([this, table, name] { drop_calc(table, name); });
        grid->attach(*del, 4, r);
        ++r;
    }
    if (r > 0) body->append(*grid);

    if (!editing) {
        if (calcs.empty())
            body->append(*cell("None yet. A calculation is worked out by Acorn for each record -- "
                               "Upper(last_name) & \", \" & first_name -- and shown beside its fields. "
                               "It lives in the solution; the database never sees it.", "acorn-status"));
        if (auto* l = dynamic_cast<Gtk::Label*>(body->get_last_child())) {
            l->set_wrap(true);
            l->set_max_width_chars(70);
        }
        auto add = Gtk::make_managed<Gtk::Button>("New Calculation…");
        add->set_name("tables.calc.new");
        add->set_halign(Gtk::Align::START);
        add->set_sensitive(can);
        add->set_tooltip_text(can ? "A name and what it is worked out as" : "Open a database first");
        add->signal_clicked().connect([this, table] {
            m_calc_new = true;
            m_calc_editing.clear();
            m_calc_typed_name.clear();
            m_calc_typed_expr.clear();
            if (const db::Table* tt = m_schema.table(table)) calc_section(*tt);
        });
        body->append(*add);
    } else {
        // The editor: Name = expression, a live line, the fields to read, Cancel / Save.
        const std::string was = m_calc_editing;
        auto ed = Gtk::make_managed<Gtk::Grid>();
        ed->set_column_spacing(8);
        ed->set_row_spacing(6);
        ed->add_css_class("acorn-calc-editor");
        auto name_e = Gtk::make_managed<Gtk::Entry>();
        name_e->set_name("tables.calc.name");
        name_e->set_placeholder_text("name");
        name_e->set_width_chars(16);
        name_e->set_text(m_calc_typed_name);
        auto expr_e = Gtk::make_managed<Gtk::Entry>();
        expr_e->set_name("tables.calc.expr");
        expr_e->add_css_class("acorn-mono");
        expr_e->set_placeholder_text("first_name & \" \" & last_name");
        expr_e->set_hexpand(true);
        expr_e->set_width_chars(40);
        expr_e->set_text(m_calc_typed_expr);
        ed->attach(*name_e, 0, 0);
        ed->attach(*cell("="), 1, 0);
        ed->attach(*expr_e, 2, 0);
        auto live = cell("", "acorn-status");
        live->set_name("tables.calc.live");
        live->set_wrap(true);
        live->set_xalign(0.0f);
        ed->attach(*live, 0, 1, 3, 1);
        std::string fl;
        for (const auto& f : fields_of(table)) fl += (fl.empty() ? "" : "  ") + f;
        for (const auto& [n, c] : calcs) if (n != was) fl += (fl.empty() ? "" : "  ") + n;
        auto fields = cell("Fields:  " + fl, "acorn-small");
        fields->set_wrap(true);
        fields->set_selectable(true);
        ed->attach(*fields, 0, 2, 3, 1);
        auto fns = cell(std::string("Functions:  ") + kFunctions + "    ·    + - * /  &  = <> < > <= >=  and or not",
                        "acorn-small");
        fns->set_wrap(true);
        ed->attach(*fns, 0, 3, 3, 1);
        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_halign(Gtk::Align::END);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->set_name("tables.calc.cancel");
        auto save = Gtk::make_managed<Gtk::Button>(was.empty() ? "Add" : "Save");
        save->set_name("tables.calc.save");
        save->add_css_class("suggested-action");
        buttons->append(*cancel);
        buttons->append(*save);
        ed->attach(*buttons, 0, 4, 3, 1);
        body->append(*ed);

        // The live line: what's wrong, in words -- or the first record's value.
        std::vector<std::string> cols = fields_of(table);
        auto check = [this, live, save, name_e, expr_e, cols, table, was, first] {
            m_calc_typed_name = name_e->get_text();
            m_calc_typed_expr = expr_e->get_text();
            core::Calc c{table, m_calc_typed_name, m_calc_typed_expr};
            auto others = m_solution.calcs_of(table);
            if (!was.empty()) others.erase(was);
            std::string why;
            if (c.expr.empty()) why = "Type what it is worked out as.";
            else if (c.name.empty()) why = "Give it a name.";
            else if (others.contains(c.name)) why = curly(table) + " already has a calculation " + curly(c.name) + ".";
            else why = core::check_calc(c, cols, others);
            live->remove_css_class("error");
            if (!why.empty()) {
                const bool soft = c.expr.empty() || c.name.empty();
                live->set_text(why);
                if (!soft) live->add_css_class("error");
                save->set_sensitive(false);
                return;
            }
            save->set_sensitive(true);
            if (!first) { live->set_text("Fine. (No records to try it on.)"); return; }
            others[c.name] = c;
            const db::Table* tt = m_schema.table(table);
            auto v = tt ? db::row_calcs(*tt, *first, others, core::today_iso()) : std::map<std::string, core::CalcOutcome>{};
            const auto& o = v[c.name];
            live->set_text(o.ok() ? "First record: " + (core::calc_text(o).empty() ? std::string("(empty)") : core::calc_text(o))
                                  : "First record: ? -- " + o.error);
        };
        check();
        name_e->signal_changed().connect(check);
        expr_e->signal_changed().connect(check);
        auto close = [this, table] {
            m_calc_new = false;
            m_calc_editing.clear();
            if (const db::Table* tt = m_schema.table(table)) calc_section(*tt);
        };
        cancel->signal_clicked().connect(close);
        auto commit = [this, save, table, was] {
            if (!save->get_sensitive()) return;
            keep_calc(core::Calc{table, m_calc_typed_name, m_calc_typed_expr}, was);
        };
        save->signal_clicked().connect(commit);
        name_e->signal_activate().connect(commit);
        expr_e->signal_activate().connect(commit);
        for (Gtk::Entry* e : {name_e, expr_e}) {   // Esc: put it away, nothing kept
            auto keys = Gtk::EventControllerKey::create();
            keys->signal_key_pressed().connect([close](guint key, guint, Gdk::ModifierType) {
                if (key != GDK_KEY_Escape) return false;
                close();
                return true;
            }, false);
            e->add_controller(keys);
        }
        focus = m_calc_new && m_calc_typed_name.empty() ? name_e : expr_e;
    }

    // Repaint in place when the section is already there; else hand it to show_table.
    if (m_calc_box && m_calc_box->get_parent()) {
        while (auto* c = m_calc_box->get_last_child()) {
            if (c == m_calc_box->get_first_child()) break;   // keep the heading
            m_calc_box->remove(*c);
        }
        m_calc_box->append(*body);
    }
    if (focus) focus->grab_focus();
}

// ── the verbs ───────────────────────────────────────────────────────────────

void Shell::keep_calc(const core::Calc& c, const std::string& was) {  // calcs: verbs::save_calc, the note, a repaint
    if (!m_journal) return;
    const auto before = m_journal->size();
    const bool could_redo = m_journal->can_redo();
    auto r = db::verbs::save_calc(*m_journal, c, was);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: calc '{}' of '{}' = '{}': {}", c.name, c.table, c.expr, r ? m_journal->undo_label() : r.error().message);
    if (!r) { journal_note("Not kept: " + r.error().message + ".", "error"); return; }
    m_calc_new = false;
    m_calc_editing.clear();
    calcs_changed(c.table);
    if (m_journal->size() != before || could_redo != m_journal->can_redo())
        journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
}

void Shell::drop_calc(const std::string& table, const std::string& name) {  // calcs: verbs::delete_calc
    if (!m_journal) return;
    auto r = db::verbs::delete_calc(*m_journal, table, name);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: delete calc '{}' of '{}': {}", name, table, r ? "done" : r.error().message);
    if (!r) { journal_note("Not deleted: " + r.error().message + ".", "error"); return; }
    calcs_changed(table);
    journal_note("Deleted the calculation " + curly(name) + ". Ctrl+Z brings it back.", "ok");
}

void Shell::calcs_changed(const std::string& table) {  // calcs: Tables, Browse and Layouts follow a calc step
    if (m_table_sel == table) show_table(table);
    load_browse_page(true);
    paint_layouts();
    sync_undo();
}

}  // namespace acorn
