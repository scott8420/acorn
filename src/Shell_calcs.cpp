#include "Shell.hpp"
#include "Log.hpp"
#include "db/Browse.hpp"

#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/stringlist.h>
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
    return db::row_calcs(*t, m_browse_data.rows[r], m_solution.calcs_of(t->name), core::today_iso(), &m_browse_sums);
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
    for (const auto& [name, x] : m_solution.summaries_of(table)) out.push_back(name);   // s014b
    return out;
}

// ── s014b: summaries in Browse ──────────────────────────────────────────────

void Shell::read_browse_summaries() {  // calcs: the shown table's summaries over the found set
    m_browse_sums.clear();
    m_browse_sums_error.clear();
    const db::Table* t = browse_info();
    if (!m_conn || !t || m_solution.summaries_of(t->name).empty()) return;
    auto r = db::read_summaries(*m_conn, *t, m_solution.summaries_of(t->name),
                                found_here() ? m_found : std::vector<db::Criterion>{}, &m_schema);
    if (r) m_browse_sums = std::move(*r);
    else   m_browse_sums_error = r.error().message;
    if (auto lg = log::get(log::Area::Shell))
        lg->debug("browse: {} summaries of '{}' {}", m_browse_sums.size(), t->name, m_browse_sums_error);
}

std::optional<core::CalcOutcome> Shell::browse_summary(const std::string& name) const {  // calcs: a summary's value as shown
    const db::Table* t = browse_info();
    const core::Summary* x = t ? m_solution.summary(t->name, name) : nullptr;
    if (!x) return std::nullopt;
    if (!m_browse_sums_error.empty()) return core::CalcOutcome{{}, m_browse_sums_error};
    if (auto it = m_browse_sums.find(name); it != m_browse_sums.end()) return core::CalcOutcome{it->second, ""};
    return core::CalcOutcome{{}, "“" + t->name + "” has no field “" + x->field + "” to summarise"};
}

Gtk::Widget* Shell::summary_strip() {  // calcs: the summaries above Cards / Grid
    const db::Table* t = browse_info();
    if (!t || m_solution.summaries_of(t->name).empty()) return nullptr;
    auto strip = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 18);
    strip->add_css_class("acorn-summary-strip");
    strip->set_name("shell.browse.summaries");
    const auto n = m_browse_data.total;
    auto over = cell(found_here() ? "Over the " + std::to_string(n) + " found:"
                                  : "Over " + std::string(n == 1 ? "the 1 record:" : "all " + std::to_string(n) + " records:"),
                     "acorn-field-name");
    strip->append(*over);
    for (const auto& [name, x] : m_solution.summaries_of(t->name)) {
        auto pair = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto nl = cell(name, "acorn-field-name");
        nl->add_css_class("acorn-calc-name");
        nl->set_tooltip_text(core::summary_says(x));
        pair->append(*nl);
        auto v = calc_label(browse_summary(name).value_or(core::CalcOutcome{}), 24);
        v->add_css_class("acorn-summary-value");
        pair->append(*v);
        strip->append(*pair);
    }
    return strip;
}

void Shell::browse_show(Gtk::Widget& view) {  // calcs: the view into the scroll, the strip above it
    auto* strip = summary_strip();
    if (!strip) { m_browse_scroll.set_child(view); return; }
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_valign(Gtk::Align::START);
    box->append(*strip);
    box->append(view);
    m_browse_scroll.set_child(*box);
}

void Shell::refresh_calc_labels() {  // calcs: a field saved in place -- worked out again
    std::map<int, std::map<std::string, core::CalcOutcome>> rows;
    for (const auto& s : m_calc_shown) {
        if (s.gen != m_view_gen) continue;
        if (s.row == -2) {   // s014b: a summary
            const auto o = browse_summary(s.name).value_or(core::CalcOutcome{});
            s.label->set_text(core::calc_text(o));
            if (o.ok()) s.label->remove_css_class("acorn-calc-error"); else s.label->add_css_class("acorn-calc-error");
            continue;
        }
        if (s.row < 0) continue;
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
        m_sum_editing.clear();
        m_sum_new = false;
    }
    if (!m_sum_editing.empty() && !m_solution.summary(t.name, m_sum_editing)) m_sum_editing.clear();   // undone away
    const bool sum_editing = m_sum_new || !m_sum_editing.empty();
    const bool editing = m_calc_new || !m_calc_editing.empty() || sum_editing;
    if (!m_calc_new && m_calc_editing.empty()) { m_calc_typed_name.clear(); m_calc_typed_expr.clear(); }
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
    // s014b: the summaries over every record (one SELECT); calculations read them.
    const auto& summaries = m_solution.summaries_of(t.name);
    std::map<std::string, core::CalcValue> sums;
    std::string sums_error;
    if (m_conn && !summaries.empty()) {
        auto sr = db::read_summaries(*m_conn, t, summaries);
        if (sr) sums = std::move(*sr); else sums_error = sr.error().message;
    }
    const auto values = first ? db::row_calcs(t, *first, calcs, core::today_iso(), &sums)
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

    // s014b: the summaries -- what each sums up, its value over every record.
    if (!summaries.empty()) {
        auto sg = Gtk::make_managed<Gtk::Grid>();
        sg->set_column_spacing(14);
        sg->set_row_spacing(5);
        int sr = 0;
        for (int c = 0; const char* h : {"Summary", "Of", "Every record"}) sg->attach(*cell(h, "acorn-th"), c++, sr);
        ++sr;
        for (const auto& [name, x] : summaries) {
            if (name == m_sum_editing) continue;
            sg->attach(*cell(name, "acorn-calc-name"), 0, sr);
            auto says = cell(core::summary_says(x));
            says->set_hexpand(true);
            sg->attach(*says, 1, sr);
            core::CalcOutcome o;
            if (!sums_error.empty()) o.error = sums_error;
            else if (auto it = sums.find(name); it != sums.end()) o.value = it->second;
            else o.error = "there's no field " + curly(x.field) + " to summarise";
            auto v = calc_label(o, 28);
            v->add_css_class("acorn-summary-value");
            sg->attach(*v, 2, sr);
            auto edit = Gtk::make_managed<Gtk::Button>();
            edit->set_icon_name("document-edit-symbolic");
            edit->add_css_class("flat");
            edit->set_name("tables.summary.edit." + name);
            edit->set_tooltip_text("Change " + curly(name));
            edit->set_sensitive(can && !editing);
            edit->signal_clicked().connect([this, name, x, table] {
                m_sum_editing = name;
                m_sum_new = false;
                m_sum_typed_name = name;
                m_sum_typed_op = x.op;
                m_sum_typed_field = x.field;
                if (const db::Table* tt = m_schema.table(table)) calc_section(*tt);
            });
            sg->attach(*edit, 3, sr);
            auto del = Gtk::make_managed<Gtk::Button>();
            del->set_icon_name("user-trash-symbolic");
            del->add_css_class("flat");
            del->set_name("tables.summary.delete." + name);
            del->set_tooltip_text("Delete " + curly(name) + " -- Ctrl+Z brings it back");
            del->set_sensitive(can && !editing);
            del->signal_clicked().connect([this, table, name] { drop_summary(table, name); });
            sg->attach(*del, 4, sr);
            ++sr;
        }
        body->append(*sg);
    }

    if (sum_editing) {
        summary_editor(*body, t, sums);
    } else if (!editing) {
        if (calcs.empty() && summaries.empty())
            body->append(*cell("None yet. A calculation is worked out by Acorn for each record -- "
                               "Upper(last_name) & \", \" & first_name -- and shown beside its fields. "
                               "It lives in the solution; the database never sees it. A summary -- Total of balance -- is one value over "
                               "the records (the found ones, after a Find), worked out by the database.", "acorn-status"));
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
        auto addsum = Gtk::make_managed<Gtk::Button>("New Summary\u2026");   // s014b
        addsum->set_name("tables.summary.new");
        addsum->set_sensitive(can);
        addsum->set_tooltip_text(can ? "Count, Total, Average, Minimum or Maximum of a field" : "Open a database first");
        addsum->signal_clicked().connect([this, table] {
            m_sum_new = true;
            m_sum_editing.clear();
            m_sum_typed_name.clear();
            m_sum_typed_field.clear();
            m_sum_typed_op = core::SummaryOp::Total;
            if (const db::Table* tt = m_schema.table(table)) calc_section(*tt);
        });
        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        add->set_halign(Gtk::Align::FILL);
        buttons->append(*add);
        buttons->append(*addsum);
        body->append(*buttons);
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
        for (const auto& [n, x] : summaries) fl += (fl.empty() ? "" : "  ") + n;   // s014b: read like fields
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
        for (const auto& [n, x] : summaries) cols.push_back(n);   // s014b
        auto check = [this, live, save, name_e, expr_e, cols, table, was, first, sums] {
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
            auto v = tt ? db::row_calcs(*tt, *first, others, core::today_iso(), &sums) : std::map<std::string, core::CalcOutcome>{};
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

// ── s014b: the summary editor ───────────────────────────────────────────────

void Shell::summary_editor(Gtk::Box& body, const db::Table& t, const std::map<std::string, core::CalcValue>& sums) {  // calcs: the summary editor
    const std::string table = t.name;
    const std::string was = m_sum_editing;
    static const core::SummaryOp kOps[] = {core::SummaryOp::Count, core::SummaryOp::Total, core::SummaryOp::Average,
                                           core::SummaryOp::Min, core::SummaryOp::Max};
    auto ed = Gtk::make_managed<Gtk::Grid>();
    ed->set_column_spacing(8);
    ed->set_row_spacing(6);
    ed->add_css_class("acorn-calc-editor");
    auto name_e = Gtk::make_managed<Gtk::Entry>();
    name_e->set_name("tables.summary.name");
    name_e->set_placeholder_text("name");
    name_e->set_width_chars(16);
    name_e->set_text(m_sum_typed_name);
    std::vector<Glib::ustring> op_words;
    guint op_at = 0;
    for (std::size_t i = 0; i < std::size(kOps); ++i) {
        op_words.push_back(core::summary_op_label(kOps[i]));
        if (kOps[i] == m_sum_typed_op) op_at = static_cast<guint>(i);
    }
    auto op_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(op_words));
    op_dd->set_name("tables.summary.op");
    op_dd->set_selected(op_at);
    auto names = std::make_shared<std::vector<std::string>>();
    std::vector<Glib::ustring> field_words;
    guint f_at = 0;
    for (const auto& c : t.columns) {
        if (c.type == db::FieldType::Container) continue;   // binary: nothing to sum up
        if (c.name == m_sum_typed_field) f_at = static_cast<guint>(names->size());
        names->push_back(c.name);
        field_words.push_back(c.name + "  (" + db::to_string(c.type) + ")");
    }
    auto f_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(field_words));
    f_dd->set_name("tables.summary.field");
    f_dd->set_hexpand(true);
    if (!names->empty()) f_dd->set_selected(f_at);
    ed->attach(*name_e, 0, 0);
    ed->attach(*cell("="), 1, 0);
    ed->attach(*op_dd, 2, 0);
    ed->attach(*cell("of"), 3, 0);
    ed->attach(*f_dd, 4, 0);
    auto live = cell("", "acorn-status");
    live->set_name("tables.summary.live");
    live->set_wrap(true);
    ed->attach(*live, 0, 1, 5, 1);
    auto how = cell("Worked out by the database over the records Browse shows -- every record, or the found ones after "
                    "a Find. A calculation can read it by name.", "acorn-small");
    how->set_wrap(true);
    ed->attach(*how, 0, 2, 5, 1);
    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->set_name("tables.summary.cancel");
    auto save = Gtk::make_managed<Gtk::Button>(was.empty() ? "Add" : "Save");
    save->set_name("tables.summary.save");
    save->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*save);
    ed->attach(*buttons, 0, 3, 5, 1);
    body.append(*ed);

    // The live line: what's wrong, or the value over every record.
    auto check = [this, live, save, name_e, op_dd, f_dd, names, table, was] {
        m_sum_typed_name = name_e->get_text();
        const auto oi = op_dd->get_selected();
        if (oi < std::size(kOps)) m_sum_typed_op = kOps[oi];
        const auto fi = f_dd->get_selected();
        m_sum_typed_field = fi < names->size() ? (*names)[fi] : std::string{};
        const core::Summary x{table, m_sum_typed_name, m_sum_typed_op, m_sum_typed_field};
        const db::Table* tt = m_schema.table(table);
        std::vector<std::string> fields = fields_of(table), taken;
        for (const auto& [n, c] : m_solution.calcs_of(table)) taken.push_back(n);
        for (const auto& [n, s] : m_solution.summaries_of(table)) if (n != was) taken.push_back(n);
        std::string why = x.name.empty() ? "Give it a name." : core::check_summary(x, fields, taken);
        const db::Column* col = tt ? tt->column(x.field) : nullptr;
        if (why.empty() && col && (x.op == core::SummaryOp::Total || x.op == core::SummaryOp::Average) &&
            col->type != db::FieldType::Integer && col->type != db::FieldType::Number)
            why = std::string(core::summary_op_label(x.op)) + " needs a number -- " + curly(x.field) + " is " +
                  db::to_string(col->type) + ".";
        live->remove_css_class("error");
        if (!why.empty()) {
            live->set_text(why);
            if (!x.name.empty()) live->add_css_class("error");
            save->set_sensitive(false);
            return;
        }
        save->set_sensitive(true);
        if (!m_conn || !tt) { live->set_text("Fine."); return; }
        auto v = db::read_summaries(*m_conn, *tt, {{x.name, x}});
        if (!v) { live->set_text("? -- " + v.error().message); return; }
        const auto shown = core::calc_text((*v)[x.name]);
        live->set_text("Every record: " + (shown.empty() ? std::string("(empty)") : shown));
    };
    check();
    name_e->signal_changed().connect(check);
    op_dd->property_selected().signal_changed().connect(check);
    f_dd->property_selected().signal_changed().connect(check);
    auto close = [this, table] {
        m_sum_new = false;
        m_sum_editing.clear();
        if (const db::Table* tt = m_schema.table(table)) calc_section(*tt);
    };
    cancel->signal_clicked().connect(close);
    auto commit = [this, save, table, was] {
        if (!save->get_sensitive()) return;
        keep_summary(core::Summary{table, m_sum_typed_name, m_sum_typed_op, m_sum_typed_field}, was);
    };
    save->signal_clicked().connect(commit);
    name_e->signal_activate().connect(commit);
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([close](guint key, guint, Gdk::ModifierType) {
        if (key != GDK_KEY_Escape) return false;
        close();
        return true;
    }, false);
    ed->add_controller(keys);
    (void)sums;
}

// ── the verbs ───────────────────────────────────────────────────────────────

void Shell::keep_summary(const core::Summary& x, const std::string& was) {  // calcs: verbs::save_summary, the note, a repaint
    if (!m_journal) return;
    const auto before = m_journal->size();
    const bool could_redo = m_journal->can_redo();
    auto r = db::verbs::save_summary(*m_journal, x, was);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: summary '{}' of '{}' = {}: {}", x.name, x.table, core::summary_says(x),
                 r ? m_journal->undo_label() : r.error().message);
    if (!r) { journal_note("Not kept: " + r.error().message + ".", "error"); return; }
    m_sum_new = false;
    m_sum_editing.clear();
    calcs_changed(x.table);
    if (m_journal->size() != before || could_redo != m_journal->can_redo())
        journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
}

void Shell::drop_summary(const std::string& table, const std::string& name) {  // calcs: verbs::delete_summary
    if (!m_journal) return;
    auto r = db::verbs::delete_summary(*m_journal, table, name);
    if (!r) { journal_note("Not deleted: " + r.error().message + ".", "error"); return; }
    calcs_changed(table);
    journal_note("Deleted the summary " + curly(name) + ". Ctrl+Z brings it back.", "ok");
}

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
