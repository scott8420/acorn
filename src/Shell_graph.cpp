#include "Shell.hpp"
#include "Log.hpp"

#include <gdkmm/contentprovider.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/droptarget.h>
#include <glibmm/miscutils.h>
#include <gtkmm/fixed.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/gestureclick.h>
#include <pangomm/fontdescription.h>
#include <gtkmm/label.h>
#include <gtkmm/overlay.h>
#include <gtkmm/separator.h>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <map>
#include <set>

// Shell_graph.cpp -- the GRAPH area (s008b): every table as a box, laid out
// automatically (parents left, the tables that point at them to the right),
// a line for each link from the child's field to the parent's key.
//
// Making a link is a drag, either way round: a field onto the table it should
// point at (or onto that table's key), or a key onto the field that will
// point at it. Acorn works out which table is the parent (the one whose KEY
// is in the drag). A popover then shows what deleting a parent record does
// (Restrict / Cascade / Set Empty), the SQL that will run, and Link. A line's
// handle opens the same popover for that link, with Unlink.
//
// Every change is a verb through m_journal (new_link / change_link /
// delete_link). Records that stop a link (they point at nothing) are MARKED:
// the popover says why, and Browse shows them with an amber edge and a hint.

namespace acorn {

namespace {

constexpr int kColWidth = 380;   // a box column, including the gap for lines (and their handles)
constexpr int kGapY     = 28;
constexpr int kMargin   = 24;

std::string curly(const std::string& s) { return "“" + s + "”"; }

std::string words_of(db::OnDelete o) {
    switch (o) {
    case db::OnDelete::Cascade:  return "Deleting a parent record deletes the records that point at it.";
    case db::OnDelete::SetEmpty: return "Deleting a parent record empties the field in the records that point at it.";
    default:                     return "A parent record that others point at can't be deleted.";
    }
}

// A field's kind, short enough for a box.
std::string kind_short(db::FieldType t) {
    switch (t) {
    case db::FieldType::Text:      return "text";
    case db::FieldType::Integer:   return "whole number";
    case db::FieldType::Number:    return "number";
    case db::FieldType::Date:      return "date";
    case db::FieldType::Time:      return "time";
    case db::FieldType::Timestamp: return "timestamp";
    case db::FieldType::Boolean:   return "yes-no";
    case db::FieldType::Container: return "container";
    default:                       return "other";
    }
}

// One drag's payload: "table\x1f" "field".
std::string payload(const std::string& t, const std::string& f) { return t + '\x1f' + f; }
bool unpack(const std::string& s, std::string& t, std::string& f) {
    const auto p = s.find('\x1f');
    if (p == std::string::npos) return false;
    t = s.substr(0, p);
    f = s.substr(p + 1);
    return true;
}

// Where `w` sits inside `in` (false: not laid out yet).
bool bounds(Gtk::Widget& w, Gtk::Widget& in, graphene_rect_t& r) {
    return gtk_widget_compute_bounds(w.gobj(), in.gobj(), &r);
}

}  // namespace

void Shell::build_graph_page() {  // graph: the Graph area
    m_graph_page.set_margin(28);
    m_graph_page.set_spacing(12);

    auto hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    auto title = Gtk::make_managed<Gtk::Label>("Graph");
    title->set_xalign(0.0f);
    title->add_css_class("acorn-hero-title");
    m_graph_where.set_xalign(0.0f);
    m_graph_where.set_wrap(true);
    m_graph_where.add_css_class("acorn-hero-sub");
    m_graph_note.set_xalign(0.0f);
    m_graph_note.set_wrap(true);
    m_graph_note.add_css_class("acorn-status");
    m_graph_note.set_visible(false);
    hero->append(*title);
    hero->append(m_graph_where);
    hero->append(m_graph_note);
    m_graph_page.append(*hero);

    m_graph_scroll.set_vexpand(true);
    m_graph_scroll.set_hexpand(true);
    m_graph_scroll.add_css_class("acorn-graph");
    m_graph_page.append(m_graph_scroll);

    // The one popover, pointed at whatever it is about.
    m_graph_pop.set_parent(m_graph_page);
    m_graph_pop.set_autohide(true);
    m_graph_pop.add_css_class("acorn-graph-pop");

    m_stack.add(m_graph_page, "graph", "Graph");
    paint_graph();
}

void Shell::graph_note(const std::string& text, const char* kind) {  // graph: under the Graph title
    m_graph_note.set_text(text);
    m_graph_note.set_visible(!text.empty());
    m_graph_note.remove_css_class("ok");
    m_graph_note.remove_css_class("error");
    if (kind && *kind) m_graph_note.add_css_class(kind);
}

void Shell::paint_graph() {  // graph: boxes laid out, lines drawn
    ++m_graph_gen;
    m_graph_canvas = nullptr;
    m_graph_lines.clear();
    m_graph_rows.clear();
    if (m_graph_pop.get_visible()) m_graph_pop.popdown();

    std::vector<const db::Table*> tables;
    for (const auto& t : m_schema.tables)
        if (t.kind == db::Table::Kind::Table) tables.push_back(&t);
    std::size_t nlinks = 0;
    for (const auto* t : tables) nlinks += t->foreign_keys.size();

    if (!m_conn) m_graph_where.set_text("Connect to a database first.");
    else if (tables.empty()) m_graph_where.set_text("This database has no tables yet.");
    else {
        std::string w = std::to_string(tables.size()) + (tables.size() == 1 ? " table · " : " tables · ") +
                        std::to_string(nlinks) + (nlinks == 1 ? " link" : " links");
        w += m_writable ? " · drag a field onto the table it points at, or a key onto the field that points at it;"
                          " drag a title to move a box"
                        : " · read-only -- drag a title to move a box";
        m_graph_where.set_text(w);
    }
    if (tables.empty()) {
        auto empty = Gtk::make_managed<Gtk::Label>(m_conn ? "No tables to draw." : "Tables appear here once you connect.");
        empty->add_css_class("acorn-status");
        empty->set_valign(Gtk::Align::START);
        empty->set_halign(Gtk::Align::START);
        m_graph_scroll.set_child(*empty);
        return;
    }

    // ── layout: a column per depth -- a table goes right of every table it
    // points at (links to itself don't count; a loop stops after N passes).
    std::map<std::string, int> col;
    for (const auto* t : tables) col[t->name] = 0;
    for (std::size_t pass = 0; pass < tables.size(); ++pass) {
        bool moved = false;
        for (const auto* t : tables)
            for (const auto& fk : t->foreign_keys) {
                if (fk.ref_table == t->name || !col.count(fk.ref_table)) continue;
                if (col[t->name] < col[fk.ref_table] + 1) { col[t->name] = col[fk.ref_table] + 1; moved = true; }
            }
        if (!moved) break;
    }
    std::map<int, std::vector<const db::Table*>> by_col;
    for (const auto* t : tables) by_col[col[t->name]].push_back(t);

    auto overlay = Gtk::make_managed<Gtk::Overlay>();
    auto lines = Gtk::make_managed<Gtk::DrawingArea>();
    auto fixed = Gtk::make_managed<Gtk::Fixed>();
    fixed->set_name("shell.graph.boxes");
    overlay->set_child(*lines);
    overlay->add_overlay(*fixed);
    overlay->set_halign(Gtk::Align::START);
    overlay->set_valign(Gtk::Align::START);

    // ── the boxes
    struct Head { Gtk::Widget* head; Gtk::Widget* box; std::string table; };
    auto heads = std::make_shared<std::vector<Head>>();
    int width = 0, height = 0;
    std::map<std::string, Gtk::Box*> boxes;
    for (auto& [c, list] : by_col) {
        int y = kMargin;
        for (const auto* t : list) {
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
            box->add_css_class("acorn-graph-box");
            box->set_size_request(220, -1);
            box->set_name("graph.box." + t->name);

            auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            head->add_css_class("acorn-graph-head");
            auto name = Gtk::make_managed<Gtk::Label>(t->name);
            name->set_xalign(0.0f);
            name->set_hexpand(true);
            name->set_ellipsize(Pango::EllipsizeMode::END);
            name->add_css_class("acorn-graph-title");
            head->append(*name);
            if (auto m = m_marks.find(t->name); m != m_marks.end() && !m->second.ids.empty()) {
                auto badge = Gtk::make_managed<Gtk::Label>("⚠ " + std::to_string(m->second.ids.size()));
                badge->add_css_class("acorn-mark");
                badge->set_tooltip_text(m->second.why);
                head->append(*badge);
            }
            head->set_tooltip_text(std::string("Drag to move the box") +
                                   (m_writable ? " -- or drop a field here to make it point at " + curly(t->name) + "'s key" : ""));
            box->append(*head);
            // The box (its title) takes a drop: "point at this table".
            if (m_writable) {
                auto dt = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::COPY);
                const std::string tn = t->name;
                dt->signal_drop().connect([this, tn, head](const Glib::ValueBase& v, double, double) {
                    Glib::Value<Glib::ustring> u;
                    u.init(v.gobj());
                    std::string st, sf;
                    if (!unpack(u.get(), st, sf)) return false;
                    on_graph_drop(st, sf, tn, "", *head);
                    return true;
                }, false);
                head->add_controller(dt);
            }

            const auto pk = t->primary_key();
            for (const auto& c : t->columns) {
                auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
                row->add_css_class("acorn-graph-field");
                const bool key = c.pk_position > 0;
                bool linked = false;
                for (const auto& fk : t->foreign_keys)
                    if (std::find(fk.columns.begin(), fk.columns.end(), c.name) != fk.columns.end()) linked = true;
                if (key) row->add_css_class("key");
                if (linked) row->add_css_class("linked");
                auto fl = Gtk::make_managed<Gtk::Label>(c.name);
                fl->set_xalign(0.0f);
                fl->set_hexpand(true);
                fl->set_ellipsize(Pango::EllipsizeMode::END);
                row->append(*fl);
                auto kind = Gtk::make_managed<Gtk::Label>(key ? (pk.size() > 1 ? "key " + std::to_string(c.pk_position) : "key")
                                                              : kind_short(c.type));
                kind->add_css_class("acorn-graph-kind");
                row->append(*kind);
                box->append(*row);
                m_graph_rows[{t->name, c.name}] = row;
                if (!m_writable) continue;
                const std::string tn = t->name, fn = c.name;
                row->set_tooltip_text(key ? "Drag onto a field that should point at this record's key -- or drop a field here"
                                          : "Drag onto the table this field should point at (or onto its key)");
                auto src = Gtk::DragSource::create();
                src->set_actions(Gdk::DragAction::COPY);
                src->signal_prepare().connect([tn, fn](double, double) {
                    Glib::Value<Glib::ustring> v;
                    v.init(Glib::Value<Glib::ustring>::value_type());
                    v.set(payload(tn, fn));
                    return Gdk::ContentProvider::create(v);
                }, false);
                row->add_controller(src);
                auto dt = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::COPY);
                dt->signal_drop().connect([this, tn, fn, row](const Glib::ValueBase& v, double, double) {
                    Glib::Value<Glib::ustring> u;
                    u.init(v.gobj());
                    std::string st, sf;
                    if (!unpack(u.get(), st, sf)) return false;
                    on_graph_drop(st, sf, tn, fn, *row);
                    return true;
                }, false);
                row->add_controller(dt);
            }
            // s009: a box placed by hand sits where the solution says; the
            // rest keep the automatic layout.
            double x = kMargin + c * kColWidth, by = y;
            if (auto pl = m_solution.boxes.find(t->name); pl != m_solution.boxes.end()) { x = pl->second.x; by = pl->second.y; }
            fixed->put(*box, x, by);
            boxes[t->name] = box;
            int mh = 0, nh = 0, mb = 0, nb = 0;
            box->measure(Gtk::Orientation::VERTICAL, 220, mh, nh, mb, nb);
            y += nh + kGapY;
            width = std::max(width, static_cast<int>(x) + 220 + kMargin);
            height = std::max(height, std::max(y, static_cast<int>(by) + nh + kGapY));

            // s009: a box moves by its title (the gesture is on the canvas
            // below, which doesn't move -- see after the loop).
            if (m_journal) {
                heads->push_back({head, box, t->name});
                gtk_widget_set_cursor_from_name(GTK_WIDGET(head->gobj()), "grab");   // gtkmm 4.10 lacks the wrapper
            }
        }
    }

    // s009: drag a box by its title to place it -- one step (Move “t”), kept
    // in the solution; Ctrl+Z moves it back. On the Fixed, whose coordinates
    // stay put while the box moves under the pointer.
    if (m_journal) {
        struct Moving { Head* h = nullptr; double x = 0, y = 0; };
        auto moving = std::make_shared<Moving>();
        auto drag = Gtk::GestureDrag::create();
        drag->set_button(GDK_BUTTON_PRIMARY);
        auto* dg = drag.get();
        drag->signal_drag_begin().connect([this, heads, moving, fixed, dg](double px, double py) {
            moving->h = nullptr;
            for (auto& h : *heads) {
                graphene_rect_t r;
                if (!bounds(*h.head, *fixed, r)) continue;
                if (px >= r.origin.x && px < r.origin.x + r.size.width && py >= r.origin.y && py < r.origin.y + r.size.height) {
                    moving->h = &h;
                    fixed->get_child_position(*h.box, moving->x, moving->y);
                }
            }
            if (!moving->h) { dg->set_state(Gtk::EventSequenceState::DENIED); return; }
            dg->set_state(Gtk::EventSequenceState::CLAIMED);
        });
        drag->signal_drag_update().connect([moving, fixed, lines](double dx, double dy) {
            if (!moving->h) return;
            fixed->move(*moving->h->box, std::max(0.0, moving->x + dx), std::max(0.0, moving->y + dy));
            lines->queue_draw();
        });
        drag->signal_drag_end().connect([this, moving](double dx, double dy) {
            if (!moving->h) return;
            const std::string tn = moving->h->table;
            moving->h = nullptr;
            if (std::abs(dx) < 3 && std::abs(dy) < 3) { Glib::signal_idle().connect_once([this] { paint_graph(); }); return; }
            const double nx = std::max(0.0, std::round(moving->x + dx));
            const double ny = std::max(0.0, std::round(moving->y + dy));
            // Repaint after the gesture has finished with this canvas.
            Glib::signal_idle().connect_once([this, tn, nx, ny] {
                auto r = db::verbs::move_box(*m_journal, tn, nx, ny);
                if (auto lg = log::get(log::Area::App))
                    lg->info("solution: move '{}' -> {},{}: {}", tn, nx, ny, r ? "kept" : r.error().message);
                paint_graph();
                if (r) graph_note("Moved " + curly(tn) + ". Ctrl+Z moves it back.", "ok");
                else   graph_note("Not moved: " + r.error().message + ".", "error");
            });
        });
        fixed->add_controller(drag);
    }

    // ── the lines, each with a handle naming its rule at its middle -- both
    // drawn together from where the rows really are, at draw time (a handle
    // placed beforehand could land before GTK has laid the boxes out).
    for (const auto* t : tables)
        for (const auto& fk : t->foreign_keys) {
            GraphLine gl;
            gl.child = t->name;
            gl.fields = fk.columns;
            gl.parent = fk.ref_table;
            gl.refs = m_schema.ref_columns(fk);
            db::LinkDef ld{fk.name, fk.columns, fk.ref_table, gl.refs, fk.on_delete, fk.on_update};
            gl.rule = ld.deleting();
            m_graph_lines.push_back(std::move(gl));
        }
    lines->set_content_width(width + 80);
    lines->set_content_height(height + 40);

    // Lines are drawn from where the rows really are (asked at draw time).
    lines->set_draw_func([this, lines](const Cairo::RefPtr<Cairo::Context>& cr, int, int) {
        cr->set_line_width(2.0);
        for (const auto& gl : m_graph_lines) {
            auto a = m_graph_rows.find({gl.child, gl.fields.empty() ? "" : gl.fields[0]});
            auto b = m_graph_rows.find({gl.parent, gl.refs.empty() ? "" : gl.refs[0]});
            if (a == m_graph_rows.end() || b == m_graph_rows.end()) continue;
            graphene_rect_t ra, rb;
            if (!bounds(*a->second, *lines, ra) || !bounds(*b->second, *lines, rb)) continue;
            double x1, y1 = ra.origin.y + ra.size.height / 2, x2, y2 = rb.origin.y + rb.size.height / 2, bend;
            if (gl.child == gl.parent) {   // to itself: a loop out on the right
                x1 = ra.origin.x + ra.size.width;
                x2 = rb.origin.x + rb.size.width;
                bend = 48;
                cr->move_to(x1, y1);
                cr->curve_to(x1 + bend, y1, x2 + bend, y2, x2, y2);
            } else {
                const bool right = ra.origin.x > rb.origin.x;   // the child sits right of its parent
                x1 = right ? ra.origin.x : ra.origin.x + ra.size.width;
                x2 = right ? rb.origin.x + rb.size.width : rb.origin.x;
                bend = std::max(40.0, std::abs(x1 - x2) / 2);
                if (std::abs(x1 - x2) < 1 && std::abs(ra.origin.x - rb.origin.x) < 1) {   // same column: loop right
                    x1 = ra.origin.x + ra.size.width;
                    x2 = rb.origin.x + rb.size.width;
                    cr->move_to(x1, y1);
                    cr->curve_to(x1 + 60, y1, x2 + 60, y2, x2, y2);
                } else {
                    cr->move_to(x1, y1);
                    cr->curve_to(right ? x1 - bend : x1 + bend, y1, right ? x2 + bend : x2 - bend, y2, x2, y2);
                }
            }
            if (gl.rule == db::OnDelete::Cascade) cr->set_source_rgba(1.0, 0.60, 0.54, 0.85);
            else if (gl.rule == db::OnDelete::SetEmpty) cr->set_source_rgba(0.62, 0.83, 0.88, 0.85);
            else cr->set_source_rgba(0.96, 0.73, 0.26, 0.85);
            cr->stroke();
            // the key end: a small dot on the parent
            cr->arc(x2, y2, 4.0, 0, 2 * M_PI);
            cr->fill();
            // the handle: a pill at the middle, the rule's name in it
            double mx, my = (y1 + y2) / 2;
            if (gl.child == gl.parent || std::abs(ra.origin.x - rb.origin.x) < 1)
                mx = std::max(ra.origin.x + ra.size.width, rb.origin.x + rb.size.width) + 30;
            else
                mx = (x1 + x2) / 2;
            auto layout = lines->create_pango_layout(db::on_delete_words(gl.rule));
            Pango::FontDescription fd("Sans 8");
            layout->set_font_description(fd);
            int tw = 0, th = 0;
            layout->get_pixel_size(tw, th);
            const double w = tw + 16, h = th + 4, x = mx - w / 2, y = my - h / 2, r = h / 2;
            cr->begin_new_sub_path();
            cr->arc(x + w - r, y + r, r, -M_PI / 2, M_PI / 2);
            cr->arc(x + r, y + r, r, M_PI / 2, 3 * M_PI / 2);
            cr->close_path();
            cr->set_source_rgba(0.08, 0.15, 0.11, 0.95);
            cr->fill_preserve();
            cr->set_source_rgba(0.96, 0.73, 0.26, 0.45);
            cr->set_line_width(1.0);
            cr->stroke();
            cr->set_source_rgba(0.96, 0.73, 0.26, 1.0);
            cr->move_to(x + 8, y + 2);
            layout->show_in_cairo_context(cr);
            cr->set_line_width(2.0);
            gl.hit = {x, y, w, h};
        }
    });
    // A click on a handle opens its link's popover (the canvas on top hears it).
    if (m_writable) {
        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_PRIMARY);
        click->signal_released().connect([this](int n, double px, double py) {
            if (n != 1) return;
            for (std::size_t i = 0; i < m_graph_lines.size(); ++i) {
                const auto& h = m_graph_lines[i].hit;
                if (h.w > 0 && px >= h.x && px < h.x + h.w && py >= h.y && py < h.y + h.h) { offer_line(i); return; }
            }
        });
        fixed->add_controller(click);
    }

    m_graph_canvas = fixed;
    m_graph_scroll.set_child(*overlay);
}

void Shell::on_graph_drop(const std::string& st, const std::string& sf, const std::string& dt, const std::string& df,
                          Gtk::Widget& anchor) {  // graph: a field dropped -- which table is the parent?
    if (!m_journal || !m_writable) return;
    const db::Table* s = m_schema.table(st);
    const db::Table* d = m_schema.table(dt);
    if (!s || !d) return;
    if (st == dt && sf == df) return;   // dropped where it started
    auto lone_key = [](const db::Table& t, const std::string& f) {
        const auto pk = t.primary_key();
        return pk.size() == 1 && pk[0] == f;
    };
    std::string child, field, parent;
    if (df.empty() || lone_key(*d, df)) { child = st; field = sf; parent = dt; }   // a field onto a table / its key
    else if (lone_key(*s, sf))          { child = dt; field = df; parent = st; }   // a key onto a field
    else {
        graph_note("Neither end is a key: drop " + curly(sf) + " onto the table it should point at (or onto its key), "
                   "or drag a key onto the field that will point at it.", "error");
        return;
    }
    const db::Table* p = m_schema.table(parent);
    if (p && p->primary_key().size() > 1) {
        graph_note(curly(parent) + "'s key has " + std::to_string(p->primary_key().size()) +
                   " fields -- the graph links one field at a time for now.", "error");
        return;
    }
    if (p && p->primary_key().empty()) {
        graph_note(curly(parent) + " has no key -- a link points at a table's key.", "error");
        return;
    }
    offer_link(child, field, parent, anchor);   // (the note stays: hiding it would move the boxes under the popover)
}

namespace {
graphene_rect_t anchor_rect(Gtk::Widget& w, Gtk::Widget& in) {
    graphene_rect_t r{};
    if (!bounds(w, in, r)) r = graphene_rect_t{{0, 0}, {1, 1}};
    return r;
}
}  // namespace

// The popover's body: the rule, what it means, the SQL, the refusal (and why
// records are marked), and the buttons. `existing`: a line's popover.
void Shell::offer_link(const std::string& child, const std::string& field, const std::string& parent,
                       Gtk::Widget& anchor) {  // graph: the popover for a new link
    auto r = anchor_rect(anchor, m_graph_page);
    m_graph_pop.set_pointing_to(Gdk::Rectangle(static_cast<int>(r.origin.x), static_cast<int>(r.origin.y),
                                               std::max(1, static_cast<int>(r.size.width)),
                                               std::max(1, static_cast<int>(r.size.height))));
    build_link_popover(child, {field}, parent, db::OnDelete::Restrict, false);
    m_graph_pop.popup();
}

void Shell::offer_line(std::size_t idx) {  // graph: a line's handle -> change / unlink
    if (idx >= m_graph_lines.size() || !m_graph_canvas) return;
    const GraphLine gl = m_graph_lines[idx];
    const auto c = anchor_rect(*m_graph_canvas, m_graph_page);   // the handle sits in the canvas
    m_graph_pop.set_pointing_to(Gdk::Rectangle(static_cast<int>(c.origin.x + gl.hit.x), static_cast<int>(c.origin.y + gl.hit.y),
                                               std::max(1, static_cast<int>(gl.hit.w)), std::max(1, static_cast<int>(gl.hit.h))));
    build_link_popover(gl.child, gl.fields, gl.parent, gl.rule, true);
    m_graph_pop.popup();
}

void Shell::build_link_popover(const std::string& child, const std::vector<std::string>& fields,
                               const std::string& parent, db::OnDelete rule, bool existing) {  // graph: the popover's body
    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(10);
    body->set_size_request(380, -1);
    std::string f;
    for (std::size_t i = 0; i < fields.size(); ++i) f += (i ? ", " : "") + curly(fields[i]);
    auto title = Gtk::make_managed<Gtk::Label>("Link " + curly(child) + " to " + curly(parent));
    title->set_xalign(0.0f);
    title->add_css_class("acorn-section");
    body->append(*title);
    auto sub = Gtk::make_managed<Gtk::Label>(curly(child) + "'s " + f + " points at " + curly(parent) + "'s key.");
    sub->set_xalign(0.0f);
    sub->set_wrap(true);
    sub->add_css_class("acorn-status");
    body->append(*sub);

    auto when = Gtk::make_managed<Gtk::Label>("When a " + curly(parent) + " record is deleted:");
    when->set_xalign(0.0f);
    body->append(*when);
    auto* rb = Gtk::make_managed<Gtk::CheckButton>("Restrict -- refuse while records point at it");
    auto* cb = Gtk::make_managed<Gtk::CheckButton>("Cascade -- delete the records that point at it");
    auto* eb = Gtk::make_managed<Gtk::CheckButton>("Set Empty -- empty " + f + " in those records");
    cb->set_group(*rb);
    eb->set_group(*rb);
    (rule == db::OnDelete::Cascade ? cb : rule == db::OnDelete::SetEmpty ? eb : rb)->set_active(true);
    body->append(*rb);
    body->append(*cb);
    body->append(*eb);

    auto sql = Gtk::make_managed<Gtk::Label>();
    sql->set_xalign(0.0f);
    sql->set_wrap(true);
    sql->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    sql->set_selectable(true);
    sql->add_css_class("acorn-mono");
    sql->add_css_class("acorn-graph-sql");
    body->append(*sql);
    auto why = Gtk::make_managed<Gtk::Label>();
    why->set_xalign(0.0f);
    why->set_wrap(true);
    why->set_visible(false);
    body->append(*why);
    auto show = Gtk::make_managed<Gtk::Button>("Show them in Browse");
    show->add_css_class("acorn-link");
    show->set_halign(Gtk::Align::START);
    show->set_visible(false);
    body->append(*show);

    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto unlink = Gtk::make_managed<Gtk::Button>("Unlink");
    unlink->set_visible(existing);
    auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto go = Gtk::make_managed<Gtk::Button>(existing ? "Apply" : "Link");
    go->add_css_class("acorn-primary");
    buttons->append(*unlink);
    buttons->append(*cancel);
    buttons->append(*go);
    body->append(*buttons);

    auto chosen = [rb, cb, eb] {
        return cb->get_active() ? db::OnDelete::Cascade : eb->get_active() ? db::OnDelete::SetEmpty : db::OnDelete::Restrict;
    };
    // The picture the verb will make -- built the verb's own way (linked()).
    auto picture = [this, child, fields, parent, existing](db::OnDelete o) -> std::expected<db::TableDef, std::string> {
        m_journal->store().forget_schema();
        auto got = m_journal->store().get(db::Store::table_id(child));
        if (m_journal->store().failed() || !got) {
            std::string e = m_journal->store().failed() ? m_journal->store().error().message : "it is not there any more";
            m_journal->store().clear_failed();
            return std::unexpected(curly(child) + ": " + e);
        }
        db::TableDef live = std::get<db::TableDef>(*got);
        if (existing) {
            for (auto& l : live.links)
                if (l.columns == fields) { l.on_delete = db::on_delete_sql(o); l.name.clear(); }
            return live;
        }
        const db::Table* p = m_schema.table(parent);
        if (!p) return std::unexpected("there is no table " + curly(parent));
        return db::verbs::linked(live, *p, fields, o);
    };
    auto refresh = [this, picture, chosen, sql, why, show, go, child, fields, parent, existing, rule] {
        const auto o = chosen();
        go->set_sensitive(false);
        show->set_visible(false);
        why->set_visible(false);
        why->remove_css_class("error");
        auto w = picture(o);
        if (!w) { sql->set_text(""); why->set_text(w.error()); why->add_css_class("error"); why->set_visible(true); return; }
        auto pv = db::verbs::preview(*m_journal, *w);
        std::string text;
        if (pv) for (const auto& st : *pv) text += (text.empty() ? "" : ";\n") + st;
        sql->set_text(existing && o == rule ? "No change." : text);
        auto loss = db::verbs::design_losses(*m_journal, *w);
        if (!loss) {
            // Records that point at nothing: marked, and said why.
            auto um = db::verbs::unmatched(*m_journal, child, fields, parent);
            if (um && !um->empty()) {
                Mark m;
                m.ids = *um;
                m.parent = parent;
                m.fields = fields;
                std::string f;
                for (std::size_t i = 0; i < fields.size(); ++i) f += (i ? ", " : "") + curly(fields[i]);
                m.why = "This record's " + f + " points at a " + curly(parent) + " record that isn't there, so " +
                        curly(child) + " can't be linked to " + curly(parent) + " yet. Give it one that exists, or "
                        "empty the field -- Acorn never deletes or empties records to make a link fit.";
                m_marks[child] = m;
                why->set_text(loss.error().message + ".\n\n" + (um->size() == 1 ? "It's" : "They're") +
                              " marked ⚠ in Browse -- hover to see why. Acorn never deletes or empties records "
                              "to make a link fit.");
                show->set_visible(true);
                paint_marks_note();
            } else {
                why->set_text(loss.error().message + ".");
            }
            why->add_css_class("error");
            why->set_visible(true);
            return;
        }
        std::string text2;   // the step's own words, else what the rule means
        for (const auto& l : *loss) text2 += (text2.empty() ? "" : "\n") + l + ".";
        if (text2.empty()) text2 = words_of(o);
        why->set_text(text2);
        why->set_visible(true);
        go->set_sensitive(pv.has_value() && !(existing && o == rule));
    };
    for (auto* b : {rb, cb, eb})
        b->signal_toggled().connect([b, refresh] { if (b->get_active()) refresh(); });
    cancel->signal_clicked().connect([this] { m_graph_pop.popdown(); });
    show->signal_clicked().connect([this, child] { m_graph_pop.popdown(); open_browse(child); });
    go->signal_clicked().connect([this, chosen, child, fields, parent, existing, why] {
        const auto o = chosen();
        auto r = existing ? db::verbs::change_link(*m_journal, child, fields, o)
                          : db::verbs::new_link(*m_journal, child, fields, parent, o);
        if (auto lg = log::get(log::Area::App))
            lg->info("journal: link '{}' -> '{}': {}", child, parent, r ? m_journal->undo_label() : r.error().message);
        if (!r) {
            why->set_text("Not linked: " + r.error().message + ".");
            why->add_css_class("error");
            why->set_visible(true);
            return;
        }
        m_marks.erase(child);
        m_graph_pop.popdown();
        reload_schema("");
        journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
    });
    unlink->signal_clicked().connect([this, child, fields, why] {
        auto r = db::verbs::delete_link(*m_journal, child, fields);
        if (auto lg = log::get(log::Area::App))
            lg->info("journal: unlink '{}': {}", child, r ? m_journal->undo_label() : r.error().message);
        if (!r) {
            why->set_text("Not unlinked: " + r.error().message + ".");
            why->add_css_class("error");
            why->set_visible(true);
            return;
        }
        m_graph_pop.popdown();
        reload_schema("");
        journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
    });
    m_graph_pop.set_child(*body);
    refresh();
    go->grab_focus();
}

void Shell::open_solution(const core::SavedConnection& c) {  // graph: the connection's .acorn -- found, or a fresh one
    namespace fs = std::filesystem;
    const std::string app_dir = (fs::path(Glib::get_user_data_dir()) / "acorn").string();
    const bool beside = c.is_file() && core::folder_writable(fs::path(c.path).parent_path().string());
    m_solution_path = core::solution_path(c, app_dir, beside);
    auto s = core::load_solution(m_solution_path);
    m_solution = s ? *s : core::Solution{};
    m_solution.points_at = c;
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: {} ({})", m_solution_path, s ? "read" : "none yet -- written on the first move");
    if (m_journal)
        m_journal->store().set_solution(&m_solution, [this] { return core::save_solution(m_solution_path, m_solution); });
}

void Shell::refresh_marks() {  // graph: marks follow the records (fixed ones drop off)
    if (!m_journal) { m_marks.clear(); return; }
    for (auto it = m_marks.begin(); it != m_marks.end();) {
        auto um = db::verbs::unmatched(*m_journal, it->first, it->second.fields, it->second.parent);
        if (!um || um->empty()) it = m_marks.erase(it);
        else { it->second.ids = *um; ++it; }
    }
}

std::string Shell::mark_of(const std::string& table, const std::string& id) const {  // graph: why a record is marked
    auto m = m_marks.find(table);
    if (m == m_marks.end() || id.empty()) return {};
    return std::find(m->second.ids.begin(), m->second.ids.end(), id) != m->second.ids.end() ? m->second.why : std::string{};
}

void Shell::paint_marks_note() {  // graph: Browse's line about marked records, then repaint what shows them
    if (!m_browse_table.empty()) load_browse_page(true);
}

}  // namespace acorn
