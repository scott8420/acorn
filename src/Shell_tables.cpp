#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>

// Shell_tables.cpp -- the TABLES area (s004c): the read-only schema browser.
// Left: the tables and views of the open connection. Right: the picked one's
// columns (name, declared type, Acorn's field type, null, default, key),
// indexes, links out (its foreign keys) and links in (other tables' foreign
// keys that point at it). Everything is painted from m_schema -- what the
// Connect worker's introspection read -- so nothing here touches the
// connection, and nothing here can write.

namespace acorn {

namespace {

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

// Full width by default -- a grid gives an ellipsizing label almost no room,
// so only the cells that can run long (defaults, link rules) opt in.
Gtk::Label* cell(const std::string& text, const char* css = nullptr, bool selectable = false) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->set_selectable(selectable);
    if (css) l->add_css_class(css);
    return l;
}

Gtk::Label* heading(const std::string& text) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->add_css_class("acorn-section");
    return l;
}

// One section on its own calm panel.
Gtk::Box* section(const std::string& title, Gtk::Widget& body) {
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->add_css_class("acorn-panel");
    box->append(*heading(title));
    box->append(body);
    return box;
}

Gtk::Label* none(const char* text) {
    auto l = cell(text, "acorn-status");
    return l;
}

}  // namespace

void Shell::build_tables_page() {  // tables: the read-only schema browser
    m_tables_page.set_margin(28);

    auto hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    auto title = Gtk::make_managed<Gtk::Label>("Tables");
    title->set_xalign(0.0f);
    title->add_css_class("acorn-hero-title");
    m_tables_where.set_xalign(0.0f);
    m_tables_where.set_wrap(true);
    m_tables_where.add_css_class("acorn-hero-sub");
    hero->append(*title);
    hero->append(m_tables_where);
    m_journal_note.set_xalign(0.0f);
    m_journal_note.set_wrap(true);
    m_journal_note.add_css_class("acorn-status");
    hero->append(m_journal_note);
    // s005b: the first write Acorn offers. Enabled only on a writable
    // connection (sync_undo greys the action).
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    hero->set_hexpand(true);
    m_new_table_btn.set_label("New Table\u2026");
    m_new_table_btn.add_css_class("acorn-primary");
    m_new_table_btn.set_valign(Gtk::Align::CENTER);
    m_new_table_btn.set_action_name("win.new-table");
    m_new_table_btn.set_tooltip_text("Make a new, empty table -- the SQL is shown before it runs");
    top->append(*hero);
    top->append(m_new_table_btn);
    m_tables_page.append(*top);

    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    body->set_vexpand(true);

    // Left: the list.
    auto left = Gtk::make_managed<Gtk::ScrolledWindow>();
    left->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    left->set_size_request(180, -1);
    left->add_css_class("acorn-panel");
    m_table_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_table_list.add_css_class("acorn-table-list");
    m_table_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (m_table_painting || !row) return;
        const int i = row->get_index();
        if (i >= 0 && static_cast<std::size_t>(i) < m_schema.tables.size())
            show_table(m_schema.tables[i].name);
    });
    left->set_child(m_table_list);

    // Right: the detail, scrolling as one.
    auto right = Gtk::make_managed<Gtk::ScrolledWindow>();
    right->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    right->set_hexpand(true);
    right->set_child(m_table_detail);

    body->append(*left);
    body->append(*right);
    m_tables_page.append(*body);

    rebuild_table_list();
    m_stack.add(m_tables_page, "tables", "Tables");
}

void Shell::rebuild_table_list() {  // tables: the left list, after a connect
    m_table_painting = true;
    while (auto* row = m_table_list.get_row_at_index(0)) m_table_list.remove(*row);
    std::size_t tables = 0, views = 0;
    for (auto& t : m_schema.tables) {
        (t.kind == db::Table::Kind::View ? views : tables)++;
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        box->set_margin(6);
        auto icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name(t.kind == db::Table::Kind::View ? "view-reveal-symbolic"
                                                                  : "view-grid-symbolic");
        icon->add_css_class("acorn-dim-icon");
        auto name = cell(t.name);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        name->set_hexpand(true);
        box->append(*icon);
        box->append(*name);
        m_table_list.append(*box);
    }
    m_table_painting = false;

    if (!m_conn) {
        m_tables_where.set_text("Connect to a database on the Connect page to look at its tables.");
    } else {
        m_tables_where.set_text(m_conn_label + " · " + std::to_string(tables) + " table" +
                                (tables == 1 ? "" : "s") + ", " + std::to_string(views) + " view" +
                                (views == 1 ? "" : "s") +
                                (m_writable ? " · made by Acorn: changes go through the journal"
                                            : " · read-only"));
    }
    // Keep the shown table if it survived the reconnect, else the first one.
    std::string pick = m_schema.table(m_table_sel) ? m_table_sel : std::string{};
    if (pick.empty() && !m_schema.tables.empty()) pick = m_schema.tables.front().name;
    show_table(pick);
}

void Shell::open_table(const std::string& name) {  // tables: go there, that table selected
    m_stack.set_visible_child("tables");
    show_table(name);
}

void Shell::show_table(const std::string& name) {  // tables: paint one table's detail
    m_calc_box = nullptr;   // s014a: the section goes with the rest
    while (auto* c = m_table_detail.get_first_child()) m_table_detail.remove(*c);
    m_table_sel.clear();
    const db::Table* t = m_schema.table(name);

    // Keep the list's selection in step (without re-entering).
    m_table_painting = true;
    m_table_list.unselect_all();
    for (std::size_t i = 0; t && i < m_schema.tables.size(); ++i)
        if (m_schema.tables[i].name == name)
            if (auto* row = m_table_list.get_row_at_index(static_cast<int>(i)))
                m_table_list.select_row(*row);
    m_table_painting = false;

    if (!t) {
        auto empty = Gtk::make_managed<Gtk::Label>(
            m_conn ? "This database has no tables yet." : "No database is open.");
        empty->add_css_class("acorn-status");
        empty->set_margin(24);
        m_table_detail.append(*empty);
        sync_undo();   // s007b: Design Table follows the shown table
        return;
    }
    m_table_sel = name;
    sync_undo();
    if (auto lg = log::get(log::Area::Shell)) lg->info("tables: show '{}'", name);

    // Title line.
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto tl = Gtk::make_managed<Gtk::Label>(t->name);
    tl->set_xalign(0.0f);
    tl->add_css_class("acorn-table-title");
    top->append(*tl);
    if (t->kind == db::Table::Kind::View) {
        auto b = Gtk::make_managed<Gtk::Label>("VIEW");
        b->add_css_class("acorn-badge");
        b->set_valign(Gtk::Align::CENTER);
        top->append(*b);
    }
    {   // s006b: straight to its records
        auto spacer = Gtk::make_managed<Gtk::Box>();
        spacer->set_hexpand(true);
        top->append(*spacer);
        auto br = Gtk::make_managed<Gtk::Button>("Browse Records");
        br->set_valign(Gtk::Align::CENTER);
        br->set_tooltip_text("Show this table's records in the Browse area");
        const std::string n = t->name;
        br->signal_clicked().connect([this, n] { open_browse(n); });
        top->append(*br);
        if (t->kind == db::Table::Kind::Table) {   // s007b: the designer
            auto ds = Gtk::make_managed<Gtk::Button>("Design\u2026");
            ds->set_valign(Gtk::Align::CENTER);
            ds->set_name("shell.tables.design");
            ds->set_action_name("win.design-table");
            ds->set_tooltip_text(m_writable && m_journal
                ? "Add, rename and delete fields and indexes -- the SQL is shown before it runs"
                : "For now Acorn changes only database files it made itself (New Database File, Ctrl+N)");
            top->append(*ds);
        }
    }
    m_table_detail.append(*top);

    // Columns.
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_column_spacing(14);
    grid->set_row_spacing(5);
    int r = 0;
    for (int c = 0; const char* h : {"Name", "Type", "Field", "Null", "Default", "Key"})
        grid->attach(*cell(h, "acorn-th"), c++, r);
    ++r;
    for (auto& col : t->columns) {
        std::string key;
        if (col.pk_position > 0) {
            key = t->primary_key().size() > 1 ? "PK " + std::to_string(col.pk_position) : "PK";
        }
        if (col.auto_increment) key += key.empty() ? "auto" : " · auto";
        grid->attach(*cell(col.name, nullptr, true), 0, r);
        grid->attach(*cell(col.decl_type.empty() ? "—" : col.decl_type, "acorn-mono"), 1, r);
        grid->attach(*cell(db::to_string(col.type), "acorn-field"), 2, r);
        grid->attach(*cell(col.nullable ? "yes" : "NOT NULL", col.nullable ? "acorn-status" : nullptr), 3, r);
        auto def = cell(col.default_sql ? *col.default_sql : "—",
                        col.default_sql ? "acorn-mono" : "acorn-status");
        def->set_ellipsize(Pango::EllipsizeMode::END);
        def->set_max_width_chars(28);
        if (col.default_sql) def->set_tooltip_text(*col.default_sql);
        grid->attach(*def, 4, r);
        grid->attach(*cell(key, "acorn-key"), 5, r);
        ++r;
    }
    m_table_detail.append(*section(std::to_string(t->columns.size()) + " column" +
                                   (t->columns.size() == 1 ? "" : "s"), *grid));

    // s014a: calculations -- the solution's, worked out by Acorn (views too).
    {
        auto placeholder = Gtk::make_managed<Gtk::Box>();
        m_calc_box = section("Calculations", *placeholder);
        m_table_detail.append(*m_calc_box);
        calc_section(*t);
    }

    if (t->kind == db::Table::Kind::View) return;   // views carry no keys, indexes or links

    // Indexes.
    auto ix = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    if (t->indexes.empty()) ix->append(*none("No indexes."));
    for (auto& i : t->indexes) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto n = cell(i.name.empty() ? "(unnamed)" : i.name, "acorn-mono");
        n->set_size_request(200, -1);
        row->append(*n);
        auto cols = cell("(" + join(i.columns) + ")");
        cols->set_hexpand(true);
        row->append(*cols);
        for (auto [on, label] : {std::pair{i.primary, "PRIMARY"}, {i.unique && !i.primary, "UNIQUE"},
                                 {i.partial, "PARTIAL"}})
            if (on) {
                auto b = Gtk::make_managed<Gtk::Label>(label);
                b->add_css_class("acorn-badge");
                b->set_valign(Gtk::Align::CENTER);
                row->append(*b);
            }
        ix->append(*row);
    }
    m_table_detail.append(*section("Indexes", *ix));

    // Links: out (this table's foreign keys) and in (who points here). A
    // linked table's name is a button -- one click to go and look at it.
    // Both directions read the same way round -- FROM the foreign key's
    // columns TO what they point at -- and the OTHER table is the button.
    //   out:  (customer_id) -> [customers] (id)
    //   in:   [orders] (customer_id) -> (id)
    const auto link_row = [this](bool in, const std::string& other, const std::string& from_cols,
                                 const std::string& to_cols, const db::ForeignKey& fk) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto go = Gtk::make_managed<Gtk::Button>(other);
        go->add_css_class("acorn-link");
        go->set_has_frame(false);
        go->set_tooltip_text("Show " + other);
        go->signal_clicked().connect([this, other] { show_table(other); });
        if (in) row->append(*go);
        row->append(*cell(from_cols, "acorn-mono"));
        row->append(*cell("→", "acorn-status"));
        if (!in) row->append(*go);
        row->append(*cell(to_cols, "acorn-mono"));
        std::string rules;
        if (!fk.on_update.empty() && fk.on_update != "NO ACTION") rules += "on update " + fk.on_update;
        if (!fk.on_delete.empty() && fk.on_delete != "NO ACTION")
            rules += (rules.empty() ? "" : " · ") + std::string("on delete ") + fk.on_delete;
        if (!fk.name.empty()) rules += (rules.empty() ? "" : " · ") + fk.name;
        if (!rules.empty()) {
            auto r = cell("  " + rules, "acorn-status");
            r->set_ellipsize(Pango::EllipsizeMode::END);
            r->set_hexpand(true);
            r->add_css_class("acorn-small");
            row->append(*r);
        }
        return row;
    };
    auto links = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    links->append(*cell("Out -- this table points at", "acorn-field-label"));
    if (t->foreign_keys.empty()) links->append(*none("Nothing."));
    for (auto& fk : t->foreign_keys)
        links->append(*link_row(false, fk.ref_table, "(" + join(fk.columns) + ")",
                                "(" + join(m_schema.ref_columns(fk)) + ")", fk));
    auto sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);
    sep->set_margin_bottom(4);
    links->append(*sep);
    links->append(*cell("In -- these point at this table", "acorn-field-label"));
    const auto in = m_schema.links_to(t->name);
    if (in.empty()) links->append(*none("Nothing."));
    for (auto& l : in)
        links->append(*link_row(true, l.from->name, "(" + join(l.fk->columns) + ")",
                                "(" + join(m_schema.ref_columns(*l.fk)) + ")", *l.fk));
    m_table_detail.append(*section("Links", *links));
}

}  // namespace acorn
