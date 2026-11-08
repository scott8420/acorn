#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/fixed.h>
#include <gtkmm/label.h>

// Shell_find.cpp -- FIND (s013a): query by form. Find (Ctrl+F) turns
// Browse's records into the picked layout's body with EMPTY boxes -- every
// field a box, the key too. Type what to look for in any of them; Enter (or
// the Find button) finds. The records that match every box become the FOUND
// SET: Cards, Grid, Form and List page through only them, and the line under
// Browse says "found 5 of 12" and what was asked. Show All (Ctrl+J) brings
// every record back. What a box asks is db::find_query's rule (Browse.hpp):
// text CONTAINS (any case), "=" alone is empty, "=x" exact, < > <= >= compare,
// the rest equal.
//
// s013b: portals and related fields are searched too -- a portal's columns
// are a row of boxes ("contacts with a phone holding 0177"), a related field
// a box ("addresses whose contact's last name has th"): each asks through
// the link (db::Criterion's table / outer / inner).
//
// Boxes are keyed in each request's `typed` as "field|f", "portal|<table>|<column>",
// "related|<link>|<field>".
//
// s022: REQUESTS (FileMaker's): New Request adds an empty set of boxes; a
// record is found when it matches EVERY box of ANY request; a request ticked
// Omit takes out what it matches instead. "Request 2 of 3" with ◀ ▶ walks
// them; Delete Request drops the shown one. A box may hold a range: 1..5.
//
// Find writes nothing: it is SELECTs through the Gate, so it works on a
// read-only connection. The found set is the find run again on each read --
// a record changed so it no longer matches leaves it.

namespace acorn {

namespace {

Gtk::Label* find_label(const std::string& s, const char* css) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    l->set_ellipsize(Pango::EllipsizeMode::END);
    l->set_single_line_mode(true);
    if (css) l->add_css_class(css);
    return l;
}

}  // namespace

// What a portal column's / related field's box asks, through its link -- none
// when the link is gone (the box isn't drawn).
std::optional<db::Criterion> Shell::find_box(const core::LayoutObject& o, const std::string& column) const {  // find: through a link
    const db::Table* t = browse_info();
    if (!t) return std::nullopt;
    if (o.kind == core::ObjectKind::Portal) {
        const db::Table* ft = m_schema.table(o.from);
        if (!ft || !ft->column(column)) return std::nullopt;
        for (const auto& k : ft->foreign_keys)
            if (k.ref_table == t->name && k.columns.size() == 1 && k.columns.front() == o.field)
                if (auto ref = m_schema.ref_columns(k); ref.size() == 1) return db::Criterion{column, "", o.from, ref.front(), o.field};
    } else if (o.kind == core::ObjectKind::Related) {
        const db::Table* pt = m_schema.table(o.from);
        if (!pt || !pt->column(o.shows)) return std::nullopt;
        for (const auto& k : t->foreign_keys)
            if (k.ref_table == o.from && k.columns.size() == 1 && k.columns.front() == o.field)
                if (auto ref = m_schema.ref_columns(k); ref.size() == 1) return db::Criterion{o.shows, "", o.from, o.field, ref.front()};
    }
    return std::nullopt;
}

bool Shell::found_here() const {  // find: a found set for the shown table
    return !m_found.empty() && !m_found_table.empty() && m_found_table == m_browse_table;
}

void Shell::start_find() {  // find: win.find -> the Find screen
    if (!m_conn || !browse_info()) return;
    if (m_stack.get_visible_child_name() != "browse") m_stack.set_visible_child("browse");
    // A find already standing: its boxes come back as typed, to refine it.
    m_find_reqs.assign(1, FindRequest{});
    m_find_at = 0;
    if (found_here() && !m_found_reqs.empty()) m_find_reqs = m_found_reqs;
    m_finding = true;
    m_record_new = false;
    browse_note("Find: type what to look for in any box, then Enter. Esc goes back.", "ok");
    paint_browse();
    paint_record();
    sync_undo();
}

void Shell::cancel_find() {  // find: back to the records as they were
    if (!m_finding) return;
    m_finding = false;
    browse_note("", "");
    paint_browse();
    paint_record();
    sync_undo();
}

void Shell::do_find() {  // find: the typed boxes -> the found set
    const db::Table* t = browse_info();
    if (!m_finding || !m_conn || !t) return;
    // The table's own fields in its order, then the layout's portals and
    // related fields in its order -- so the line reads as the boxes do.
    // s022: every request in turn, numbered as they're shown.
    std::vector<db::Criterion> crit;
    const core::Layout L = form_layout();
    for (std::size_t ri = 0; ri < m_find_reqs.size(); ++ri) {
        const auto& typed = m_find_reqs[ri].typed;
        const auto at = crit.size();
        for (const auto& c : t->columns)
            if (auto it = typed.find("field|" + c.name); it != typed.end()) crit.push_back({c.name, it->second});
        for (int oi : core::part_objects(L, core::Part::Body)) {
            const auto& o = L.objects[static_cast<std::size_t>(oi)];
            const bool portal = o.kind == core::ObjectKind::Portal;
            if (!portal && o.kind != core::ObjectKind::Related) continue;
            for (const auto& col : portal ? o.columns : std::vector<std::string>{o.shows}) {
                const std::string key = portal ? "portal|" + o.from + "|" + col : "related|" + o.field + "|" + o.shows;
                auto it = typed.find(key);
                if (it == typed.end()) continue;
                if (auto c = find_box(o, col)) { c->typed = it->second; crit.push_back(*c); }
            }
        }
        for (auto i = at; i < crit.size(); ++i) {
            crit[i].request = static_cast<int>(ri);
            crit[i].omit = m_find_reqs[ri].omit;
        }
    }
    auto q = db::find_query(*m_conn, *t, crit, &m_schema);
    if (!q) {
        browse_note("Not found: " + q.error() + ".", "error");
        return;
    }
    auto p = db::read_found(*m_conn, *t, crit, {0, 1}, &m_schema);
    if (auto lg = log::get(log::Area::Shell))
        lg->info("find: '{}' {}: {}", t->name, db::find_summary(crit), p ? std::to_string(p->total) : p.error().message);
    if (!p) {
        browse_note("Could not find: " + p.error().message + ".", "error");
        return;
    }
    if (p->total == 0) {   // the find stays open: change it, or Esc
        browse_note("No records match " + db::find_summary(crit) + " -- change the find, or Esc to go back.", "error");
        return;
    }
    std::erase_if(crit, [](const db::Criterion& c) { return c.typed.find_first_not_of(" \t") == std::string::npos; });
    m_found = crit;
    m_found_reqs = m_find_reqs;
    m_found_table = t->name;
    m_finding = false;
    m_browse_req.offset = 0;
    m_browse_sel.clear();
    m_browse_sel_index = -1;
    m_form_focus.clear();
    browse_note("Found " + std::to_string(p->total) + (p->total == 1 ? " record" : " records") +
                ". Show All (Ctrl+J) brings back every record.", "ok");
    load_browse_page();
}

void Shell::show_all() {  // find: win.show-all -> every record again
    if (!found_here() && !m_finding) return;
    m_found.clear();
    m_found_reqs.clear();
    m_found_table.clear();
    m_finding = false;
    m_browse_req.offset = 0;
    browse_note("Showing every record.", "ok");
    load_browse_page();
}

// The Find screen: the picked layout's body, its field objects as empty
// boxes (named "find.field.<f>"), words and rules as the Form draws them.
// Portals and related fields aren't searched yet -- shown dimmed.
void Shell::paint_find() {  // find: the layout's boxes, empty
    const db::Table* t = browse_info();
    const unsigned gen = m_view_gen;
    bool kept = false;
    const core::Layout L = form_layout(&kept);
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    outer->set_halign(Gtk::Align::START);
    outer->set_valign(Gtk::Align::START);
    outer->set_name("shell.browse.find_view");
    form_caption(*outer, *t, L, kept, {});

    auto how = find_label("Text finds what contains it (any case) · “=” alone: empty · “=x” exactly x · "
                          "< > <= >= compare · 1..5 a range · blank boxes ask nothing · a portal's boxes find records with "
                          "any matching row · New Request: records matching EITHER request · Omit: leave out what it matches",
                          "acorn-status");
    how->set_ellipsize(Pango::EllipsizeMode::NONE);
    how->set_wrap(true);
    how->set_single_line_mode(false);
    how->set_max_width_chars(70);
    outer->append(*how);

    // s022: the requests -- which one is shown, ◀ ▶, New / Delete, Omit.
    {
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        bar->set_name("shell.browse.find_requests");
        const auto n = m_find_reqs.size();
        auto prev = Gtk::make_managed<Gtk::Button>();
        prev->set_icon_name("go-previous-symbolic");
        prev->add_css_class("flat");
        prev->set_name("shell.browse.find_prev");
        prev->set_tooltip_text("The request before");
        prev->set_sensitive(m_find_at > 0);
        prev->signal_clicked().connect([this, gen] { if (gen == m_view_gen) find_request(static_cast<int>(m_find_at) - 1); });
        auto which = find_label("Request " + std::to_string(m_find_at + 1) + " of " + std::to_string(n), "acorn-field-name");
        which->set_name("shell.browse.find_which");
        auto next = Gtk::make_managed<Gtk::Button>();
        next->set_icon_name("go-next-symbolic");
        next->add_css_class("flat");
        next->set_name("shell.browse.find_next");
        next->set_tooltip_text("The request after");
        next->set_sensitive(m_find_at + 1 < n);
        next->signal_clicked().connect([this, gen] { if (gen == m_view_gen) find_request(static_cast<int>(m_find_at) + 1); });
        auto add = Gtk::make_managed<Gtk::Button>("New Request");
        add->set_name("shell.browse.find_new_request");
        add->set_tooltip_text("Another set of empty boxes -- a record matching EITHER request is found");
        add->signal_clicked().connect([this, gen] { if (gen == m_view_gen) find_request(-1); });
        auto del = Gtk::make_managed<Gtk::Button>("Delete Request");
        del->set_name("shell.browse.find_delete_request");
        del->set_sensitive(n > 1);
        del->set_tooltip_text(n > 1 ? "Drop this request (its boxes)" : "The only request -- clear its boxes instead");
        del->signal_clicked().connect([this, gen] { if (gen == m_view_gen) delete_find_request(); });
        auto omit = Gtk::make_managed<Gtk::CheckButton>("Omit");
        omit->set_name("shell.browse.find_omit");
        omit->set_active(m_find_reqs[m_find_at].omit);
        omit->set_tooltip_text("Leave out the records this request matches, instead of finding them");
        omit->signal_toggled().connect([this, gen, omit] {
            if (gen == m_view_gen) m_find_reqs[m_find_at].omit = omit->get_active();
        });
        bar->append(*prev);
        bar->append(*which);
        bar->append(*next);
        bar->append(*add);
        bar->append(*del);
        bar->append(*omit);
        outer->append(*bar);
    }

    std::vector<std::string> fields;
    for (const auto& c : t->columns) fields.push_back(c.name);
    auto fixed = Gtk::make_managed<Gtk::Fixed>();
    fixed->add_css_class("acorn-form");
    fixed->add_css_class("acorn-find");
    fixed->set_name("shell.browse.find");
    const int top = core::part_top(L, core::Part::Body);
    fixed->set_size_request(L.width, std::max(1, core::part_height(L, core::Part::Body)));
    Gtk::Entry* first = nullptr;
    Gtk::Entry* focus_back = nullptr;
    // A Find box: what's typed kept by `key`; Enter finds, Esc goes back.
    auto entry = [&](const std::string& key, const std::string& name, const char* hint) {
        auto e = Gtk::make_managed<Gtk::Entry>();
        e->set_name(name);
        e->add_css_class("acorn-form-entry");
        e->add_css_class("acorn-find-entry");
        e->set_hexpand(true);
        e->set_width_chars(4);
        if (hint) e->set_placeholder_text(hint);
        if (auto it = find_typed().find(key); it != find_typed().end()) e->set_text(it->second);
        e->signal_changed().connect([this, e, key, gen] {
            if (gen == m_view_gen) find_typed()[key] = e->get_text();
        });
        e->signal_activate().connect([this, gen] { if (gen == m_view_gen) do_find(); });
        auto keys = Gtk::EventControllerKey::create();
        keys->signal_key_pressed().connect(
            [this, gen](guint keyval, guint, Gdk::ModifierType) -> bool {
                if (gen != m_view_gen || keyval != GDK_KEY_Escape) return false;
                Glib::signal_idle().connect_once([this] { cancel_find(); });
                return true;
            },
            false);
        keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        e->add_controller(keys);
        if (!first) first = e;
        if (!focus_back) if (auto it = find_typed().find(key); it != find_typed().end() && !it->second.empty()) focus_back = e;
        return e;
    };
    for (int oi : core::part_objects(L, core::Part::Body)) {
        const auto& o = L.objects[static_cast<std::size_t>(oi)];
        Gtk::Widget* w = nullptr;
        if (o.kind == core::ObjectKind::Line || o.kind == core::ObjectKind::Box) {
            auto da = Gtk::make_managed<Gtk::DrawingArea>();
            da->set_can_target(false);
            const bool line = o.kind == core::ObjectKind::Line;
            da->set_draw_func([da, line](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                GdkRGBA ink{};
                gtk_widget_get_color(GTK_WIDGET(da->gobj()), &ink);
                cr->set_source_rgba(ink.red, ink.green, ink.blue, line ? 0.45 : 0.30);
                cr->set_line_width(1);
                if (!line)       cr->rectangle(0.5, 0.5, w - 1, h - 1);
                else if (w >= h) { cr->move_to(0, h / 2 + 0.5); cr->line_to(w, h / 2 + 0.5); }
                else             { cr->move_to(w / 2 + 0.5, 0); cr->line_to(w / 2 + 0.5, h); }
                cr->stroke();
            });
            w = da;
        } else if (o.kind == core::ObjectKind::Button) {   // s017b: shown, dim -- a button doesn't run in Find
            auto b = Gtk::make_managed<Gtk::Button>(o.text);
            b->add_css_class("acorn-layout-button");
            b->set_sensitive(false);
            b->set_can_target(false);
            b->set_name("find.button." + (o.script.empty() ? o.text : o.script));
            w = b;
        } else if (o.kind == core::ObjectKind::Text) {
            auto l = find_label(o.text, "acorn-field-name");
            l->set_xalign(o.align == core::Align::Left ? 0.0f : o.align == core::Align::Center ? 0.5f : 1.0f);
            w = l;
        } else if (o.kind == core::ObjectKind::Portal) {   // s013b: a row of boxes, one per column, under the names
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            box->add_css_class("acorn-portal");
            box->set_name("find.portal." + o.from);
            box->set_overflow(Gtk::Overflow::HIDDEN);
            auto names = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
            names->add_css_class("acorn-portal-names");
            names->set_homogeneous(true);
            auto boxes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
            boxes->set_homogeneous(true);
            int shown = 0;
            for (const auto& col : o.columns) {
                if (!find_box(o, col)) continue;
                auto n = find_label(col, nullptr);
                n->set_max_width_chars(1);
                names->append(*n);
                auto e = entry("portal|" + o.from + "|" + col, "find.portal." + o.from + "." + col, nullptr);
                e->set_max_width_chars(1);
                e->set_tooltip_text("Records with a “" + o.from + "” whose " + col + " matches");
                boxes->append(*e);
                ++shown;
            }
            for (Gtk::Widget* r : {static_cast<Gtk::Widget*>(names), static_cast<Gtk::Widget*>(boxes)}) {
                r->set_margin_start(6);
                r->set_margin_end(6);
            }
            names->set_margin_top(4);
            if (shown == 0) {
                w = find_label("missing link “" + o.from + "." + o.field + "” -- not searched", "acorn-missing");
            } else {
                auto how2 = find_label("a record with any “" + o.from + "” that matches", "acorn-null");
                how2->set_margin_start(6);
                box->append(*names);
                box->append(*boxes);
                box->append(*how2);
                w = box;
            }
        } else if (o.kind == core::ObjectKind::Related) {   // s013b: a box, through the link
            if (find_box(o, o.shows)) {
                auto e = entry("related|" + o.field + "|" + o.shows, "find.related." + o.from + "." + o.shows, nullptr);
                e->set_tooltip_text("Records whose “" + o.from + "” (by " + o.field + ") has a matching " + o.shows);
                e->add_css_class("acorn-related");
                w = e;
            } else {
                w = find_label("missing link “" + o.from + "." + o.shows + "” -- not searched", "acorn-missing");
            }
        } else if (!t->column(o.field) && m_solution.summary(t->name, o.field)) {   // s014b
            auto l = find_label("summary -- not searched", "acorn-null");
            l->add_css_class("acorn-form-static");
            w = l;
        } else if (!t->column(o.field) && m_solution.calc(t->name, o.field)) {   // s014a
            auto l = find_label("calculation -- not searched", "acorn-null");
            l->add_css_class("acorn-form-static");
            l->set_tooltip_text("“" + o.field + "” is worked out by Acorn, not kept in the database -- Find can't ask it yet");
            w = l;
        } else if (core::missing_field(o, fields)) {
            w = find_label("missing field “" + o.field + "”", "acorn-missing");
        } else if (const db::Column* col = t->column(o.field); col && col->type == db::FieldType::Container) {
            auto l = find_label("binary -- not searched", "acorn-null");
            l->add_css_class("acorn-form-static");
            w = l;
        } else {
            w = entry("field|" + o.field, "find.field." + o.field,
                      col && col->type == db::FieldType::Date ? "YYYY-MM-DD, <, >" : nullptr);
        }
        w->set_size_request(o.w, o.h);
        fixed->put(*w, o.x, o.y - top);
    }
    outer->append(*fixed);

    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->set_name("shell.browse.find_cancel");
    cancel->set_tooltip_text("Back to the records as they were (Esc)");
    cancel->signal_clicked().connect([this, gen] { if (gen == m_view_gen) cancel_find(); });
    auto go = Gtk::make_managed<Gtk::Button>("Find");
    go->add_css_class("acorn-primary");
    go->set_name("shell.browse.find_go");
    go->set_tooltip_text("Find the records that match every box of any request -- less what an Omit request matches (Enter)");
    go->signal_clicked().connect([this, gen] { if (gen == m_view_gen) do_find(); });
    buttons->append(*cancel);
    buttons->append(*go);
    outer->append(*buttons);

    m_browse_painting = true;
    m_browse_scroll.set_child(*outer);
    m_browse_painting = false;
    if (auto* e = focus_back ? focus_back : first) {
        e->grab_focus();
        e->set_position(-1);
    }
}

void Shell::find_request(int to) {  // find: s022 -- show request `to` (-1: a new, empty one at the end)
    if (!m_finding) return;
    if (to < 0) {
        m_find_reqs.emplace_back();
        m_find_at = m_find_reqs.size() - 1;
    } else if (static_cast<std::size_t>(to) < m_find_reqs.size()) {
        m_find_at = static_cast<std::size_t>(to);
    } else {
        return;
    }
    browse_note("Request " + std::to_string(m_find_at + 1) + " of " + std::to_string(m_find_reqs.size()) +
                " -- a record matching every box of ANY request is found; Omit leaves out what it matches.", "ok");
    paint_browse();
}

void Shell::delete_find_request() {  // find: s022 -- the shown request goes (never the last one)
    if (!m_finding || m_find_reqs.size() < 2) return;
    m_find_reqs.erase(m_find_reqs.begin() + static_cast<std::ptrdiff_t>(m_find_at));
    if (m_find_at >= m_find_reqs.size()) m_find_at = m_find_reqs.size() - 1;
    browse_note("Request deleted -- " + std::to_string(m_find_reqs.size()) + " left.", "ok");
    paint_browse();
}

}  // namespace acorn
