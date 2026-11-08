#include "Shell.hpp"
#include "Log.hpp"

#include <glibmm/datetime.h>
#include <gtkmm/calendar.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/fixed.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/viewport.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>

#include <cstdio>
#include <functional>

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
    const bool can = browse_writable();
    const std::string row_id = m_browse_sel;

    form_caption(*outer, *t, L, kept, row_id);
    Gtk::Widget* focus_back = nullptr;
    // s010d: the Form shows the body -- the header / footer are the List's (s012a).
    auto fixed = layout_part(L, core::Part::Body, can, focus_back);
    fixed->set_name("shell.browse.form");
    outer->append(*fixed);

    m_browse_painting = true;
    m_browse_scroll.set_child(*outer);
    m_browse_painting = false;
    if (focus_back) focus_back->grab_focus();
}

// s012a: the LIST view -- the picked layout drawn as rows: the header once
// at the top, the body once per record of the page, the footer once at the
// bottom; the header and footer stay put while the rows scroll. A click
// picks the record; a double-click opens it in the Form. Fields in the
// header / footer show the picked record.
// s012c: rows are EDITED in place -- the Form's boxes in every row (when the
// table is writable), each saving as the Form's do (Enter / leave = one step,
// Esc puts it back, Ctrl+Z with nothing typed undoes). A box taking the focus
// picks its row (a save is always for the picked record). Tab goes along a
// row and on into the next; Up / Down go to the same box a row up / down.
// The rows themselves take no focus while they edit (Tab would stop on each
// row before its first box); read-only, they walk and Enter opens the Form
// as before. Portals in a row stay shown -- edit them on the record's Form.
void Shell::paint_list() {  // form: the page through its layout, a row each
    const db::Table* t = browse_info();
    const unsigned gen = m_view_gen;
    bool kept = false;
    const core::Layout L = form_layout(&kept);
    const bool can = browse_writable();   // s012c: rows are boxes
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    outer->set_halign(Gtk::Align::START);
    outer->set_name("shell.browse.list_view");
    form_caption(*outer, *t, L, kept, {});
    auto sheet = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);   // the parts butt together: one sheet
    sheet->set_vexpand(true);
    outer->append(*sheet);

    struct Row { int& r; int was; ~Row() { r = was; } };   // m_paint_row back as it was, however we leave
    Gtk::Widget* unused = nullptr;
    auto part = [&](core::Part p, int row, bool edit, Gtk::Widget*& fb) -> Gtk::Fixed* {
        Row keep{m_paint_row, m_paint_row};
        m_paint_row = row;
        return layout_part(L, p, edit, fb);
    };
    const int picked = m_browse_sel_index >= 0 && static_cast<std::size_t>(m_browse_sel_index) < m_browse_data.rows.size()
                           ? m_browse_sel_index : 0;
    if (core::part_height(L, core::Part::Header) > 0) {
        auto h = part(core::Part::Header, picked, false, unused);
        h->add_css_class("acorn-list-header");
        h->set_name("shell.browse.list_header");
        h->set_tooltip_text("The layout's header -- drawn once, above the records");
        sheet->append(*h);
    }

    auto list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::SINGLE);
    list->set_activate_on_single_click(false);   // a click picks; a double-click (or Enter) opens the Form
    list->add_css_class("acorn-list");
    list->set_name("shell.browse.list_rows");
    Gtk::Widget* focus_back = nullptr;   // s012c: the box last focused, in the picked row
    for (std::size_t r = 0; r < m_browse_data.rows.size(); ++r) {
        Gtk::Widget* fb = nullptr;
        auto body = part(core::Part::Body, static_cast<int>(r), can, fb);
        if (static_cast<int>(r) == m_browse_sel_index) focus_back = fb;
        body->add_css_class("acorn-list-body");
        const std::string mark = r < m_browse_data.ids.size() ? mark_of(t->name, m_browse_data.ids[r]) : std::string{};
        if (!mark.empty()) {   // s008b: an amber edge and a hint
            body->add_css_class("acorn-marked");
            body->set_tooltip_text("⚠ " + mark);
        }
        list->append(*body);
        if (can) {
            if (auto* lr = list->get_row_at_index(static_cast<int>(r))) lr->set_focusable(false);   // Tab: box to box
            // A box (or check, or calendar button) taking the focus picks its
            // row -- after the box it left has saved for the row it was in.
            auto focus = Gtk::EventControllerFocus::create();
            focus->signal_enter().connect([this, list, body, gen] {
                if (m_browse_painting || gen != m_view_gen) return;
                auto* lr = dynamic_cast<Gtk::ListBoxRow*>(body->get_parent());
                if (lr && !lr->is_selected()) list->select_row(*lr);
            });
            body->add_controller(focus);
        }
    }
    if (can) {
        // s012c: Up / Down -- the same box a row up / down (by its name:
        // "form.field.<f>" / "form.date.<f>"). Heard before the box, so the
        // list's own row-walk never moves a cursor the rows don't show.
        auto keys = Gtk::EventControllerKey::create();
        keys->signal_key_pressed().connect(
            [this, list, gen](guint keyval, guint, Gdk::ModifierType state) -> bool {
                if (gen != m_view_gen) return false;
                if (keyval != GDK_KEY_Up && keyval != GDK_KEY_Down && keyval != GDK_KEY_KP_Up && keyval != GDK_KEY_KP_Down)
                    return false;
                if ((state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::ALT_MASK |
                              Gdk::ModifierType::SHIFT_MASK)) != Gdk::ModifierType{}) return false;
                auto* root = list->get_root();
                Gtk::Widget* f = root ? root->get_focus() : nullptr;
                Gtk::Widget* named = nullptr;   // the focused box's own widget (it has the name)
                Gtk::ListBoxRow* from = nullptr;
                for (Gtk::Widget* w = f; w && w != list; w = w->get_parent()) {
                    const std::string n = w->get_name();
                    if (!named && (n.rfind("form.field.", 0) == 0 || n.rfind("form.date.", 0) == 0)) named = w;
                    if (auto* lr = dynamic_cast<Gtk::ListBoxRow*>(w)) { from = lr; break; }
                }
                if (!named || !from) return false;   // not in a row's box: let it be
                const bool up = keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up;
                auto* to = list->get_row_at_index(from->get_index() + (up ? -1 : 1));
                if (!to) return true;   // the first / last row: stay
                const std::string want = named->get_name();
                std::function<Gtk::Widget*(Gtk::Widget*)> find = [&](Gtk::Widget* w) -> Gtk::Widget* {
                    if (std::string(w->get_name()) == want) return w;
                    for (auto* c = w->get_first_child(); c; c = c->get_next_sibling())
                        if (auto* hit = find(c)) return hit;
                    return nullptr;
                };
                if (auto* hit = find(to)) hit->grab_focus();
                return true;
            },
            false);
        keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        list->add_controller(keys);
    }
    auto rows = Gtk::make_managed<Gtk::ScrolledWindow>();
    rows->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    rows->set_vexpand(true);
    // Nothing scrolls under the pointer on a click: a tall row's top scrolled
    // into view would move the second click of a double-click elsewhere (no
    // activation). The picked row is brought into view below, by hand.
    auto port = Gtk::make_managed<Gtk::Viewport>(Glib::RefPtr<Gtk::Adjustment>{}, Glib::RefPtr<Gtk::Adjustment>{});
    port->set_scroll_to_focus(false);
    port->set_child(*list);
    rows->set_child(*port);
    // ...and the list's own: once parented, GtkListBox clamps a clicked row's
    // top into view through the viewport's adjustment (312 -> 336 between the
    // two clicks of a double-click: the drive). Let go of it.
    list->set_adjustment(Glib::RefPtr<Gtk::Adjustment>{});
    // The row in view: `whole` -- its top must show (a fresh paint); else only
    // when none of it shows (the arrows walked off the end).
    auto show_row = [this, rows, list, gen](int index, bool whole) {
        Glib::signal_idle().connect_once([this, rows, list, gen, index, whole] {
            if (gen != m_view_gen) return;
            auto* r = list->get_row_at_index(index);
            if (!r) return;
            graphene_point_t p{0, 0}, q{0, 0};
            if (!gtk_widget_compute_point(GTK_WIDGET(r->gobj()), GTK_WIDGET(list->gobj()), &p, &q)) return;
            auto adj = rows->get_vadjustment();
            const double y = q.y, h = r->get_height(), page = adj->get_page_size(), at = adj->get_value();
            const bool off = whole ? (y < at || y + std::min(h, page) > at + page) : (y + h <= at || y >= at + page);
            if (off) adj->set_value(std::max(0.0, y));
        });
    };
    list->signal_row_selected().connect([this, gen, show_row](Gtk::ListBoxRow* r) {
        if (m_browse_painting || gen != m_view_gen) return;
        browse_select(r ? r->get_index() : -1);
        if (r) show_row(r->get_index(), false);
    });
    // A double-click or Enter: that record in the Form.
    list->signal_row_activated().connect([this, gen](Gtk::ListBoxRow* r) {
        if (gen != m_view_gen || !r) return;
        browse_select(r->get_index());
        Glib::signal_idle().connect_once([this] { m_browse_form_btn.set_active(true); });
    });
    sheet->append(*rows);

    if (core::part_height(L, core::Part::Footer) > 0) {
        auto f = part(core::Part::Footer, picked, false, unused);
        f->add_css_class("acorn-list-footer");
        f->set_name("shell.browse.list_footer");
        f->set_tooltip_text("The layout's footer -- drawn once, below the records");
        sheet->append(*f);
    }

    m_browse_painting = true;
    Gtk::ListBoxRow* sel = m_browse_sel_index >= 0 ? list->get_row_at_index(m_browse_sel_index) : nullptr;
    if (sel) list->select_row(*sel);
    m_browse_scroll.set_child(*outer);
    m_browse_painting = false;
    if (sel) show_row(m_browse_sel_index, true);   // the picked row in view, once it has its place
    if (focus_back) focus_back->grab_focus();       // s012c: back in the box you were in (an undo, a repaint)
}

// The Form's and the List's caption: which layout, and where it came from;
// a picker when the table has more than one (shared with Layouts); the
// record's ⚠ mark (Form only -- `row_id` empty for the List).
void Shell::form_caption(Gtk::Box& outer, const db::Table& tbl, const core::Layout& L, bool kept, const std::string& row_id) {  // form: caption + picker
    const db::Table* t = &tbl;
    const unsigned gen = m_view_gen;
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
        dd->set_tooltip_text("Which layout the records are shown through (Layouts shares the pick)");
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
        outer.append(*row);
    } else {
        outer.append(*cap);
    }
    if (const std::string mark = mark_of(t->name, row_id); !mark.empty()) {   // s008b's marks, here too
        auto m = form_label("⚠ " + mark, "acorn-mark");
        m->set_ellipsize(Pango::EllipsizeMode::NONE);
        m->set_wrap(true);
        m->set_single_line_mode(false);
        m->set_max_width_chars(60);
        outer.append(*m);
    }

}

// s012a: one part of a layout (header, body or footer) drawn for the record
// browse_value reads -- the picked one, or the List's row (m_paint_row).
// `edit`: fields are boxes that save (the Form); else shown, not edited.
Gtk::Fixed* Shell::layout_part(const core::Layout& L, core::Part part, bool edit, Gtk::Widget*& focus_back) {  // form: a part, drawn
    const db::Table* t = browse_info();
    const unsigned gen = m_view_gen;
    std::vector<std::string> fields;
    for (const auto& c : t->columns) fields.push_back(c.name);
    const std::string row_id = m_paint_row >= 0 && static_cast<std::size_t>(m_paint_row) < m_browse_data.ids.size()
                                   ? m_browse_data.ids[static_cast<std::size_t>(m_paint_row)] : m_browse_sel;
    auto fixed = Gtk::make_managed<Gtk::Fixed>();
    fixed->add_css_class("acorn-form");
    const int top = core::part_top(L, part);
    fixed->set_size_request(L.width, std::max(1, core::part_height(L, part)));
    // s010c: lines and boxes first -- drawn behind the fields and words.
    for (int oi : core::part_objects(L, part)) {
        const auto& o = L.objects[static_cast<std::size_t>(oi)];
        Gtk::Widget* w = nullptr;
        if (o.kind == core::ObjectKind::Line || o.kind == core::ObjectKind::Box) {
            auto da = Gtk::make_managed<Gtk::DrawingArea>();
            da->set_can_target(false);
            da->add_css_class("acorn-form-drawing");
            const bool line = o.kind == core::ObjectKind::Line;
            da->set_draw_func([da, line](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                GdkRGBA ink{};
                gtk_widget_get_color(GTK_WIDGET(da->gobj()), &ink);
                cr->set_source_rgba(ink.red, ink.green, ink.blue, line ? 0.45 : 0.30);
                if (line) {
                    cr->set_line_width(1);
                    if (w >= h) { cr->move_to(0, h / 2 + 0.5); cr->line_to(w, h / 2 + 0.5); }
                    else        { cr->move_to(w / 2 + 0.5, 0); cr->line_to(w / 2 + 0.5, h); }
                } else {
                    cr->set_line_width(1);
                    cr->rectangle(0.5, 0.5, w - 1, h - 1);
                }
                cr->stroke();
            });
            w = da;
        } else if (o.kind == core::ObjectKind::Portal) {   // s011a
            w = form_portal(o, *t, focus_back, edit && m_paint_row < 0);   // s012c: a List row's portal is shown
        } else if (o.kind == core::ObjectKind::Related) {  // s011d
            w = form_related(o, *t);
        } else if (o.kind == core::ObjectKind::Text) {
            auto l = form_label(o.text, "acorn-field-name");
            // s012b: as the layout says -- right (toward its box) unless told otherwise
            l->set_xalign(o.align == core::Align::Left ? 0.0f : o.align == core::Align::Center ? 0.5f : 1.0f);
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
            const bool boxed = edit && !key && col->type != db::FieldType::Container;
            if (!boxed) {
                auto l = form_label(shown, shown.empty() ? "acorn-null" : nullptr);
                l->add_css_class("acorn-form-static");
                l->set_selectable(true);
                if (key && edit) l->set_tooltip_text("The record's key -- Acorn finds the record by it, so it is not changed here");
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
        fixed->put(*w, o.x, o.y - top);
    }
    return fixed;
}

// s011a: a PORTAL -- the records of o.from whose o.field points at the shown
// record, one row each under the column names, scrolling inside its box. A
// link that's gone is a dashed "missing link", like a missing field. Reads
// through the Gate: a SELECT.
// s011b: EDITED -- when the related table is writable each cell is a box that
// saves as the Form's do (Enter / leave = one step, Esc puts it back, Ctrl+Z
// with nothing typed undoes); the link field is never a box (it is this
// record's key). "New" at the foot adds a related record with the link filled
// in; a row's trash deletes it, following its own links as Delete Record does.
// Each one step.
Gtk::Widget* Shell::form_portal(const core::LayoutObject& o, const db::Table& parent, Gtk::Widget*& focus_back, bool edit) {  // form: the related records
    constexpr std::int64_t kShown = 200;   // more than that: a line says how many, Browse has the rest
    constexpr int kTrashW = 28;            // the row's delete button, and the names' row's matching gap
    const unsigned gen = m_view_gen;
    const db::Table* ft = m_schema.table(o.from);
    const db::ForeignKey* fk = nullptr;
    if (ft)
        for (const auto& k : ft->foreign_keys)
            if (k.ref_table == parent.name && k.columns.size() == 1 && k.columns.front() == o.field) fk = &k;
    const auto ref = fk ? m_schema.ref_columns(*fk) : std::vector<std::string>{};
    if (!fk || ref.size() != 1) {
        auto l = form_label("missing link “" + o.from + "." + o.field + "”", "acorn-missing");
        l->set_tooltip_text("This portal lists “" + o.from + "” records by “" + o.field + "”, which no longer points at “" +
                            parent.name + "”. Nothing is lost -- link them again in the Graph, or remove the portal in Layouts.");
        return l;
    }
    const bool can = edit && m_writable && m_journal && db::Store::why_not_writable(*ft).empty();
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->add_css_class("acorn-portal");
    box->set_name("form.portal." + o.from);
    box->set_overflow(Gtk::Overflow::HIDDEN);

    // A row: the columns sharing the width, then (when editable) the trash's slot.
    auto row_of = [&](const char* css) {
        auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        line->add_css_class(css);
        auto cols = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        cols->set_homogeneous(true);
        cols->set_hexpand(true);
        line->append(*cols);
        return std::pair{line, cols};
    };
    auto cell_label = [&](const std::string& text, bool dim) {
        auto l = form_label(text, dim ? "acorn-null" : nullptr);
        l->set_hexpand(true);
        l->set_valign(Gtk::Align::CENTER);
        l->set_max_width_chars(1);   // natural width tiny: the columns share the portal's width, never widen the Form
        if (!text.empty()) l->set_tooltip_text(text);
        return l;
    };
    {
        auto [hr, hc] = row_of("acorn-portal-head");
        for (const auto& c : o.columns) {
            const bool gone = !ft->column(c);
            auto l = cell_label(gone ? c + " ?" : c, gone);
            if (gone) l->set_tooltip_text("“" + o.from + "” no longer has “" + c + "” -- this column stays empty");
            hc->append(*l);
        }
        if (can) {
            auto gap = Gtk::make_managed<Gtk::Box>();
            gap->set_size_request(kTrashW, -1);
            hr->append(*gap);
        }
        box->append(*hr);
    }

    auto rows = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    const db::Value* key = browse_value(ref.front());
    std::expected<db::Page, db::Error> page = db::Page{};
    if (key) page = db::read_related(*m_conn, *ft, {o.field}, {*key}, {0, kShown});
    const std::string from = o.from;
    if (!page) {
        rows->append(*form_label("Could not read “" + o.from + "”: " + page.error().message, "acorn-missing"));
        if (auto lg = log::get(log::Area::Shell)) lg->warn("form: portal '{}': {}", o.from, page.error().message);
    } else if (page->rows.empty()) {
        auto l = form_label("No “" + o.from + "” point at this record." + (can ? " New adds one." : ""), "acorn-null");
        l->add_css_class("acorn-portal-more");
        rows->append(*l);
    } else {
        std::vector<int> at;   // each column's place in the page's row (-1: gone)
        for (const auto& c : o.columns) {
            int k = -1;
            for (std::size_t i = 0; i < page->columns.size(); ++i) if (page->columns[i] == c) k = static_cast<int>(i);
            at.push_back(k);
        }
        for (std::size_t ri = 0; ri < page->rows.size(); ++ri) {
            const auto& r = page->rows[ri];
            const std::string rid = ri < page->ids.size() ? page->ids[ri] : std::string{};
            const bool row_can = can && !rid.empty();
            auto [line, cols] = row_of("acorn-portal-row");
            for (std::size_t i = 0; i < at.size(); ++i) {
                const db::Column* col = at[i] >= 0 ? &ft->columns[static_cast<std::size_t>(at[i])] : nullptr;
                const std::string tx = col && static_cast<std::size_t>(at[i]) < r.size()
                                           ? db::cell_text(r[static_cast<std::size_t>(at[i])], col->type) : std::string{};
                const bool boxed = row_can && col && col->pk_position == 0 && col->name != o.field &&
                                   col->type != db::FieldType::Container;
                if (!boxed) { cols->append(*cell_label(tx, false)); continue; }
                auto e = Gtk::make_managed<Gtk::Entry>();
                e->add_css_class("acorn-portal-entry");
                e->set_hexpand(true);
                e->set_width_chars(1);
                e->set_text(tx);
                e->set_enable_undo(false);
                e->set_name("form.portal." + from + "." + col->name);
                if (col->type == db::FieldType::Date) e->set_placeholder_text("YYYY-MM-DD");
                const std::string cname = col->name;
                const std::string fkey = "portal|" + from + "|" + rid + "|" + cname;
                // What the record holds: a save keeps the boxes (Tab's focus stays),
                // so the box's own idea of "saved" moves with each save.
                auto saved = std::make_shared<std::string>(tx);
                auto keep = [this, e, from, rid, cname, saved] {
                    const std::string typed = e->get_text();
                    if (commit_related(from, rid, cname, typed, *saved)) *saved = typed;
                };
                e->signal_activate().connect([this, keep, gen] { if (gen == m_view_gen) keep(); });
                auto focus = Gtk::EventControllerFocus::create();
                focus->signal_enter().connect([this, fkey, gen] { if (gen == m_view_gen) m_form_focus = fkey; });
                focus->signal_leave().connect([this, keep, gen] {
                    if (m_browse_painting || gen != m_view_gen) return;
                    keep();
                });
                e->add_controller(focus);
                // Esc -- or Ctrl+Z with typing pending -- puts the box back; Ctrl+Z
                // with nothing pending undoes the last step (as the Form's boxes).
                auto keys = Gtk::EventControllerKey::create();
                keys->signal_key_pressed().connect(
                    [this, e, saved, gen](guint keyval, guint, Gdk::ModifierType state) -> bool {
                        if (gen != m_view_gen) return false;
                        const bool ctrl_z = (keyval == GDK_KEY_z || keyval == GDK_KEY_Z) &&
                            (state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK &&
                            (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType::SHIFT_MASK;
                        if (keyval != GDK_KEY_Escape && !ctrl_z) return false;
                        if (ctrl_z && std::string(e->get_text()) == *saved) { activate_action("win.undo"); return true; }
                        e->set_text(*saved);
                        e->set_position(-1);
                        return true;
                    },
                    false);
                keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
                e->add_controller(keys);
                if (m_form_focus == fkey) focus_back = e;
                cols->append(*e);
            }
            if (can) {
                auto del = Gtk::make_managed<Gtk::Button>();
                del->set_icon_name("user-trash-symbolic");
                del->add_css_class("flat");
                del->add_css_class("acorn-portal-trash");
                del->set_size_request(kTrashW, -1);
                del->set_name("form.portal." + from + ".delete");
                del->set_sensitive(row_can);
                del->set_tooltip_text("Delete this “" + from + "” record -- Ctrl+Z brings it back");
                del->signal_clicked().connect([this, rid, from, gen] {
                    if (gen != m_view_gen) return;
                    auto d = db::verbs::delete_record(*m_journal, rid);
                    if (auto lg = log::get(log::Area::App))
                        lg->info("journal: portal delete {}: {}", db::Store::describe(rid), d ? "deleted" : d.error().message);
                    if (d) browse_note("Deleted " + db::Store::describe(rid) + ". Ctrl+Z brings it back.", "ok");
                    else   browse_note("Not deleted: " + d.error().message + ".", "error");
                    m_form_focus.clear();
                    Glib::signal_idle().connect_once([this] { load_browse_page(); });   // a row went: repaint
                });
                line->append(*del);
            }
            rows->append(*line);
        }
        if (page->total > static_cast<std::int64_t>(page->rows.size())) {
            auto more = form_label(std::to_string(page->rows.size()) + " of " + std::to_string(page->total) +
                                   " shown -- Browse “" + o.from + "” has them all", "acorn-status");
            more->add_css_class("acorn-portal-more");
            rows->append(*more);
        }
    }
    auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
    sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    sc->set_vexpand(true);
    sc->set_child(*rows);
    box->append(*sc);
    if (can && key && !key->is_null()) {   // the foot: New, the link filled in
        auto add = Gtk::make_managed<Gtk::Button>("New “" + o.from + "”");
        add->add_css_class("flat");
        add->add_css_class("acorn-portal-new");
        add->set_halign(Gtk::Align::START);
        add->set_name("form.portal." + from + ".new");
        add->set_tooltip_text("A new “" + from + "” record pointing at this one -- Ctrl+Z takes it away");
        const std::string link = o.field;
        const db::Value kv = *key;
        std::string first;   // the new row's first box takes the focus
        for (const auto& c : o.columns)
            if (const auto* col = ft->column(c); first.empty() && col && col->pk_position == 0 && c != link &&
                                                 col->type != db::FieldType::Container) first = c;
        add->signal_clicked().connect([this, from, link, kv, first, gen] {
            if (gen != m_view_gen) return;
            auto made = db::verbs::new_record(*m_journal, from, {link}, {kv});
            if (auto lg = log::get(log::Area::App))
                lg->info("journal: portal new in '{}': {}", from, made ? "made" : made.error().message);
            if (made) {
                browse_note("Added a “" + from + "” record. Ctrl+Z takes it away.", "ok");
                m_form_focus = first.empty() ? std::string{} : "portal|" + from + "|" + *made + "|" + first;
            } else {
                browse_note("Not added: " + made.error().message + ".", "error");
            }
            Glib::signal_idle().connect_once([this] { load_browse_page(); });   // a row came: repaint
        });
        box->append(*add);
    } else if (!can) {
        box->set_tooltip_text("“" + o.from + "” records whose “" + o.field + "” points at this one -- " +
                              (!edit ? std::string("shown here; open the record's Form to edit them") :
                               m_writable ? "“" + o.from + "” has no key, so they are shown, not edited"
                                          : std::string("this connection is read-only")));
    }
    return box;
}

// s011d: a RELATED field -- a field of the record this one points at (an
// address's contact's last_name), read by the link's value: a SELECT through
// the Gate. Shown, not edited (edit it on the parent's own Form). An empty
// link, or one pointing at no record, shows empty and says why in its
// tooltip; a link that's gone is a dashed "missing link", never dropped.
Gtk::Widget* Shell::form_related(const core::LayoutObject& o, const db::Table& child) {  // form: a parent's field
    const db::Table* pt = m_schema.table(o.from);
    const db::ForeignKey* fk = nullptr;
    for (const auto& k : child.foreign_keys)
        if (k.ref_table == o.from && k.columns.size() == 1 && k.columns.front() == o.field) fk = &k;
    const auto ref = fk && pt ? m_schema.ref_columns(*fk) : std::vector<std::string>{};
    if (!fk || !pt || ref.size() != 1) {
        auto l = form_label("missing link “" + child.name + "." + o.field + "”", "acorn-missing");
        l->set_tooltip_text("This shows “" + o.from + "”.“" + o.shows + "” through “" + o.field + "”, which no longer points at “" +
                            o.from + "”. Nothing is lost -- link them again in the Graph, or remove it in Layouts.");
        return l;
    }
    const db::Column* col = pt->column(o.shows);
    if (!col) {
        auto l = form_label("missing field “" + o.from + "." + o.shows + "”", "acorn-missing");
        l->set_tooltip_text("“" + o.from + "” no longer has “" + o.shows + "”. Nothing is lost -- the layout editor can "
                            "point it at another field or remove it.");
        return l;
    }
    const std::string about = "From the “" + o.from + "” record that “" + o.field + "” points at -- edit it on the “" + o.from + "” Form";
    std::string shown, why;
    const db::Value* link = browse_value(o.field);
    if (!link || link->is_null()) {
        why = "“" + o.field + "” is empty -- it points at no “" + o.from + "” record";
    } else if (auto page = db::read_related(*m_conn, *pt, {ref.front()}, {*link}, {0, 1}); !page) {
        why = "Could not read “" + o.from + "”: " + page.error().message;
        if (auto lg = log::get(log::Area::Shell)) lg->warn("form: related '{}.{}': {}", o.from, o.shows, page.error().message);
    } else if (page->rows.empty()) {
        why = "No “" + o.from + "” record has “" + ref.front() + "” " + db::cell_text(*link, child.column(o.field)->type) +
              " -- “" + o.field + "” points at nothing";
    } else {
        for (std::size_t i = 0; i < page->columns.size(); ++i)
            if (page->columns[i] == o.shows && i < page->rows.front().size()) shown = db::cell_text(page->rows.front()[i], col->type);
    }
    auto l = form_label(shown, shown.empty() ? "acorn-null" : nullptr);
    l->add_css_class("acorn-form-static");
    l->add_css_class("acorn-related");
    l->set_selectable(true);
    l->set_name("form.related." + o.from + "." + o.shows);
    l->set_tooltip_text(why.empty() ? (shown.empty() ? about : shown + "\n" + about) : why);
    return l;
}

bool Shell::commit_related(const std::string& table, const std::string& row, const std::string& column,
                           const std::string& typed, const std::string& saved) {  // form: a portal cell -> set_field
    if (!m_journal || row.empty() || typed == saved) return false;   // left alone: not a step
    const db::Table* t = m_schema.table(table);
    const db::Column* c = t ? t->column(column) : nullptr;
    if (!c) return false;
    auto v = db::parse_field(typed, *c);
    if (!v) {   // refused: repaint, so the box shows what is really there
        browse_note("Not saved: " + v.error() + ".", "error");
        Glib::signal_idle().connect_once([this] { load_browse_page(); });
        return false;
    }
    auto r = db::verbs::set_field(*m_journal, row, column, *v);
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: portal edit {} '{}': {}", db::Store::describe(row), column, r ? "saved" : r.error().message);
    if (r) browse_note("Saved “" + table + "”.“" + column + "”. Ctrl+Z undoes it.", "ok");
    else   browse_note("Not saved: " + r.error().message + ".", "error");
    const bool ok = r.has_value();   // saved: the boxes stay (Tab's focus); refused: repaint
    Glib::signal_idle().connect_once([this, ok] { load_browse_page(ok); });
    return ok;
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
