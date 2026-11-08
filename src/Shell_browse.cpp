#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/sizegroup.h>

// Shell_browse.cpp -- the BROWSE area (s006b): one table's records, a page at
// a time, as calm cards (the default, REQUIREMENTS §2) or a dense grid. The
// picked record is kept by its journal id, so it survives a view toggle, a
// re-sort and a repaint -- s006c's edits name it.
//
// Reads only: db::read_page is SELECTs through the Gate. Nothing here writes.

namespace acorn {

namespace {

constexpr std::int64_t kPage = 50;

Gtk::Label* text(const std::string& s, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    if (css) l->add_css_class(css);
    return l;
}

// A value cell: one line, ellipsized, the whole value as tooltip when cut.
Gtk::Label* value_label(const std::string& s, int max_chars) {
    std::string one = s;
    for (auto& ch : one) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    auto l = text(one, s.empty() ? "acorn-null" : nullptr);
    l->set_ellipsize(Pango::EllipsizeMode::END);
    l->set_max_width_chars(max_chars);
    l->set_single_line_mode(true);
    if (static_cast<int>(one.size()) > max_chars) l->set_tooltip_text(s);
    return l;
}

}  // namespace

void Shell::build_browse_page() {  // browse: the Browse area
    m_browse_page.set_margin(28);

    // Hero + the table picker on one line.
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    hero->set_hexpand(true);
    auto title = text("Browse", "acorn-hero-title");
    m_browse_where.set_xalign(0.0f);
    m_browse_where.set_wrap(true);
    m_browse_where.add_css_class("acorn-hero-sub");
    hero->append(*title);
    hero->append(m_browse_where);
    m_browse_note.set_xalign(0.0f);
    m_browse_note.set_wrap(true);
    m_browse_note.add_css_class("acorn-status");
    m_browse_note.set_visible(false);
    hero->append(m_browse_note);
    top->append(*hero);

    m_browse_names = Gtk::StringList::create({});
    m_browse_pick.set_model(m_browse_names);
    m_browse_pick.set_valign(Gtk::Align::CENTER);
    m_browse_pick.set_tooltip_text("The table (or view) whose records are shown");
    m_browse_pick.set_name("shell.browse.pick");
    m_browse_pick.property_selected().signal_changed().connect([this] {
        if (m_browse_painting) return;
        const auto i = m_browse_pick.get_selected();
        if (i != GTK_INVALID_LIST_POSITION && i < m_schema.tables.size()) browse_table(m_schema.tables[i].name);
    });
    top->append(m_browse_pick);
    m_browse_page.append(*top);

    // The bar: Cards | Grid on the left, the pager on the right.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto views = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    views->add_css_class("linked");
    views->add_css_class("acorn-view-toggle");
    m_browse_cards_btn.set_label("Cards");
    m_browse_cards_btn.set_tooltip_text("One card per record");
    m_browse_grid_btn.set_label("Grid");
    m_browse_grid_btn.set_tooltip_text("A dense grid -- click a heading to sort");
    m_browse_grid_btn.set_group(m_browse_cards_btn);
    // s010a: Form -- one record through its layout, edited in place.
    m_browse_form_btn.set_label("Form");
    m_browse_form_btn.set_tooltip_text("One record at a time, through its layout -- edit it in place");
    m_browse_form_btn.set_group(m_browse_cards_btn);
    // s012a: List -- the page through the same layout, a row per record.
    m_browse_list_btn.set_label("List");
    m_browse_list_btn.set_tooltip_text("Every record of the page through its layout, a row each, edited in place -- "
                                       "the header above, the footer below. Double-click a row for its Form");
    m_browse_list_btn.set_group(m_browse_cards_btn);
    // s015a: Preview -- the same layout laid on pages, as it will print.
    m_browse_preview_btn.set_label("Preview");
    m_browse_preview_btn.set_tooltip_text("The records through the List's layout, on paper -- the header and footer on "
                                          "every page. Print or save a PDF from here");
    m_browse_preview_btn.set_group(m_browse_cards_btn);
    m_browse_cards_btn.set_active(true);
    auto to_view = [this](Gtk::ToggleButton& b, BrowseView v) {
        b.signal_toggled().connect([this, &b, v] {
            if (!b.get_active() || m_browse_view == v) return;
            m_browse_view = v;
            m_form_focus.clear();
            m_finding = false;   // s013a: another view leaves the Find screen
            // The Form shows one record: the picked one, else the page's first.
            if (v == BrowseView::Form && m_browse_sel_index < 0 && !m_browse_data.ids.empty() && !m_record_new) {
                m_browse_sel_index = 0;
                m_browse_sel = m_browse_data.ids[0];
            }
            paint_browse();
            paint_record();
            sync_undo();
        });
    };
    to_view(m_browse_cards_btn, BrowseView::Cards);
    to_view(m_browse_grid_btn, BrowseView::Grid);
    to_view(m_browse_form_btn, BrowseView::Form);
    to_view(m_browse_list_btn, BrowseView::List);
    to_view(m_browse_preview_btn, BrowseView::Preview);
    views->append(m_browse_cards_btn);
    views->append(m_browse_grid_btn);
    views->append(m_browse_form_btn);
    views->append(m_browse_list_btn);
    views->append(m_browse_preview_btn);
    bar->append(*views);
    // s006c: the record verbs (greyed by sync_undo when the table can't take them).
    m_browse_new.set_label("New Record");
    m_browse_new.add_css_class("acorn-primary");
    m_browse_new.set_action_name("win.new-record");
    m_browse_new.set_margin_start(12);
    m_browse_delete.set_label("Delete Record");
    m_browse_delete.set_action_name("win.delete-record");
    m_browse_delete.set_tooltip_text("Delete the picked record -- Ctrl+Z brings it back");
    bar->append(m_browse_new);
    bar->append(m_browse_delete);
    // s013a: Find -- query by form; Show All only while a found set stands.
    m_browse_find.set_label("Find");
    m_browse_find.set_action_name("win.find");
    m_browse_find.set_tooltip_text("Find records: type what to look for in the layout's boxes (Ctrl+F)");
    m_browse_find.set_margin_start(12);
    m_browse_show_all.set_label("Show All");
    m_browse_show_all.set_action_name("win.show-all");
    m_browse_show_all.set_tooltip_text("Every record again (Ctrl+J)");
    m_browse_show_all.set_visible(false);
    bar->append(m_browse_find);
    bar->append(m_browse_show_all);
    // s017a: Scripts -- the shown table's, filled each time it opens.
    m_browse_scripts.set_label("Scripts");
    m_browse_scripts.set_tooltip_text("Run one of this table's scripts -- Ctrl+Z undoes what it wrote, in one step");
    m_browse_scripts.set_margin_start(12);
    m_browse_scripts.set_popover(m_browse_scripts_pop);
    m_browse_scripts_pop.add_css_class("acorn-graph-pop");   // the skin's popover ground
    m_browse_scripts.set_create_popup_func([this] { fill_scripts_menu(); });   // filled before each opening
    bar->append(m_browse_scripts);
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    bar->append(*spacer);
    m_browse_range.add_css_class("acorn-status");
    m_browse_range.set_valign(Gtk::Align::CENTER);
    m_browse_prev.set_icon_name("go-previous-symbolic");
    m_browse_prev.set_tooltip_text("The previous page");
    m_browse_next.set_icon_name("go-next-symbolic");
    m_browse_next.set_tooltip_text("The next page");
    m_browse_prev.signal_clicked().connect([this] {
        if (m_browse_view == BrowseView::Form) { form_step(-1); return; }   // s010a: a record at a time
        m_browse_req.offset = std::max<std::int64_t>(0, m_browse_req.offset - m_browse_req.limit);
        m_browse_sel_index = -1;
        load_browse_page();
    });
    m_browse_next.signal_clicked().connect([this] {
        if (m_browse_view == BrowseView::Form) { form_step(+1); return; }
        if (m_browse_req.offset + m_browse_req.limit >= m_browse_data.total) return;
        m_browse_req.offset += m_browse_req.limit;
        m_browse_sel_index = -1;
        load_browse_page();
    });
    bar->append(m_browse_range);
    bar->append(m_browse_prev);
    bar->append(m_browse_next);
    m_browse_page.append(*bar);

    m_browse_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    m_browse_scroll.set_vexpand(true);
    m_browse_scroll.set_hexpand(true);
    // s006c: the record panel beside the records.
    m_record_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_record_scroll.set_size_request(320, -1);
    m_record_scroll.set_hexpand(false);   // its boxes expand inside it, not it into the records
    m_record_scroll.add_css_class("acorn-panel");
    m_record_scroll.set_visible(false);
    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    body->set_vexpand(true);
    body->append(m_browse_scroll);
    body->append(m_record_scroll);
    m_browse_page.append(*body);

    rebuild_browse_pick();
    m_stack.add(m_browse_page, "browse", "Browse");
}

const db::Table* Shell::browse_info() const {  // browse: the shown table
    return m_browse_table.empty() ? nullptr : m_schema.table(m_browse_table);
}

void Shell::rebuild_browse_pick() {  // browse: the picker, after a connect / a step
    m_browse_painting = true;
    std::vector<Glib::ustring> names;
    guint at = GTK_INVALID_LIST_POSITION;
    for (std::size_t i = 0; i < m_schema.tables.size(); ++i) {
        const auto& t = m_schema.tables[i];
        names.push_back(t.kind == db::Table::Kind::View ? t.name + " (view)" : t.name);
        if (t.name == m_browse_table) at = static_cast<guint>(i);
    }
    m_browse_names->splice(0, m_browse_names->get_n_items(), names);
    m_browse_pick.set_sensitive(!names.empty());
    // The shown table is gone (undone, or another connection): show the first.
    if (at == GTK_INVALID_LIST_POSITION) {
        m_browse_table.clear();
        m_browse_sel.clear();
        if (!m_schema.tables.empty()) { at = 0; m_browse_table = m_schema.tables[0].name; m_browse_req = {0, kPage}; }
    }
    m_browse_pick.set_selected(at);
    m_browse_painting = false;
    load_browse_page();
}

void Shell::browse_table(const std::string& name) {  // browse: this table's first page
    if (name == m_browse_table) return;
    m_browse_table = name;
    m_form_focus.clear();
    m_finding = false;   // s013a: a find belongs to its table
    m_found.clear();
    m_found_typed.clear();
    m_found_table.clear();
    m_browse_req = {0, kPage};
    m_browse_sel.clear();
    m_browse_sel_index = -1;
    m_record_new = false;
    browse_note("", "");
    if (auto lg = log::get(log::Area::Shell)) lg->info("browse: show '{}'", name);
    load_browse_page();
}

void Shell::open_browse(const std::string& name) {  // browse: go there, that table showing
    m_stack.set_visible_child("browse");
    for (std::size_t i = 0; i < m_schema.tables.size(); ++i)
        if (m_schema.tables[i].name == name) {
            m_browse_pick.set_selected(static_cast<guint>(i));   // -> browse_table
            break;
        }
    browse_table(name);
}

void Shell::load_browse_page(bool keep_panel) {  // browse: read the page, repaint
    m_browse_data = {};
    const db::Table* t = browse_info();
    if (!m_conn || !t) {
        m_browse_where.set_text(m_conn ? "This database has no tables yet."
                                       : "Connect to a database first -- its records show here.");
        m_record_new = false;
        paint_browse();
        paint_record();
        sync_undo();
        return;
    }
    // On the UI thread: 50 records from a local file is instant. A server's
    // page moves to a worker when servers become writable (handoff).
    // s013a: a found set reads only what matches -- the find run again, so
    // the page, the count and an edited record follow it.
    const bool found = found_here();
    auto p = found ? db::read_found(*m_conn, *t, m_found, m_browse_req, &m_schema) : db::read_page(*m_conn, *t, m_browse_req);
    if (!p && found) {   // the find no longer fits the table (a field undone away): every record again
        browse_note("The find no longer fits “" + t->name + "” (" + p.error().message + ") -- showing every record.", "error");
        m_found.clear();
        m_found_table.clear();
        load_browse_page(keep_panel);
        return;
    }
    if (found) {
        auto all = db::read_page(*m_conn, *t, {0, 1});
        m_found_of = all ? all->total : 0;
    }
    if (!p) {
        m_browse_where.set_text("Could not read “" + t->name + "”: " + p.error().message);
        if (auto lg = log::get(log::Area::Shell)) lg->warn("browse: read '{}': {}", t->name, p.error().message);
        paint_browse();
        return;
    }
    // Past the end (records went away): step back to the last page.
    if (p->rows.empty() && p->total > 0 && m_browse_req.offset > 0) {
        m_browse_req.offset = ((p->total - 1) / m_browse_req.limit) * m_browse_req.limit;
        load_browse_page(keep_panel);
        return;
    }
    m_browse_data = std::move(*p);
    read_browse_summaries();   // s014b: over the same found set
    const auto why = db::Store::why_not_writable(*t);
    std::string where = "“" + t->name + "” -- " + std::to_string(m_browse_data.total) + " record" +
                        (m_browse_data.total == 1 ? "" : "s");
    if (found)   // s013a: "found 5 of 12 · last_name has “a”"
        where = "“" + t->name + "” -- found " + std::to_string(m_browse_data.total) + " of " +
                std::to_string(m_found_of) + " · " + db::find_summary(m_found);
    if (!m_writable)       where += " · read-only";
    else if (!why.empty()) where += " · read-only: " + why;
    // s008b: records that stop a link are marked -- say so, and how to read
    // them. Re-read each time: a record fixed in the panel drops its mark.
    if (m_marks.count(t->name)) refresh_marks();
    if (auto m = m_marks.find(t->name); m != m_marks.end() && !m->second.ids.empty())
        where += " · ⚠ " + std::to_string(m->second.ids.size()) + (m->second.ids.size() == 1 ? " record points" : " records point") +
                 " at a “" + m->second.parent + "” record that isn't there (marked -- hover one to see why)";
    m_browse_where.set_text(where);
    // Keep the picked record if it is on this page.
    m_browse_sel_index = -1;
    for (std::size_t i = 0; i < m_browse_data.ids.size(); ++i)
        if (!m_browse_sel.empty() && m_browse_data.ids[i] == m_browse_sel) m_browse_sel_index = static_cast<int>(i);
    // s010a: the Form always shows a record -- the picked one gone (deleted,
    // another page), the page's first.
    if (m_browse_view == BrowseView::Form && m_browse_sel_index < 0 && !m_browse_data.ids.empty() && !m_record_new) {
        m_browse_sel_index = 0;
        m_browse_sel = m_browse_data.ids[0];
    }
    // s010a: a field just saved from the Form keeps its boxes (the one Tab
    // moved to holds the focus); the values under them are fresh.
    // s013a: ...unless the picked record has left the page (edited out of a
    // found set): its boxes would edit a record no longer shown.
    if (keep_panel && (m_browse_view == BrowseView::Form || m_browse_view == BrowseView::List) &&
        (m_browse_sel_index >= 0 || m_browse_sel.empty()) && !m_finding) {   // s012c: + List
        refresh_calc_labels();   // s014a: the values under the boxes are fresh -- so are the calculations
        sync_undo();
        return;
    }
    paint_browse();
    // s013a: a record edited out of a found set takes its panel with it.
    if (!keep_panel || (m_browse_sel_index < 0 && !m_browse_sel.empty())) paint_record();
    sync_undo();
}

void Shell::paint_browse() {  // browse: cards or grid
    ++m_view_gen;   // the old cards / grid say nothing from here on
    m_calc_shown.clear();   // s014a: their calculation labels go with them
    m_browse_range.set_text(browse_info() ? db::page_range(m_browse_data) : "");
    m_browse_prev.set_sensitive(m_browse_req.offset > 0);
    m_browse_next.set_sensitive(m_browse_req.offset + static_cast<std::int64_t>(m_browse_data.rows.size()) <
                                m_browse_data.total);
    m_browse_prev.set_tooltip_text(m_browse_view == BrowseView::Form ? "The previous record" : "The previous page");
    m_browse_next.set_tooltip_text(m_browse_view == BrowseView::Form ? "The next record" : "The next page");
    if (m_browse_view == BrowseView::Form && browse_info() && m_browse_sel_index >= 0) {   // s010a: "Record 7 of 312"
        const auto at = m_browse_req.offset + m_browse_sel_index;
        m_browse_range.set_text("Record " + std::to_string(at + 1) + " of " + std::to_string(m_browse_data.total));
        m_browse_prev.set_sensitive(at > 0);
        m_browse_next.set_sensitive(at + 1 < m_browse_data.total);
    }
    m_browse_cards_btn.set_sensitive(browse_info() != nullptr);
    m_browse_grid_btn.set_sensitive(browse_info() != nullptr);
    m_browse_form_btn.set_sensitive(browse_info() != nullptr);
    m_browse_list_btn.set_sensitive(browse_info() != nullptr);
    m_browse_preview_btn.set_sensitive(browse_info() != nullptr);
    m_browse_scripts.set_sensitive(browse_info() != nullptr && m_journal != nullptr && !m_script_run);   // s017a
    m_browse_show_all.set_visible(found_here());
    if (m_finding && browse_info()) {   // s013a: the Find screen
        m_browse_prev.set_sensitive(false);
        m_browse_next.set_sensitive(false);
        m_browse_range.set_text("Find");
        paint_find();
        return;
    }
    if (!browse_info() || m_browse_data.rows.empty()) {
        auto empty = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        empty->add_css_class("acorn-panel");
        empty->set_valign(Gtk::Align::START);
        empty->append(*text(browse_info() ? "No records in this table yet." : "Nothing to show.", "acorn-status"));
        m_browse_scroll.set_child(*empty);
        return;
    }
    switch (m_browse_view) {
        case BrowseView::Cards: paint_cards(); break;
        case BrowseView::Grid:  paint_grid();  break;
        case BrowseView::Form:  paint_form();  break;
        case BrowseView::List:  paint_list();  break;
        case BrowseView::Preview: paint_preview(); break;
    }
}

void Shell::paint_cards() {  // browse: one calm card per record
    const db::Table* t = browse_info();
    auto flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_selection_mode(Gtk::SelectionMode::SINGLE);
    flow->set_homogeneous(true);
    flow->set_min_children_per_line(1);
    flow->set_max_children_per_line(6);
    flow->set_row_spacing(12);
    flow->set_column_spacing(12);
    flow->set_valign(Gtk::Align::START);
    flow->add_css_class("acorn-cards");
    flow->set_name("shell.browse.cards_view");

    for (std::size_t r = 0; r < m_browse_data.rows.size(); ++r) {
        const auto& row = m_browse_data.rows[r];
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        card->add_css_class("acorn-card");
        auto head = text(db::card_title(*t, row), "acorn-card-title");
        head->set_ellipsize(Pango::EllipsizeMode::END);
        const std::string mark = r < m_browse_data.ids.size() ? mark_of(t->name, m_browse_data.ids[r]) : std::string{};
        if (!mark.empty()) {   // s008b: an amber edge and a hint
            card->add_css_class("acorn-marked");
            auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            head->set_hexpand(true);
            line->append(*head);
            auto m = text("⚠", "acorn-mark");
            m->set_tooltip_text(mark);
            line->append(*m);
            line->set_tooltip_text(mark);
            card->append(*line);
        } else {
            card->append(*head);
        }
        auto g = Gtk::make_managed<Gtk::Grid>();
        g->set_column_spacing(10);
        g->set_row_spacing(3);
        for (std::size_t c = 0; c < t->columns.size() && c < row.size(); ++c) {
            const auto& col = t->columns[c];
            auto name = text(col.name, "acorn-field-name");
            name->set_valign(Gtk::Align::START);
            g->attach(*name, 0, static_cast<int>(c));
            auto v = value_label(db::cell_text(row[c], col.type), 28);
            v->set_hexpand(true);
            g->attach(*v, 1, static_cast<int>(c));
        }
        // s014a: the table's calculations, after its fields -- worked out, never typed in.
        int at = static_cast<int>(t->columns.size());
        for (const auto& [cname, oc] : calcs_of_row(r)) {
            auto name = text(cname, "acorn-field-name");
            name->add_css_class("acorn-calc-name");
            name->set_valign(Gtk::Align::START);
            name->set_tooltip_text("Worked out: " + m_solution.calc(t->name, cname)->expr);
            g->attach(*name, 0, at);
            auto v = calc_label(oc, 28);
            v->set_hexpand(true);
            g->attach(*v, 1, at++);
        }
        card->append(*g);
        flow->append(*card);
    }
    const unsigned gen = m_view_gen;
    flow->signal_selected_children_changed().connect([this, flow, gen] {
        if (m_browse_painting || gen != m_view_gen) return;
        auto sel = flow->get_selected_children();
        browse_select(sel.empty() ? -1 : sel.front()->get_index());
    });
    m_browse_painting = true;
    if (m_browse_sel_index >= 0)
        if (auto* ch = flow->get_child_at_index(m_browse_sel_index)) flow->select_child(*ch);
    m_browse_painting = false;
    browse_show(*flow);   // s014b: the summaries above
}

void Shell::paint_grid() {  // browse: the dense grid, sortable headers
    const db::Table* t = browse_info();
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    outer->add_css_class("acorn-panel");
    outer->add_css_class("acorn-grid");
    outer->set_valign(Gtk::Align::START);
    outer->set_halign(Gtk::Align::START);

    // One size group per column: the heading and every cell under it line up.
    std::vector<Glib::RefPtr<Gtk::SizeGroup>> cols;
    for (std::size_t c = 0; c < t->columns.size(); ++c)
        cols.push_back(Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL));

    auto header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    header->add_css_class("acorn-grid-head");
    for (std::size_t c = 0; c < t->columns.size(); ++c) {
        const auto& col = t->columns[c];
        std::string label = col.name;
        if (m_browse_req.sort == col.name) label += m_browse_req.descending ? "  ▾" : "  ▴";
        auto b = Gtk::make_managed<Gtk::Button>();
        auto bl = text(label);
        bl->set_ellipsize(Pango::EllipsizeMode::END);
        bl->set_max_width_chars(24);
        bl->set_width_chars(std::min<int>(static_cast<int>(g_utf8_strlen(label.c_str(), -1)), 24));
        b->set_child(*bl);
        b->add_css_class("flat");
        b->add_css_class("acorn-grid-heading");
        b->set_tooltip_text("Sort by " + col.name + (col.pk_position > 0 ? " (key)" : "") + " -- again to reverse");
        const std::string name = col.name;
        b->signal_clicked().connect([this, name] {
            if (m_browse_req.sort == name) m_browse_req.descending = !m_browse_req.descending;
            else { m_browse_req.sort = name; m_browse_req.descending = false; }
            m_browse_req.offset = 0;
            load_browse_page();
        });
        cols[c]->add_widget(*b);
        header->append(*b);
    }
    // s014a: the calculations after the fields -- a heading that doesn't sort
    // (Acorn works them out a page at a time; the database can't order by them).
    const auto& calcs = m_solution.calcs_of(t->name);
    for (const auto& [cname, calc] : calcs) {
        cols.push_back(Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL));
        auto hl = text(cname);
        hl->set_ellipsize(Pango::EllipsizeMode::END);
        hl->set_max_width_chars(24);
        hl->set_width_chars(std::min<int>(static_cast<int>(g_utf8_strlen(cname.c_str(), -1)), 24));
        hl->add_css_class("acorn-grid-heading");
        hl->add_css_class("acorn-calc-name");
        hl->set_tooltip_text("Worked out: " + calc.expr + " -- not sortable");
        cols.back()->add_widget(*hl);
        header->append(*hl);
    }
    outer->append(*header);

    auto list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::SINGLE);
    list->add_css_class("acorn-grid-rows");
    list->set_name("shell.browse.grid_view");
    for (std::size_t r = 0; r < m_browse_data.rows.size(); ++r) {
        const auto& row = m_browse_data.rows[r];
        auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        const std::string mark = r < m_browse_data.ids.size() ? mark_of(t->name, m_browse_data.ids[r]) : std::string{};
        if (!mark.empty()) {   // s008b: an amber edge and a hint
            line->add_css_class("acorn-marked");
            line->set_tooltip_text(mark);
        }
        for (std::size_t c = 0; c < t->columns.size() && c < row.size(); ++c) {
            const std::string shown = db::cell_text(row[c], t->columns[c].type);
            auto v = value_label(shown, 24);
            // Ask for the text's width (up to 24): with the record panel open
            // the grid scrolls sideways instead of squeezing every cell to "…".
            v->set_width_chars(std::min<int>(static_cast<int>(g_utf8_strlen(shown.c_str(), -1)), 24));
            v->add_css_class("acorn-grid-cell");
            const auto ft = t->columns[c].type;
            if (ft == db::FieldType::Integer || ft == db::FieldType::Number) v->set_xalign(1.0f);
            cols[c]->add_widget(*v);
            line->append(*v);
        }
        std::size_t ci = t->columns.size();
        for (const auto& [cname, oc] : calcs_of_row(r)) {   // s014a
            auto v = calc_label(oc, 24);
            const std::string shown = core::calc_text(oc);
            v->set_width_chars(std::min<int>(static_cast<int>(g_utf8_strlen(shown.c_str(), -1)), 24));
            v->add_css_class("acorn-grid-cell");
            if (oc.ok() && std::holds_alternative<double>(oc.value)) v->set_xalign(1.0f);
            if (ci < cols.size()) cols[ci++]->add_widget(*v);
            line->append(*v);
        }
        list->append(*line);
    }
    const unsigned gen = m_view_gen;
    list->signal_row_selected().connect([this, gen](Gtk::ListBoxRow* r) {
        if (m_browse_painting || gen != m_view_gen) return;
        browse_select(r ? r->get_index() : -1);
    });
    m_browse_painting = true;
    if (m_browse_sel_index >= 0)
        if (auto* r = list->get_row_at_index(m_browse_sel_index)) list->select_row(*r);
    m_browse_painting = false;
    outer->append(*list);
    browse_show(*outer);   // s014b: the summaries above
}

void Shell::browse_select(int index) {  // browse: the picked record
    m_browse_sel_index = index;
    m_browse_sel = (index >= 0 && static_cast<std::size_t>(index) < m_browse_data.ids.size())
                       ? m_browse_data.ids[static_cast<std::size_t>(index)] : std::string{};
    if (auto lg = log::get(log::Area::Shell)) lg->debug("browse: select {} '{}'", index, m_browse_sel);
    if (index >= 0) m_record_new = false;
    paint_record();
    sync_undo();
}

}  // namespace acorn
