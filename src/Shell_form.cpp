#include "Shell.hpp"
#include "Log.hpp"

#include <glibmm/datetime.h>
#include <gtkmm/calendar.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/fixed.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>

#include <cstdio>

// Shell_form.cpp -- the FORM view (s010a): Browse's third view. One record,
// drawn through its table's layout (core::Layout, kept in the solution) and
// edited in place -- each box saves as the record panel's do (Enter / leave =
// one step, Esc puts it back, Ctrl+Z undoes). The previous / next arrows step
// a record at a time, across pages.
//
// The layout: the one kept in the solution for the table, else the DEFAULT --
// "<table> Form", computed from the table as it is now (a label left, a box
// right, a row per field). A field object shows the field's kind from the live
// table; a field the table no longer has is a dashed "missing field" box.
//
// Nothing here writes SQL; saves go through commit_field -> verbs::set_field.

namespace acorn {

namespace {

Gtk::Label* form_label(const std::string& s, const char* css) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    l->set_ellipsize(Pango::EllipsizeMode::END);
    l->set_single_line_mode(true);
    if (css) l->add_css_class(css);
    return l;
}

bool yes(const db::Value& v) {
    if (auto i = v.as_int()) return *i != 0;
    if (const auto* t = v.as_text()) return *t == "t" || *t == "true" || *t == "1";
    return false;
}

// "2026-11-07" -> a day; nullopt when the text isn't a date.
std::optional<Glib::DateTime> day_of(const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
    auto dt = Glib::DateTime::create_local(y, m, d, 0, 0, 0.0);
    if (!dt) return std::nullopt;
    return dt;
}

}  // namespace

core::Layout Shell::form_layout(bool* kept) const {  // form: the picked layout -- kept, else the default
    if (kept) *kept = false;
    const db::Table* t = browse_info();
    if (!t) return {};
    // s010b: the layout last picked (here or in Layouts), else the default.
    return layout_of(t->name, picked_layout(t->name), kept);
}

void Shell::paint_form() {  // form: the picked record through its layout
    const db::Table* t = browse_info();
    const unsigned gen = m_view_gen;
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    outer->set_halign(Gtk::Align::START);
    outer->set_valign(Gtk::Align::START);
    outer->set_name("shell.browse.form_view");

    const bool picked = m_browse_sel_index >= 0 && static_cast<std::size_t>(m_browse_sel_index) < m_browse_data.rows.size();
    if (!picked) {
        outer->append(*form_label(m_record_new ? "A new record -- fill it in on the right, then Add."
                                               : "No record to show.", "acorn-status"));
        m_browse_scroll.set_child(*outer);
        return;
    }

    bool kept = false;
    const core::Layout L = form_layout(&kept);
    std::vector<std::string> fields;
    for (const auto& c : t->columns) fields.push_back(c.name);
    const bool can = browse_writable();
    const std::string row_id = m_browse_sel;

    // The caption: which layout, and where it came from.
    auto cap = form_label(L.name + (kept ? "  ·  kept in the solution"
                                         : "  ·  the default layout -- the table's fields, in order"), "acorn-status");
    cap->set_tooltip_text(kept ? "This layout is kept in the solution (the .acorn file), not the database"
                               : "Made from the table as it is now; change it in Layouts and it is kept");
    // s010b: more than one layout -- a picker (shared with the Layouts area).
    if (const auto names = layout_names(t->name); names.size() > 1) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        std::vector<Glib::ustring> shown(names.begin(), names.end());
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(shown));
        dd->set_name("shell.browse.form_layout");
        dd->set_tooltip_text("Which layout the record is shown through");
        for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == L.name) dd->set_selected(static_cast<guint>(i));
        const std::string tn = t->name;
        dd->property_selected().signal_changed().connect([this, dd, names, tn, gen] {
            if (gen != m_view_gen) return;
            const auto i = dd->get_selected();
            if (i == GTK_INVALID_LIST_POSITION || i >= names.size()) return;
            m_layout_pick[tn] = names[i];
            m_form_focus.clear();
            Glib::signal_idle().connect_once([this] { paint_browse(); });
        });
        row->append(*dd);
        cap->set_text(kept ? "kept in the solution" : "the default -- the table's fields, in order");   // the picker names it
        cap->set_valign(Gtk::Align::CENTER);
        row->append(*cap);
        outer->append(*row);
    } else {
        outer->append(*cap);
    }
    if (const std::string mark = mark_of(t->name, row_id); !mark.empty()) {   // s008b's marks, here too
        auto m = form_label("⚠ " + mark, "acorn-mark");
        m->set_ellipsize(Pango::EllipsizeMode::NONE);
        m->set_wrap(true);
        m->set_single_line_mode(false);
        m->set_max_width_chars(60);
        outer->append(*m);
    }

    auto fixed = Gtk::make_managed<Gtk::Fixed>();
    fixed->add_css_class("acorn-form");
    fixed->set_size_request(L.width, L.height);
    fixed->set_name("shell.browse.form");
    Gtk::Widget* focus_back = nullptr;

    for (const auto& o : L.objects) {
        Gtk::Widget* w = nullptr;
        if (o.kind == core::ObjectKind::Text) {
            auto l = form_label(o.text, "acorn-field-name");
            l->set_xalign(1.0f);   // toward its box
            l->set_tooltip_text(o.text);
            w = l;
        } else if (core::missing_field(o, fields)) {
            auto l = form_label("missing field “" + o.field + "”", "acorn-missing");
            l->set_tooltip_text("This layout shows “" + o.field + "”, which “" + t->name +
                                "” no longer has. Nothing is lost -- the layout editor can remove the box or point it at another field.");
            w = l;
        } else {
            const db::Column* col = t->column(o.field);
            const db::Value* v = browse_value(o.field);
            const std::string shown = v ? db::cell_text(*v, col->type) : std::string{};
            const bool key = col->pk_position > 0;
            const bool boxed = can && !key && col->type != db::FieldType::Container;
            if (!boxed) {
                auto l = form_label(shown, shown.empty() ? "acorn-null" : nullptr);
                l->add_css_class("acorn-form-static");
                l->set_selectable(true);
                if (key && can) l->set_tooltip_text("The record's key -- Acorn finds the record by it, so it is not changed here");
                else if (!shown.empty()) l->set_tooltip_text(shown);
                w = l;
            } else if (col->type == db::FieldType::Boolean) {
                auto holder = Gtk::make_managed<Gtk::Box>();
                auto cb = Gtk::make_managed<Gtk::CheckButton>();
                cb->set_name("form.field." + o.field);
                if (v && v->is_null()) cb->set_inconsistent(true);
                if (v && !v->is_null()) cb->set_active(yes(*v));
                const std::string colname = o.field;
                cb->signal_toggled().connect([this, cb, row_id, colname, gen] {
                    if (m_browse_painting || gen != m_view_gen) return;
                    cb->set_inconsistent(false);
                    commit_field(row_id, colname, cb->get_active() ? "Yes" : "No");
                });
                holder->append(*cb);
                w = holder;
            } else {
                auto e = Gtk::make_managed<Gtk::Entry>();
                e->set_name("form.field." + o.field);
                e->add_css_class("acorn-form-entry");
                e->set_hexpand(true);
                e->set_width_chars(4);
                e->set_text(shown);
                if (v && v->is_null()) e->set_placeholder_text("empty");
                if (col->type == db::FieldType::Time) e->set_placeholder_text("HH:MM:SS");
                if (col->type == db::FieldType::Timestamp) e->set_placeholder_text("YYYY-MM-DD HH:MM:SS");
                wire_field_entry(e, row_id, o.field, &m_view_gen, gen);
                auto focus = Gtk::EventControllerFocus::create();
                const std::string colname = o.field;
                focus->signal_enter().connect([this, colname, gen] { if (gen == m_view_gen) m_form_focus = colname; });
                e->add_controller(focus);
                if (o.field == m_form_focus) focus_back = e;
                w = e;
                if (col->type == db::FieldType::Date) {   // a date box: the entry + a calendar
                    e->set_placeholder_text("YYYY-MM-DD");
                    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
                    line->append(*e);
                    auto pick = Gtk::make_managed<Gtk::MenuButton>();
                    pick->set_icon_name("x-office-calendar-symbolic");
                    pick->set_tooltip_text("Pick a day");
                    pick->add_css_class("flat");
                    pick->set_name("form.date." + o.field);
                    auto pop = Gtk::make_managed<Gtk::Popover>();
                    auto cal = Gtk::make_managed<Gtk::Calendar>();
                    if (auto d = day_of(shown)) cal->select_day(*d);
                    cal->signal_day_selected().connect([this, cal, e, pop, row_id, colname, gen] {
                        if (gen != m_view_gen) return;
                        const std::string day = cal->get_date().format("%Y-%m-%d");
                        e->set_text(day);
                        pop->popdown();
                        commit_field(row_id, colname, day);
                    });
                    pop->set_child(*cal);
                    pick->set_popover(*pop);
                    line->append(*pick);
                    w = line;
                }
            }
        }
        w->set_size_request(o.w, o.h);
        fixed->put(*w, o.x, o.y);
    }
    outer->append(*fixed);

    m_browse_painting = true;
    m_browse_scroll.set_child(*outer);
    m_browse_painting = false;
    if (focus_back) focus_back->grab_focus();
}

void Shell::form_step(int delta) {  // form: the previous / next record, across pages
    if (!browse_info() || m_browse_data.total <= 0) return;
    const std::int64_t at = m_browse_req.offset + std::max(0, m_browse_sel_index) + delta;
    if (at < 0 || at >= m_browse_data.total) return;
    m_form_focus.clear();   // the arrow has the focus; don't pull it back into a box
    const std::int64_t page = (at / m_browse_req.limit) * m_browse_req.limit;
    if (page != m_browse_req.offset) {
        m_browse_req.offset = page;
        m_browse_sel.clear();
        m_browse_sel_index = -1;
        load_browse_page();
    }
    const auto i = static_cast<int>(at - m_browse_req.offset);
    if (i >= 0 && static_cast<std::size_t>(i) < m_browse_data.ids.size()) {
        m_browse_sel_index = i;
        m_browse_sel = m_browse_data.ids[static_cast<std::size_t>(i)];
        paint_browse();
        sync_undo();
    }
}

}  // namespace acorn
