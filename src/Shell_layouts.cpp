#include "Shell.hpp"
#include "Log.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>

// Shell_layouts.cpp -- the LAYOUTS area (s010b): the layout editor.
//
// A table picker, its layouts (New / Duplicate / Rename / Delete), a canvas and
// an inspector. The canvas draws the shown layout as the Form places it --
// field objects as outlined boxes naming the field and its kind, text objects
// as their words, a field the table no longer has dashed. Press an object to
// pick it; drag its middle to move it, a handle (corners, sides) to resize it,
// snapped to the 8 px grid (Shift: by the pixel). The arrows nudge by 8 (Shift:
// by 1); Delete takes it off the layout; Esc lets go of it.
//
// EVERY edit is one verbs::save_layout -- a step on the same Ctrl+Z list as a
// record or a table ("Change Layout “contacts Form”"). Letting go of a drag is
// one step, not one per pixel. The default layout stays computed until its
// first edit, which keeps it ("New Layout “contacts Form”").
//
// The layout lives in the solution (the .acorn), never the database -- so
// Layouts works on a read-only connection too. Nothing here writes SQL.

namespace acorn {

namespace {

constexpr double kHandle = 7.0;   // a handle's square
constexpr double kHit    = 6.0;   // how near a press must land to grab a handle
constexpr int    kRoom   = 240;   // room past the body to drag into

struct Rgb { double r, g, b; };
constexpr Rgb kAmber{0xf5 / 255.0, 0xb9 / 255.0, 0x42 / 255.0};   // the skin's accent (@acorn_amber)

std::string curly(const std::string& s) { return "“" + s + "”"; }

// The eight handles of a box, with what each grabs.
std::vector<std::pair<core::Grab, std::pair<double, double>>> handles(const core::LayoutObject& o) {
    const double l = o.x, t = o.y, r = o.x + o.w, b = o.y + o.h, cx = o.x + o.w / 2.0, cy = o.y + o.h / 2.0;
    using G = core::Grab;
    return {{G::NW, {l, t}}, {G::N, {cx, t}}, {G::NE, {r, t}}, {G::E, {r, cy}},
            {G::SE, {r, b}}, {G::S, {cx, b}}, {G::SW, {l, b}}, {G::W, {l, cy}}};
}

const char* cursor_for(core::Grab g) {
    using G = core::Grab;
    switch (g) {
        case G::Move: return "move";
        case G::N: case G::S: return "ns-resize";
        case G::E: case G::W: return "ew-resize";
        case G::NW: case G::SE: return "nwse-resize";
        case G::NE: case G::SW: return "nesw-resize";
        default: return "default";
    }
}

Gtk::Label* side_label(const std::string& s, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    l->set_wrap(true);
    if (css) l->add_css_class(css);
    return l;
}

}  // namespace

// ── the model, as the UI sees it ────────────────────────────────────────────

std::vector<std::string> Shell::fields_of(const std::string& table) const {  // layouts: the live table's fields
    std::vector<std::string> out;
    if (const db::Table* t = m_schema.table(table))
        for (const auto& c : t->columns) out.push_back(c.name);
    return out;
}

std::vector<std::string> Shell::layout_names(const std::string& table) const {  // layouts: the default first, then the kept ones
    std::vector<std::string> out{core::default_layout_name(table)};
    if (auto it = m_solution.layouts.find(table); it != m_solution.layouts.end())
        for (const auto& [name, l] : it->second)
            if (name != out.front()) out.push_back(name);
    return out;
}

std::string Shell::picked_layout(const std::string& table) const {  // layouts: last picked, else the default
    if (auto it = m_layout_pick.find(table); it != m_layout_pick.end()) {
        const auto names = layout_names(table);
        if (std::find(names.begin(), names.end(), it->second) != names.end()) return it->second;
    }
    return core::default_layout_name(table);
}

core::Layout Shell::layout_of(const std::string& table, const std::string& name, bool* kept) const {  // layouts: kept, else computed
    if (kept) *kept = false;
    if (const auto* l = m_solution.layout(table, name)) {
        if (kept) *kept = true;
        return *l;
    }
    if (name == core::default_layout_name(table)) return core::default_layout(table, fields_of(table));
    return {};
}

// ── the area ────────────────────────────────────────────────────────────────

void Shell::build_layouts_page() {  // layouts: the Layouts area
    m_layouts_page.set_margin(28);

    // Title, the line under it, the note a step leaves.
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    hero->set_hexpand(true);
    auto title = Gtk::make_managed<Gtk::Label>("Layouts");
    title->set_xalign(0.0f);
    title->add_css_class("acorn-hero-title");
    m_layouts_where.set_xalign(0.0f);
    m_layouts_where.set_wrap(true);
    m_layouts_where.add_css_class("acorn-hero-sub");
    m_layouts_note.set_xalign(0.0f);
    m_layouts_note.set_wrap(true);
    m_layouts_note.add_css_class("acorn-status");
    m_layouts_note.set_visible(false);
    hero->append(*title);
    hero->append(m_layouts_where);
    hero->append(m_layouts_note);
    top->append(*hero);

    m_lay_tables = Gtk::StringList::create({});
    m_lay_table_pick.set_model(m_lay_tables);
    m_lay_table_pick.set_valign(Gtk::Align::CENTER);
    m_lay_table_pick.set_tooltip_text("The table whose layouts are shown");
    m_lay_table_pick.set_name("shell.layouts.table");
    m_lay_table_pick.property_selected().signal_changed().connect([this] {
        if (m_lay_painting) return;
        const auto i = m_lay_table_pick.get_selected();
        if (i == GTK_INVALID_LIST_POSITION || i >= m_schema.tables.size()) return;
        m_lay_table = m_schema.tables[i].name;
        m_lay_sel = -1;
        Glib::signal_idle().connect_once([this] { paint_layouts(); });
    });
    top->append(m_lay_table_pick);
    m_layouts_page.append(*top);

    // The bar: the layout picker and its verbs.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_lay_names = Gtk::StringList::create({});
    m_lay_name_pick.set_model(m_lay_names);
    m_lay_name_pick.set_tooltip_text("The layout shown -- the table's default, or one kept in the solution");
    m_lay_name_pick.set_name("shell.layouts.layout");
    m_lay_name_pick.property_selected().signal_changed().connect([this] {
        if (m_lay_painting || m_lay_table.empty()) return;
        const auto names = layout_names(m_lay_table);
        const auto i = m_lay_name_pick.get_selected();
        if (i == GTK_INVALID_LIST_POSITION || i >= names.size()) return;
        m_layout_pick[m_lay_table] = names[i];
        m_lay_sel = -1;
        Glib::signal_idle().connect_once([this] { paint_layouts(); });
    });
    bar->append(m_lay_name_pick);
    m_lay_new.set_label("New");
    m_lay_new.set_tooltip_text("A new layout: the table's fields, under a new name -- Ctrl+Z takes it away");
    m_lay_new.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_layout_new));
    m_lay_dup.set_label("Duplicate");
    m_lay_dup.set_tooltip_text("This layout again, under a new name");
    m_lay_dup.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_layout_duplicate));
    m_lay_rename.set_label("Rename…");
    m_lay_rename.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_layout_rename));
    m_lay_delete.set_label("Delete");
    m_lay_delete.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_layout_delete));
    for (auto* b : {&m_lay_new, &m_lay_dup, &m_lay_rename, &m_lay_delete}) bar->append(*b);
    m_lay_new.set_margin_start(6);
    m_layouts_caption.set_xalign(0.0f);
    m_layouts_caption.set_hexpand(true);
    m_layouts_caption.set_margin_start(12);
    m_layouts_caption.set_ellipsize(Pango::EllipsizeMode::END);
    m_layouts_caption.add_css_class("acorn-status");
    bar->append(m_layouts_caption);
    m_layouts_page.append(*bar);

    // The canvas: the body (styled as the Form's) under a drawing area that
    // draws the objects and takes every press.
    m_lay_paper.add_css_class("acorn-form");
    m_lay_paper.set_halign(Gtk::Align::START);
    m_lay_paper.set_valign(Gtk::Align::START);
    m_lay_fixed.put(m_lay_paper, 0, 0);
    m_lay_overlay.set_child(m_lay_fixed);
    m_lay_overlay.add_overlay(m_lay_canvas);
    m_lay_overlay.set_halign(Gtk::Align::START);
    m_lay_overlay.set_valign(Gtk::Align::START);
    m_lay_canvas.set_name("shell.layouts.canvas");
    m_lay_canvas.set_focusable(true);
    m_lay_canvas.set_focus_on_click(true);
    m_lay_canvas.set_draw_func(sigc::mem_fun(*this, &Shell::draw_layout));
    m_lay_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    m_lay_scroll.set_hexpand(true);
    m_lay_scroll.set_vexpand(true);
    m_lay_scroll.set_child(m_lay_overlay);

    // Press: pick (a handle of the picked object first, then the top-most
    // object under the pointer). Drag: move / resize, live. Let go: one step.
    auto drag = Gtk::GestureDrag::create();
    drag->set_button(GDK_BUTTON_PRIMARY);
    auto* dg = drag.get();
    drag->signal_drag_begin().connect([this](double px, double py) {
        m_lay_canvas.grab_focus();
        m_lay_grab = core::Grab::None;
        if (m_lay_sel >= 0 && static_cast<std::size_t>(m_lay_sel) < m_lay.objects.size())
            for (const auto& [g, p] : handles(m_lay.objects[static_cast<std::size_t>(m_lay_sel)]))
                if (std::abs(px - p.first) <= kHit && std::abs(py - p.second) <= kHit) m_lay_grab = g;
        if (m_lay_grab == core::Grab::None) {
            int hit = -1;
            for (int i = static_cast<int>(m_lay.objects.size()) - 1; i >= 0 && hit < 0; --i) {
                const auto& o = m_lay.objects[static_cast<std::size_t>(i)];
                if (px >= o.x && px < o.x + o.w && py >= o.y && py < o.y + o.h) hit = i;
            }
            const bool changed = hit != m_lay_sel;
            m_lay_sel = hit;
            if (hit >= 0) m_lay_grab = core::Grab::Move;
            if (changed) { m_lay_focus.clear(); paint_inspector(); }
        }
        if (m_lay_grab != core::Grab::None) m_lay_from = m_lay.objects[static_cast<std::size_t>(m_lay_sel)];
        m_lay_canvas.queue_draw();
    });
    drag->signal_drag_update().connect([this, dg](double dx, double dy) {
        if (m_lay_grab == core::Grab::None || m_lay_sel < 0 || !m_journal) return;
        const bool fine = (dg->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
        m_lay.objects[static_cast<std::size_t>(m_lay_sel)] =
            core::dragged(m_lay_from, m_lay_grab, static_cast<int>(std::lround(dx)), static_cast<int>(std::lround(dy)),
                          fine ? 1 : core::kLayoutGrid);
        m_lay_canvas.queue_draw();
    });
    drag->signal_drag_end().connect([this](double, double) {
        const core::Grab g = m_lay_grab;
        m_lay_grab = core::Grab::None;
        if (g == core::Grab::None || m_lay_sel < 0 || static_cast<std::size_t>(m_lay_sel) >= m_lay.objects.size()) return;
        const auto& now = m_lay.objects[static_cast<std::size_t>(m_lay_sel)];
        if (now == m_lay_from) return;   // a press, no move: only picked
        const std::string what = (g == core::Grab::Move ? "Moved " : "Resized ") + core::object_label(now);
        // After the gesture has finished with the canvas.
        Glib::signal_idle().connect_once([this, what] { keep_layout(m_lay, what); });
    });
    m_lay_canvas.add_controller(drag);

    // The pointer's shape says what a press would grab.
    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_motion().connect([this](double px, double py) {
        if (m_lay_grab != core::Grab::None) return;
        core::Grab g = core::Grab::None;
        if (m_lay_sel >= 0 && static_cast<std::size_t>(m_lay_sel) < m_lay.objects.size())
            for (const auto& [h, p] : handles(m_lay.objects[static_cast<std::size_t>(m_lay_sel)]))
                if (std::abs(px - p.first) <= kHit && std::abs(py - p.second) <= kHit) g = h;
        if (g == core::Grab::None)
            for (const auto& o : m_lay.objects)
                if (px >= o.x && px < o.x + o.w && py >= o.y && py < o.y + o.h) g = core::Grab::Move;
        gtk_widget_set_cursor_from_name(GTK_WIDGET(m_lay_canvas.gobj()), m_journal ? cursor_for(g) : "default");
    });
    m_lay_canvas.add_controller(motion);

    // The arrows nudge, Delete removes, Esc lets go -- the canvas's own keys
    // (Ctrl+Z still bubbles to the window: the canvas isn't a text box).
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) -> bool {
            if ((state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::ALT_MASK)) != Gdk::ModifierType{}) return false;
            const int step = (state & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK ? 1 : core::kLayoutGrid;
            switch (keyval) {
                case GDK_KEY_Left:  layout_nudge(-step, 0); return true;
                case GDK_KEY_Right: layout_nudge(step, 0);  return true;
                case GDK_KEY_Up:    layout_nudge(0, -step); return true;
                case GDK_KEY_Down:  layout_nudge(0, step);  return true;
                case GDK_KEY_Delete:
                case GDK_KEY_BackSpace: layout_remove_object(); return true;
                case GDK_KEY_Escape:
                    if (m_lay_sel < 0) return false;
                    m_lay_sel = -1;
                    m_lay_canvas.queue_draw();
                    paint_inspector();
                    return true;
                default: return false;
            }
        },
        false);
    m_lay_canvas.add_controller(keys);

    m_lay_inspector.set_size_request(240, -1);
    m_lay_inspector.set_hexpand(false);
    m_lay_inspector.add_css_class("acorn-panel");
    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    body->set_vexpand(true);
    body->append(m_lay_scroll);
    body->append(m_lay_inspector);
    m_layouts_page.append(*body);

    m_lay_pop.set_parent(m_layouts_page);
    m_lay_pop.set_autohide(true);
    m_lay_pop.add_css_class("acorn-graph-pop");   // the Graph's dark popover

    m_stack.add(m_layouts_page, "layouts", "Layouts");
    paint_layouts();
}

void Shell::layouts_note(const std::string& text, const char* kind) {  // layouts: under the Layouts title
    m_layouts_note.set_text(text);
    m_layouts_note.set_visible(!text.empty());
    m_layouts_note.remove_css_class("ok");
    m_layouts_note.remove_css_class("error");
    if (kind && *kind) m_layouts_note.add_css_class(kind);
}

void Shell::paint_layouts() {  // layouts: pickers, canvas and inspector from the solution
    if (!m_lay_tables || m_lay_grab != core::Grab::None) return;   // not built yet / never under a drag
    m_lay_painting = true;
    // The table: the one shown, else Browse's, else the first.
    std::vector<Glib::ustring> tnames;
    guint at = GTK_INVALID_LIST_POSITION;
    if (m_lay_table.empty() || !m_schema.table(m_lay_table)) m_lay_table = m_schema.table(m_browse_table) ? m_browse_table : "";
    if (m_lay_table.empty() && !m_schema.tables.empty()) m_lay_table = m_schema.tables[0].name;
    for (std::size_t i = 0; i < m_schema.tables.size(); ++i) {
        const auto& t = m_schema.tables[i];
        tnames.push_back(t.kind == db::Table::Kind::View ? t.name + " (view)" : t.name);
        if (t.name == m_lay_table) at = static_cast<guint>(i);
    }
    m_lay_tables->splice(0, m_lay_tables->get_n_items(), tnames);
    m_lay_table_pick.set_selected(at);
    m_lay_table_pick.set_sensitive(!tnames.empty());

    // The layout: the one last picked, else the default.
    std::vector<Glib::ustring> lnames;
    guint lat = GTK_INVALID_LIST_POSITION;
    if (!m_lay_table.empty()) {
        const auto names = layout_names(m_lay_table);
        const auto pick = picked_layout(m_lay_table);
        for (std::size_t i = 0; i < names.size(); ++i) {
            lnames.push_back(names[i]);
            if (names[i] == pick) lat = static_cast<guint>(i);
        }
        m_lay = layout_of(m_lay_table, pick, &m_lay_kept);
    } else {
        m_lay = {};
        m_lay_kept = false;
    }
    m_lay_names->splice(0, m_lay_names->get_n_items(), lnames);
    m_lay_name_pick.set_selected(lat);
    m_lay_name_pick.set_sensitive(!lnames.empty());
    m_lay_painting = false;
    if (m_lay_sel >= static_cast<int>(m_lay.objects.size())) m_lay_sel = -1;

    const bool on = m_journal && !m_lay_table.empty();
    if (!m_conn) m_layouts_where.set_text("Connect to a database first.");
    else if (m_lay_table.empty()) m_layouts_where.set_text("This database has no tables yet.");
    else {
        const auto n = lnames.size();
        m_layouts_where.set_text(curly(m_lay_table) + " · " + std::to_string(n) + (n == 1 ? " layout" : " layouts") +
                                 " · drag to move, a handle to resize (Shift: by the pixel) · arrows nudge · Delete removes");
    }
    m_layouts_caption.set_text(m_lay_table.empty() ? std::string{}
                               : m_lay_kept ? "kept in the solution"
                                            : "the default -- computed from the table until you change it");
    m_layouts_caption.set_tooltip_text(m_lay_kept ? "This layout is kept in the solution (the .acorn file), not the database"
                                                  : "Your first change keeps it in the solution; until then a new field just appears on it");
    m_lay_new.set_sensitive(on);
    m_lay_dup.set_sensitive(on);
    m_lay_rename.set_sensitive(on && m_lay_kept && m_lay.name != core::default_layout_name(m_lay_table));
    m_lay_delete.set_sensitive(on && m_lay_kept);
    m_lay_rename.set_tooltip_text(m_lay_rename.get_sensitive() ? "Give this layout another name"
                                  : "The default keeps its name -- Duplicate it to make one of your own");
    m_lay_delete.set_tooltip_text(m_lay.name == core::default_layout_name(m_lay_table)
                                      ? "Forget your changes to the default -- it is computed from the table again"
                                      : "Delete this layout from the solution -- Ctrl+Z brings it back");

    // The canvas: the body, and room past it to drag into.
    m_lay_paper.set_visible(!m_lay_table.empty());
    m_lay_paper.set_size_request(std::max(1, m_lay.width), std::max(1, m_lay.height));
    m_lay_fixed.set_size_request(m_lay.width + kRoom, m_lay.height + kRoom);
    m_lay_canvas.queue_draw();
    paint_inspector();
}

// ── the canvas ──────────────────────────────────────────────────────────────

void Shell::draw_layout(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {  // layouts: the canvas
    if (m_lay_table.empty()) return;
    GdkRGBA ink{};
    gtk_widget_get_color(GTK_WIDGET(m_lay_canvas.gobj()), &ink);
    auto set = [&](double a) { cr->set_source_rgba(ink.red, ink.green, ink.blue, a); };
    const auto fields = fields_of(m_lay_table);
    const db::Table* t = m_schema.table(m_lay_table);

    // The grid: a dot every 8 px over the body.
    set(0.10);
    for (int y = core::kLayoutGrid; y < std::min(h, m_lay.height); y += core::kLayoutGrid)
        for (int x = core::kLayoutGrid; x < std::min(w, m_lay.width); x += core::kLayoutGrid)
            cr->rectangle(x - 0.5, y - 0.5, 1, 1);
    cr->fill();

    auto text_in = [&](const std::string& s, const core::LayoutObject& o, bool right, double alpha) {
        auto pl = m_lay_canvas.create_pango_layout(s);
        pl->set_width(std::max(0, o.w - 12) * PANGO_SCALE);
        pl->set_ellipsize(Pango::EllipsizeMode::END);
        pl->set_alignment(right ? Pango::Alignment::RIGHT : Pango::Alignment::LEFT);
        int tw = 0, th = 0;
        pl->get_pixel_size(tw, th);
        cr->save();
        cr->rectangle(o.x, o.y, o.w, o.h);
        cr->clip();
        set(alpha);
        cr->move_to(o.x + 6, o.y + (o.h - th) / 2.0);
        pl->show_in_cairo_context(cr);
        cr->restore();
    };

    for (const auto& o : m_lay.objects) {
        const double x = o.x + 0.5, y = o.y + 0.5, ow = o.w - 1, oh = o.h - 1;
        if (o.kind == core::ObjectKind::Text) {
            cr->set_line_width(1);
            set(0.18);
            cr->set_dash(std::vector<double>{2, 3}, 0);
            cr->rectangle(x, y, ow, oh);
            cr->stroke();
            cr->unset_dash();
            text_in(o.text, o, true, 0.75);
        } else if (core::missing_field(o, fields)) {
            cr->set_line_width(1);
            set(0.50);
            cr->set_dash(std::vector<double>{5, 4}, 0);
            cr->rectangle(x, y, ow, oh);
            cr->stroke();
            cr->unset_dash();
            text_in("missing field " + curly(o.field), o, false, 0.55);
        } else {
            set(0.06);
            cr->rectangle(x, y, ow, oh);
            cr->fill();
            set(0.38);
            cr->set_line_width(1);
            cr->rectangle(x, y, ow, oh);
            cr->stroke();
            const db::Column* col = t ? t->column(o.field) : nullptr;
            text_in(o.field + (col ? std::string("  ·  ") + db::to_string(col->type) : std::string{}), o, false, 0.9);
        }
    }

    // The picked object: an amber edge and its eight handles.
    if (m_lay_sel >= 0 && static_cast<std::size_t>(m_lay_sel) < m_lay.objects.size()) {
        const auto& o = m_lay.objects[static_cast<std::size_t>(m_lay_sel)];
        cr->set_source_rgb(kAmber.r, kAmber.g, kAmber.b);
        cr->set_line_width(2);
        cr->rectangle(o.x, o.y, o.w, o.h);
        cr->stroke();
        for (const auto& [g, p] : handles(o)) cr->rectangle(p.first - kHandle / 2, p.second - kHandle / 2, kHandle, kHandle);
        cr->fill();
    }
}

// ── the inspector ───────────────────────────────────────────────────────────

void Shell::paint_inspector() {  // layouts: the picked object's field / words / place
    const unsigned gen = ++m_lay_gen;
    while (auto* c = m_lay_inspector.get_first_child()) m_lay_inspector.remove(*c);
    m_lay_inspector.set_visible(!m_lay_table.empty());
    if (m_lay_table.empty()) return;
    const bool can = static_cast<bool>(m_journal);

    if (m_lay_sel < 0 || static_cast<std::size_t>(m_lay_sel) >= m_lay.objects.size()) {
        auto h = side_label(m_lay.name, "acorn-field-name");
        m_lay_inspector.append(*h);
        m_lay_inspector.append(*side_label(std::to_string(m_lay.width) + " × " + std::to_string(m_lay.height) + " px · " +
                                           std::to_string(m_lay.objects.size()) + " objects", "acorn-status"));
        m_lay_inspector.append(*side_label("Click an object to pick it. Its field, its words and its place show here.",
                                           "acorn-status"));
        return;
    }
    const auto idx = static_cast<std::size_t>(m_lay_sel);
    const core::LayoutObject o = m_lay.objects[idx];
    Gtk::Widget* focus_back = nullptr;
    auto remember = [this, gen](Gtk::Widget& w, const std::string& name) {
        w.set_name(name);
        auto f = Gtk::EventControllerFocus::create();
        f->signal_enter().connect([this, gen, name] { if (gen == m_lay_gen) m_lay_focus = name; });
        w.add_controller(f);
    };

    m_lay_inspector.append(*side_label(o.kind == core::ObjectKind::Field ? "Field box" : "Words", "acorn-field-name"));

    if (o.kind == core::ObjectKind::Field) {
        // The field it shows -- a missing one listed first, so it can be
        // pointed at a real one.
        const auto fields = fields_of(m_lay_table);
        auto names = std::make_shared<std::vector<std::string>>();
        std::vector<Glib::ustring> shown;
        if (core::missing_field(o, fields)) { names->push_back(o.field); shown.push_back(o.field + " (missing)"); }
        for (const auto& f : fields) { names->push_back(f); shown.push_back(f); }
        auto list = Gtk::StringList::create(shown);
        auto dd = Gtk::make_managed<Gtk::DropDown>(list);
        guint at = 0;
        for (std::size_t i = 0; i < names->size(); ++i) if ((*names)[i] == o.field) at = static_cast<guint>(i);
        dd->set_selected(at);
        dd->set_sensitive(can);
        dd->set_tooltip_text("The field this box shows");
        remember(*dd, "layouts.field");
        dd->property_selected().signal_changed().connect([this, dd, names, idx, gen] {
            if (gen != m_lay_gen || idx >= m_lay.objects.size()) return;
            const auto i = dd->get_selected();
            if (i == GTK_INVALID_LIST_POSITION || i >= names->size() || (*names)[i] == m_lay.objects[idx].field) return;
            auto l = m_lay;
            l.objects[idx].field = (*names)[i];
            const std::string what = "Pointed the box at " + curly((*names)[i]);
            Glib::signal_idle().connect_once([this, l, what] { keep_layout(l, what); });
        });
        m_lay_inspector.append(*side_label("Shows", "acorn-status"));
        m_lay_inspector.append(*dd);
        if (m_lay_focus == "layouts.field") focus_back = dd;
    } else {
        auto e = Gtk::make_managed<Gtk::Entry>();
        e->set_text(o.text);
        e->set_sensitive(can);
        e->set_tooltip_text("The words -- Enter (or leaving the box) keeps them");
        remember(*e, "layouts.words");
        auto keep = [this, e, idx, gen] {
            if (gen != m_lay_gen || idx >= m_lay.objects.size()) return;
            const std::string words = e->get_text();
            if (words == m_lay.objects[idx].text) return;
            auto l = m_lay;
            l.objects[idx].text = words;
            const std::string what = "Changed the words to " + curly(words);
            Glib::signal_idle().connect_once([this, l, what] { keep_layout(l, what); });
        };
        e->signal_activate().connect(keep);
        auto leave = Gtk::EventControllerFocus::create();
        leave->signal_leave().connect(keep);
        e->add_controller(leave);
        m_lay_inspector.append(*side_label("Says", "acorn-status"));
        m_lay_inspector.append(*e);
        if (m_lay_focus == "layouts.words") focus_back = e;
    }

    // Its place and size, in pixels.
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    grid->set_margin_top(6);
    const struct { const char* name; int core::LayoutObject::* v; int lo; } parts[] = {
        {"x", &core::LayoutObject::x, 0}, {"y", &core::LayoutObject::y, 0},
        {"w", &core::LayoutObject::w, 1}, {"h", &core::LayoutObject::h, 1}};
    int row = 0;
    for (const auto& p : parts) {
        auto adj = Gtk::Adjustment::create(o.*(p.v), p.lo, 9999, core::kLayoutGrid, 64, 0);
        auto spin = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
        spin->set_numeric(true);
        spin->set_sensitive(can);
        spin->set_hexpand(true);
        const std::string nm = std::string("layouts.") + p.name;
        remember(*spin, nm);
        auto member = p.v;
        const std::string label = p.name;
        spin->signal_value_changed().connect([this, spin, idx, gen, member, label] {
            if (gen != m_lay_gen || idx >= m_lay.objects.size()) return;
            const int v = spin->get_value_as_int();
            if (m_lay.objects[idx].*member == v) return;
            auto l = m_lay;
            l.objects[idx].*member = v;
            const std::string what = (label == "x" || label == "y" ? "Placed " : "Sized ") +
                                     core::object_label(l.objects[idx]);
            Glib::signal_idle().connect_once([this, l, what] { keep_layout(l, what); });
        });
        auto lab = Gtk::make_managed<Gtk::Label>(label == "w" ? "width" : label == "h" ? "height" : label);
        lab->set_xalign(0.0f);
        lab->add_css_class("acorn-status");
        grid->attach(*lab, 0, row);
        grid->attach(*spin, 1, row);
        ++row;
        if (m_lay_focus == nm) focus_back = spin;
    }
    m_lay_inspector.append(*grid);

    auto rm = Gtk::make_managed<Gtk::Button>("Remove from Layout");
    rm->set_name("layouts.remove");
    rm->set_margin_top(10);
    rm->set_sensitive(can);
    rm->set_tooltip_text("Take this off the layout (Delete) -- the field and its values are untouched; Ctrl+Z puts it back");
    rm->signal_clicked().connect([this] { layout_remove_object(); });
    m_lay_inspector.append(*rm);
    if (focus_back) focus_back->grab_focus();
}

// ── the verbs ───────────────────────────────────────────────────────────────

void Shell::keep_layout(core::Layout l, const std::string& did) {  // layouts: verbs::save_layout, the note, a repaint
    if (!m_journal) return;
    core::fit(l);
    const auto before = m_journal->size();
    auto r = db::verbs::save_layout(*m_journal, l);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: layout '{}' of '{}': {} -> {}", l.name, l.table, did, r ? m_journal->undo_label() : r.error().message);
    m_layout_pick[l.table] = l.name;
    paint_layouts();   // from the solution: a refused edit goes back
    // journal_note: every area's note says it (Browse's would otherwise keep
    // the last step's words).
    if (!r) journal_note("Not kept: " + r.error().message + ".", "error");
    else if (m_journal->size() != before) journal_note(did + ". Ctrl+Z puts it back.", "ok");
}

void Shell::layout_nudge(int dx, int dy) {  // layouts: the arrows
    if (!m_journal || m_lay_sel < 0 || static_cast<std::size_t>(m_lay_sel) >= m_lay.objects.size()) return;
    auto l = m_lay;
    auto& o = l.objects[static_cast<std::size_t>(m_lay_sel)];
    o.x = std::max(0, o.x + dx);
    o.y = std::max(0, o.y + dy);
    if (o == m_lay.objects[static_cast<std::size_t>(m_lay_sel)]) return;   // against the edge
    keep_layout(l, "Moved " + core::object_label(o));
    m_lay_canvas.grab_focus();
}

void Shell::layout_remove_object() {  // layouts: Delete
    if (!m_journal || m_lay_sel < 0 || static_cast<std::size_t>(m_lay_sel) >= m_lay.objects.size()) return;
    auto l = m_lay;
    const std::string what = "Removed " + core::object_label(l.objects[static_cast<std::size_t>(m_lay_sel)]);
    l.objects.erase(l.objects.begin() + m_lay_sel);
    m_lay_sel = -1;
    keep_layout(l, what);
    m_lay_canvas.grab_focus();
}

void Shell::on_layout_new() {  // layouts: the default, under a new name
    if (!m_journal || m_lay_table.empty()) return;
    auto l = core::default_layout(m_lay_table, fields_of(m_lay_table));
    l.name = core::unique_layout_name(l.name, layout_names(m_lay_table));
    m_lay_sel = -1;
    keep_layout(l, "Made the layout " + curly(l.name));
}

void Shell::on_layout_duplicate() {  // layouts: the shown layout, under a new name
    if (!m_journal || m_lay_table.empty()) return;
    auto l = m_lay;
    l.name = core::unique_layout_name(m_lay.name + " copy", layout_names(m_lay_table));
    keep_layout(l, "Made " + curly(l.name) + " from " + curly(m_lay.name));
}

void Shell::on_layout_rename() {  // layouts: a popover -> verbs::rename_layout
    if (!m_journal || !m_lay_kept) return;
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);
    auto e = Gtk::make_managed<Gtk::Entry>();
    e->set_name("layouts.rename_entry");
    e->set_text(m_lay.name);
    e->set_width_chars(24);
    auto why = side_label("", "acorn-status");
    why->set_visible(false);
    const std::string table = m_lay_table, from = m_lay.name;
    e->signal_activate().connect([this, e, why, table, from] {
        const std::string to = e->get_text();
        auto r = db::verbs::rename_layout(*m_journal, table, from, to);
        if (auto lg = log::get(log::Area::App))
            lg->info("solution: rename layout '{}' -> '{}': {}", from, to, r ? "kept" : r.error().message);
        if (!r) { why->set_text(r.error().message); why->set_visible(true); return; }
        m_lay_pop.popdown();
        m_layout_pick[table] = to;
        Glib::signal_idle().connect_once([this, from, to] {
            paint_layouts();
            if (from != to) journal_note("Renamed " + curly(from) + " to " + curly(to) + ". Ctrl+Z puts the old name back.", "ok");
        });
    });
    box->append(*side_label("Rename " + curly(from) + " -- Enter keeps it", "acorn-status"));
    box->append(*e);
    box->append(*why);
    m_lay_pop.set_child(*box);
    graphene_rect_t r;
    if (gtk_widget_compute_bounds(GTK_WIDGET(m_lay_rename.gobj()), GTK_WIDGET(m_layouts_page.gobj()), &r)) {
        Gdk::Rectangle at(static_cast<int>(r.origin.x), static_cast<int>(r.origin.y),
                          static_cast<int>(r.size.width), static_cast<int>(r.size.height));
        m_lay_pop.set_pointing_to(at);
    }
    m_lay_pop.popup();
    e->grab_focus();
}

void Shell::on_layout_delete() {  // layouts: verbs::delete_layout
    if (!m_journal || !m_lay_kept) return;
    const std::string table = m_lay_table, name = m_lay.name;
    auto r = db::verbs::delete_layout(*m_journal, table, name);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: delete layout '{}' of '{}': {}", name, table, r ? "gone" : r.error().message);
    m_lay_sel = -1;
    paint_layouts();
    if (!r) journal_note("Not deleted: " + r.error().message + ".", "error");
    else journal_note(name == core::default_layout_name(table)
                          ? "Forgot the changes to " + curly(name) + " -- it is the default again. Ctrl+Z brings them back."
                          : "Deleted the layout " + curly(name) + ". Ctrl+Z brings it back.", "ok");
}

}  // namespace acorn
