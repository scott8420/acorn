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
#include <gtkmm/viewport.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>
#include <utility>

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
// s010e: SEVERAL picked. Shift-click adds an object to the pick or drops it;
// Ctrl+A picks every object in the body. A drag on any picked object moves
// them all, the arrows nudge them all, Delete removes them all -- each ONE
// step. Handles show (and resize) only with one picked.
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

// s010d: the part line near (px, py): 1 the header line (the top edge when
// there's no header), 2 the footer line (the bottom edge when there's no
// footer), 3 the bottom edge under a footer; 0 none.
int line_at(const core::Layout& l, double px, double py) {
    if (px < 0 || px > l.width) return 0;
    auto near = [py](int y) { return std::abs(py - y) <= 4.0; };
    if (l.footer > 0 && near(l.height)) return 3;
    if (near(core::footer_top(l))) return 2;
    if (near(l.header)) return 1;
    return 0;
}

Gtk::Label* side_label(const std::string& s, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    l->set_wrap(true);
    l->set_max_width_chars(24);   // s010f: wrap inside the inspector's 264 px -- a long hint never widens it
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

std::vector<core::PortalLink> Shell::portal_links(const std::string& table) const {  // layouts: s011a -- links a portal can follow
    std::vector<core::PortalLink> out;
    const db::Table* t = m_schema.table(table);
    if (!t) return out;
    for (const auto& in : m_schema.links_to(table))
        if (in.from && in.fk && in.fk->columns.size() == 1 && m_schema.ref_columns(*in.fk).size() == 1)
            out.push_back({in.from->name, in.fk->columns.front()});
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
        m_lay_picks.clear();
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
        m_lay_picks.clear();
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
    // s010d drive: the canvas takes the focus on a press, and a viewport that
    // scrolls to its focus child jumps to the canvas's top mid-drag -- the
    // content slides under the pointer (a footer dragged 40 px came out 16).
    if (auto* vp = dynamic_cast<Gtk::Viewport*>(m_lay_scroll.get_child())) vp->set_scroll_to_focus(false);

    // Press: pick (a handle of the picked object first, then the top-most
    // object under the pointer). Drag: move / resize, live. Let go: one step.
    auto drag = Gtk::GestureDrag::create();
    drag->set_button(GDK_BUTTON_PRIMARY);
    auto* dg = drag.get();
    drag->signal_drag_begin().connect([this, dg](double px, double py) {
        m_lay_canvas.grab_focus();
        m_lay_grab = core::Grab::None;
        m_lay_release = -1;
        m_lay_anchor = -1;
        const int one = lay_one();
        if (one >= 0 && static_cast<std::size_t>(one) < m_lay.objects.size())   // handles: only with one picked
            for (const auto& [g, p] : handles(m_lay.objects[static_cast<std::size_t>(one)]))
                if (std::abs(px - p.first) <= kHit && std::abs(py - p.second) <= kHit) m_lay_grab = g;
        if (m_lay_grab != core::Grab::None) m_lay_anchor = one;
        m_lay_line = 0;
        if (m_lay_grab == core::Grab::None && m_journal && (m_lay_line = line_at(m_lay, px, py)) != 0) {
            m_lay_line_from = m_lay;   // a part line: no object is grabbed
            m_lay_canvas.queue_draw();
            return;
        }
        if (m_lay_grab == core::Grab::None) {
            int hit = -1;
            for (int i = static_cast<int>(m_lay.objects.size()) - 1; i >= 0 && hit < 0; --i) {
                const auto& o = m_lay.objects[static_cast<std::size_t>(i)];
                if (px >= o.x && px < o.x + o.w && py >= o.y && py < o.y + o.h) hit = i;
            }
            const bool shift = (dg->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
            const auto was = m_lay_picks;
            if (hit < 0) {
                if (!shift) m_lay_picks.clear();               // empty canvas: let go (Shift: keep the pick)
            } else if (core::is_picked(m_lay_picks, hit)) {
                // Already picked: a drag moves the whole pick. A press that
                // doesn't move drops it (Shift) or picks only it (plain).
                m_lay_release = shift ? -2 - hit : (m_lay_picks.size() > 1 ? hit : -1);
            } else if (shift) {
                core::toggle_pick(m_lay_picks, hit);            // Shift: joins the pick
            } else {
                m_lay_picks = {hit};
            }
            if (hit >= 0 && core::is_picked(m_lay_picks, hit)) { m_lay_grab = core::Grab::Move; m_lay_anchor = hit; }
            if (m_lay_picks != was) { m_lay_focus.clear(); paint_inspector(); }
        }
        if (m_lay_grab != core::Grab::None) m_lay_drag_from = m_lay;
        m_lay_canvas.queue_draw();
    });
    drag->signal_drag_update().connect([this, dg](double dx, double dy) {
        if (m_lay_line) {   // s010d: a part line, live
            m_lay = m_lay_line_from;
            const int dyi = static_cast<int>(std::lround(dy));
            if (m_lay_line == 1) core::move_part_line(m_lay, core::Part::Header, m_lay_line_from.header + dyi);
            else if (m_lay_line == 2) core::move_part_line(m_lay, core::Part::Footer, core::footer_top(m_lay_line_from) + dyi);
            else core::move_layout_bottom(m_lay, m_lay_line_from.height + dyi);
            m_lay_paper.set_size_request(std::max(1, m_lay.width), std::max(1, m_lay.height));
            m_lay_fixed.set_size_request(m_lay.width + kRoom, m_lay.height + kRoom);
            m_lay_canvas.queue_draw();
            return;
        }
        if (m_lay_grab == core::Grab::None || m_lay_anchor < 0 || !m_journal ||
            static_cast<std::size_t>(m_lay_anchor) >= m_lay_drag_from.objects.size()) return;
        const bool fine = (dg->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
        const int ix = static_cast<int>(std::lround(dx)), iy = static_cast<int>(std::lround(dy));
        const int grid = fine ? 1 : core::kLayoutGrid;
        if (m_lay_grab == core::Grab::Move)   // s010e: every picked object, together
            m_lay = core::moved_objects(m_lay_drag_from, m_lay_picks, m_lay_anchor, ix, iy, grid);
        else
            m_lay.objects[static_cast<std::size_t>(m_lay_anchor)] =
                core::dragged(m_lay_drag_from.objects[static_cast<std::size_t>(m_lay_anchor)], m_lay_grab, ix, iy, grid);
        m_lay_canvas.queue_draw();
    });
    drag->signal_drag_end().connect([this](double, double) {
        if (const int line = std::exchange(m_lay_line, 0); line) {
            if (m_lay == m_lay_line_from) return;
            const std::string what = line == 1 ? (m_lay.header == 0 ? "Removed the header" : m_lay_line_from.header == 0 ? "Made a header" : "Moved the header line")
                                   : (m_lay.footer == 0 ? "Removed the footer" : m_lay_line_from.footer == 0 ? "Made a footer" : "Sized the footer");
            Glib::signal_idle().connect_once([this, what] { keep_layout(m_lay, what); });
            return;
        }
        const core::Grab g = m_lay_grab;
        m_lay_grab = core::Grab::None;
        const int release = std::exchange(m_lay_release, -1);
        if (g == core::Grab::None || m_lay_anchor < 0) return;
        if (m_lay == m_lay_drag_from) {   // a press, no move: only picked
            if (release != -1) {   // s010e: Shift drops it / a plain press picks only it
                if (release <= -2) core::toggle_pick(m_lay_picks, -2 - release);
                else m_lay_picks = {release};
                m_lay_focus.clear();
                paint_inspector();
                m_lay_canvas.queue_draw();
            }
            return;
        }
        const std::string what = (g == core::Grab::Move ? "Moved " : "Resized ") +
                                 (g == core::Grab::Move ? core::picks_label(m_lay, m_lay_picks)
                                                        : core::object_label(m_lay.objects[static_cast<std::size_t>(m_lay_anchor)]));
        // After the gesture has finished with the canvas.
        Glib::signal_idle().connect_once([this, what] { keep_layout(m_lay, what); });
    });
    m_lay_canvas.add_controller(drag);

    // The pointer's shape says what a press would grab.
    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_motion().connect([this](double px, double py) {
        if (m_lay_grab != core::Grab::None) return;
        core::Grab g = core::Grab::None;
        if (const int one = lay_one(); one >= 0 && static_cast<std::size_t>(one) < m_lay.objects.size())
            for (const auto& [h, p] : handles(m_lay.objects[static_cast<std::size_t>(one)]))
                if (std::abs(px - p.first) <= kHit && std::abs(py - p.second) <= kHit) g = h;
        if (g == core::Grab::None && line_at(m_lay, px, py)) {
            gtk_widget_set_cursor_from_name(GTK_WIDGET(m_lay_canvas.gobj()), m_journal ? "ns-resize" : "default");
            return;
        }
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
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::ALT_MASK);
            if (mods == Gdk::ModifierType::CONTROL_MASK && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
                layout_pick_all();   // s010e
                return true;
            }
            if (mods != Gdk::ModifierType{}) return false;
            const int step = (state & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK ? 1 : core::kLayoutGrid;
            switch (keyval) {
                case GDK_KEY_Left:  layout_nudge(-step, 0); return true;
                case GDK_KEY_Right: layout_nudge(step, 0);  return true;
                case GDK_KEY_Up:    layout_nudge(0, -step); return true;
                case GDK_KEY_Down:  layout_nudge(0, step);  return true;
                case GDK_KEY_Delete:
                case GDK_KEY_BackSpace: layout_remove_object(); return true;
                case GDK_KEY_Escape:
                    if (m_lay_picks.empty()) return false;
                    m_lay_picks.clear();
                    m_lay_canvas.queue_draw();
                    paint_inspector();
                    return true;
                default: return false;
            }
        },
        false);
    m_lay_canvas.add_controller(keys);

    m_lay_inspector.set_size_request(264, -1);   // s010f: one width in every state (was 240 + whatever the hints asked)
    m_lay_inspector.set_hexpand(false);
    m_lay_inspector.add_css_class("acorn-panel");
    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    body->set_vexpand(true);
    body->append(m_lay_scroll);
    // s010f: the inspector scrolls up and down. Without that, a window too
    // short for it squeezed its wrapping hints, which then asked for MORE
    // width -- the inspector widened (and the canvas narrowed) whenever it
    // grew a row. Its width now comes from its contents alone.
    auto insp = Gtk::make_managed<Gtk::ScrolledWindow>();
    insp->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    insp->set_propagate_natural_width(true);
    insp->set_has_frame(false);
    insp->set_hexpand(false);
    insp->set_child(m_lay_inspector);
    body->append(*insp);
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
    if (!m_lay_tables || m_lay_grab != core::Grab::None || m_lay_line) return;   // not built yet / never under a drag
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
    std::erase_if(m_lay_picks, [this](int i) { return i < 0 || i >= static_cast<int>(m_lay.objects.size()); });

    const bool on = m_journal && !m_lay_table.empty();
    if (!m_conn) m_layouts_where.set_text("Connect to a database first.");
    else if (m_lay_table.empty()) m_layouts_where.set_text("This database has no tables yet.");
    else {
        const auto n = lnames.size();
        m_layouts_where.set_text(curly(m_lay_table) + " · " + std::to_string(n) + (n == 1 ? " layout" : " layouts") +
                                 " · drag to move, a handle to resize (Shift: by the pixel) · Shift-click picks several · arrows nudge · Delete removes");
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
    const auto links = portal_links(m_lay_table);
    const db::Table* t = m_schema.table(m_lay_table);

    // The grid: a dot every 8 px over the body.
    set(0.10);
    for (int y = core::kLayoutGrid; y < std::min(h, m_lay.height); y += core::kLayoutGrid)
        for (int x = core::kLayoutGrid; x < std::min(w, m_lay.width); x += core::kLayoutGrid)
            cr->rectangle(x - 0.5, y - 0.5, 1, 1);
    cr->fill();

    // s010d: the parts -- the header and footer shaded, a dashed line between
    // each, its name at the right.
    auto band = [&](int y0, int y1, const char* name) {
        set(0.035);
        cr->rectangle(0, y0, m_lay.width, y1 - y0);
        cr->fill();
        auto pl = m_lay_canvas.create_pango_layout(name);
        int tw = 0, th = 0;
        pl->get_pixel_size(tw, th);
        set(0.40);
        cr->move_to(m_lay.width - tw - 8, y0 + 2);
        pl->show_in_cairo_context(cr);
    };
    auto rule = [&](int y) {
        set(0.40);
        cr->set_line_width(1);
        cr->set_dash(std::vector<double>{6, 4}, 0);
        cr->move_to(0, y + 0.5);
        cr->line_to(m_lay.width, y + 0.5);
        cr->stroke();
        cr->unset_dash();
    };
    if (m_lay.header > 0) { band(0, m_lay.header, "header"); rule(m_lay.header); }
    if (m_lay.footer > 0) { band(core::footer_top(m_lay), m_lay.height, "footer"); rule(core::footer_top(m_lay)); }

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
        if (o.kind == core::ObjectKind::Line) {   // s010c: a rule through the middle
            set(0.65);
            cr->set_line_width(2);
            if (o.w >= o.h) { cr->move_to(o.x, o.y + o.h / 2.0); cr->line_to(o.x + o.w, o.y + o.h / 2.0); }
            else            { cr->move_to(o.x + o.w / 2.0, o.y); cr->line_to(o.x + o.w / 2.0, o.y + o.h); }
            cr->stroke();
        } else if (o.kind == core::ObjectKind::Box) {
            set(0.55);
            cr->set_line_width(1.5);
            cr->rectangle(o.x + 0.75, o.y + 0.75, o.w - 1.5, o.h - 1.5);
            cr->stroke();
        } else if (o.kind == core::ObjectKind::Portal) {   // s011a: a list -- the column names, then rows
            const bool gone = core::missing_link(o, links);
            cr->set_line_width(1);
            if (gone) {
                set(0.50);
                cr->set_dash(std::vector<double>{5, 4}, 0);
                cr->rectangle(x, y, ow, oh);
                cr->stroke();
                cr->unset_dash();
                text_in("missing link " + curly(o.from + "." + o.field), o, false, 0.55);
                continue;
            }
            set(0.04);
            cr->rectangle(x, y, ow, oh);
            cr->fill();
            set(0.38);
            cr->rectangle(x, y, ow, oh);
            cr->stroke();
            const int rh = core::kPortalRowH;
            set(0.08);   // the names' row, shaded
            cr->rectangle(x, y, ow, std::min(rh, o.h - 1));
            cr->fill();
            set(0.14);   // row rules
            for (int ry = o.y + rh; ry < o.y + o.h; ry += rh) {
                cr->move_to(o.x + 1, ry + 0.5);
                cr->line_to(o.x + o.w - 1, ry + 0.5);
            }
            cr->stroke();
            const int n = static_cast<int>(o.columns.size());
            const db::Table* ft = m_schema.table(o.from);
            for (int c = 0; c < n; ++c) {   // the columns share the width
                core::LayoutObject cell = o;
                cell.x = o.x + (o.w * c) / n;
                cell.w = o.x + (o.w * (c + 1)) / n - cell.x;
                cell.h = std::min(rh, o.h);
                const bool col_gone = !ft || !ft->column(o.columns[static_cast<std::size_t>(c)]);
                text_in(o.columns[static_cast<std::size_t>(c)] + (col_gone ? " ?" : ""), cell, false, col_gone ? 0.45 : 0.85);
            }
            if (o.h > rh) {   // what it lists, under the names
                core::LayoutObject cap = o;
                cap.y = o.y + rh;
                cap.h = std::min(rh, o.h - rh);
                text_in(o.from + " that point here (by " + o.field + ")", cap, false, 0.45);
            }
        } else if (o.kind == core::ObjectKind::Text) {
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

    // The picked objects: an amber edge each; the eight handles only with one.
    cr->set_source_rgb(kAmber.r, kAmber.g, kAmber.b);
    cr->set_line_width(2);
    for (int i : m_lay_picks) {
        if (i < 0 || static_cast<std::size_t>(i) >= m_lay.objects.size()) continue;
        const auto& o = m_lay.objects[static_cast<std::size_t>(i)];
        cr->rectangle(o.x, o.y, o.w, o.h);
        cr->stroke();
    }
    if (const int one = lay_one(); one >= 0 && static_cast<std::size_t>(one) < m_lay.objects.size()) {
        for (const auto& [g, p] : handles(m_lay.objects[static_cast<std::size_t>(one)]))
            cr->rectangle(p.first - kHandle / 2, p.second - kHandle / 2, kHandle, kHandle);
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

    if (m_lay_picks.size() > 1) {   // s010e: several -- how many, what, and Remove
        m_lay_inspector.append(*side_label(std::to_string(m_lay_picks.size()) + " objects", "acorn-field-name"));
        int f = 0, w = 0, ln = 0, bx = 0, pt = 0;
        for (int i : m_lay_picks) {
            if (i < 0 || static_cast<std::size_t>(i) >= m_lay.objects.size()) continue;
            switch (m_lay.objects[static_cast<std::size_t>(i)].kind) {
                case core::ObjectKind::Field: ++f; break;
                case core::ObjectKind::Text:  ++w; break;
                case core::ObjectKind::Line:  ++ln; break;
                case core::ObjectKind::Box:   ++bx; break;
                case core::ObjectKind::Portal: ++pt; break;
            }
        }
        std::string kinds;
        auto add = [&kinds](int n, const char* one, const char* many) {
            if (n == 0) return;
            if (!kinds.empty()) kinds += ", ";
            kinds += std::to_string(n) + " " + (n == 1 ? one : many);
        };
        add(f, "field box", "field boxes");
        add(w, "set of words", "sets of words");
        add(ln, "line", "lines");
        add(bx, "box", "boxes");
        add(pt, "portal", "portals");
        m_lay_inspector.append(*side_label(kinds, "acorn-status"));
        m_lay_inspector.append(*side_label(
            "Drag any of them to move them all; the arrows nudge them all; Delete removes them all -- one step each. "
            "Shift-click one to drop it from the pick. Place and size show for one object.", "acorn-status"));
        auto rm = Gtk::make_managed<Gtk::Button>("Remove " + std::to_string(m_lay_picks.size()) + " from Layout");
        rm->set_name("layouts.remove");
        rm->set_margin_top(10);
        rm->set_sensitive(can);
        rm->set_tooltip_text("Take these off the layout (Delete) -- fields and their values are untouched; Ctrl+Z puts them back");
        rm->signal_clicked().connect([this, gen] { if (gen == m_lay_gen) layout_remove_object(); });
        m_lay_inspector.append(*rm);
        return;
    }
    if (lay_one() < 0 || static_cast<std::size_t>(lay_one()) >= m_lay.objects.size()) {
        auto h = side_label(m_lay.name, "acorn-field-name");
        m_lay_inspector.append(*h);
        m_lay_inspector.append(*side_label(std::to_string(m_lay.width) + " × " + std::to_string(m_lay.height) + " px · " +
                                           std::to_string(m_lay.objects.size()) + " objects", "acorn-status"));
        m_lay_inspector.append(*side_label("Click an object to pick it -- Shift-click for several, Ctrl+A for the whole body. "
                                           "Its field, its words and its place show here.",
                                           "acorn-status"));
        // s010d: the parts, and how to change them.
        m_lay_inspector.append(*side_label(
            "Parts: header " + std::to_string(m_lay.header) + " · body " +
            std::to_string(m_lay.height - m_lay.header - m_lay.footer) + " · footer " + std::to_string(m_lay.footer) +
            " px. Drag the top edge down for a header, the bottom edge for a footer; drag a line back to remove an empty one. "
            "The Form shows the body.", "acorn-status"));
        // s010c: Add -- each lands under everything, picked; one step.
        auto add_head = side_label("Add", "acorn-field-name");
        add_head->set_margin_top(10);
        m_lay_inspector.append(*add_head);
        const auto missing = core::fields_not_on(m_lay, fields_of(m_lay_table));
        auto names = std::make_shared<std::vector<std::string>>(missing);
        std::vector<Glib::ustring> shown(missing.begin(), missing.end());
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(shown));
        dd->set_name("layouts.add_pick");
        dd->set_tooltip_text("A field this layout doesn't show yet");
        dd->set_sensitive(can && !missing.empty());
        auto addf = Gtk::make_managed<Gtk::Button>(missing.empty() ? "Every field is on it" : "Add Field");
        addf->set_name("layouts.add_field");
        addf->set_sensitive(can && !missing.empty());
        addf->set_tooltip_text("The field's name on the left and its box on the right, under everything");
        addf->signal_clicked().connect([this, dd, names, gen] {
            if (gen != m_lay_gen) return;
            const auto i = dd->get_selected();
            if (i == GTK_INVALID_LIST_POSITION || i >= names->size()) return;
            auto l = m_lay;
            const int n = core::add_object(l, core::ObjectKind::Field, (*names)[i]);
            m_lay_picks.clear();   // s010e: the row picked -- its words and its box, to move together
            for (int k = static_cast<int>(l.objects.size()) - n; k < static_cast<int>(l.objects.size()); ++k) m_lay_picks.push_back(k);
            const std::string what = "Added " + curly((*names)[i]);
            // The canvas takes the keys: the arrows / Esc / Delete act on what was added.
            Glib::signal_idle().connect_once([this, l, what] { m_lay_focus.clear(); keep_layout(l, what); m_lay_canvas.grab_focus(); });
        });
        if (!missing.empty()) m_lay_inspector.append(*dd);
        m_lay_inspector.append(*addf);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        row->set_homogeneous(true);
        const struct { const char* label; const char* name; core::ObjectKind kind; const char* tip; } adds[] = {
            {"Words", "layouts.add_words", core::ObjectKind::Text, "Words of your own -- change them in the inspector"},
            {"Line", "layouts.add_line", core::ObjectKind::Line, "A rule -- across when wider than tall, else down"},
            {"Box", "layouts.add_box", core::ObjectKind::Box, "A rectangle, drawn behind the fields"}};
        for (const auto& a : adds) {
            auto b = Gtk::make_managed<Gtk::Button>(a.label);
            b->set_name(a.name);
            b->set_sensitive(can);
            b->set_tooltip_text(a.tip);
            const auto kind = a.kind;
            b->signal_clicked().connect([this, kind, gen] {
                if (gen != m_lay_gen) return;
                auto l = m_lay;
                core::add_object(l, kind, kind == core::ObjectKind::Text ? "Words" : "");
                m_lay_picks = {static_cast<int>(l.objects.size()) - 1};
                const std::string what = "Added " + core::object_label(l.objects.back());
                Glib::signal_idle().connect_once([this, l, what] { m_lay_focus.clear(); keep_layout(l, what); m_lay_canvas.grab_focus(); });
            });
            row->append(*b);
        }
        m_lay_inspector.append(*row);
        // s011a: a portal -- offered only when something links INTO this table.
        if (const auto links = portal_links(m_lay_table); !links.empty()) {
            auto lk = std::make_shared<std::vector<core::PortalLink>>(links);
            std::vector<Glib::ustring> lshown;
            for (const auto& k : links) lshown.push_back(core::link_label(k));
            auto pdd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(lshown));
            pdd->set_name("layouts.add_portal_pick");
            pdd->set_tooltip_text("A table that points at “" + m_lay_table + "” -- the portal lists its records that point at the shown one");
            pdd->set_sensitive(can);
            pdd->set_margin_top(6);
            auto addp = Gtk::make_managed<Gtk::Button>("Add Portal");
            addp->set_name("layouts.add_portal");
            addp->set_sensitive(can);
            addp->set_tooltip_text("A list of the related records (a contact's phones), under everything");
            addp->signal_clicked().connect([this, pdd, lk, gen] {
                if (gen != m_lay_gen) return;
                const auto i = pdd->get_selected();
                if (i == GTK_INVALID_LIST_POSITION || i >= lk->size()) return;
                const auto& k = (*lk)[i];
                std::string counting;
                if (const db::Table* ft = m_schema.table(k.from))
                    for (const auto& c : ft->columns) if (c.auto_increment && c.pk_position > 0) counting = c.name;
                auto l = m_lay;
                core::add_portal(l, k, core::portal_columns(fields_of(k.from), k.field, counting));
                m_lay_picks = {static_cast<int>(l.objects.size()) - 1};
                const std::string what = "Added " + core::object_label(l.objects.back());
                Glib::signal_idle().connect_once([this, l, what] { m_lay_focus.clear(); keep_layout(l, what); m_lay_canvas.grab_focus(); });
            });
            m_lay_inspector.append(*pdd);
            m_lay_inspector.append(*addp);
        }
        return;
    }
    const auto idx = static_cast<std::size_t>(lay_one());
    const core::LayoutObject o = m_lay.objects[idx];
    Gtk::Widget* focus_back = nullptr;
    auto remember = [this, gen](Gtk::Widget& w, const std::string& name) {
        w.set_name(name);
        auto f = Gtk::EventControllerFocus::create();
        f->signal_enter().connect([this, gen, name] { if (gen == m_lay_gen) m_lay_focus = name; });
        w.add_controller(f);
    };

    m_lay_inspector.append(*side_label(o.kind == core::ObjectKind::Field ? "Field box"
                                       : o.kind == core::ObjectKind::Text ? "Words"
                                       : o.kind == core::ObjectKind::Line ? "Line"
                                       : o.kind == core::ObjectKind::Portal ? "Portal" : "Box", "acorn-field-name"));

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
    } else if (o.kind == core::ObjectKind::Portal) {   // s011a: what it lists (columns change in s011b+)
        if (core::missing_link(o, portal_links(m_lay_table)))
            m_lay_inspector.append(*side_label("Its link is gone: " + curly(o.from + "." + o.field) + " no longer points at " +
                                               curly(m_lay_table) + ". Nothing is lost -- link them again in the Graph, "
                                               "or remove the portal.", "acorn-mark"));
        else
            m_lay_inspector.append(*side_label("Lists the " + curly(o.from) + " records whose " + curly(o.field) +
                                               " points at the shown " + curly(m_lay_table) + " record.", "acorn-status"));
        std::string cols;
        for (const auto& c : o.columns) cols += (cols.empty() ? "" : ", ") + c;
        m_lay_inspector.append(*side_label("Columns: " + (cols.empty() ? std::string("none") : cols), "acorn-status"));
    } else if (o.kind == core::ObjectKind::Text) {
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
    const bool could_redo = m_journal->can_redo();   // s010e: a step after an undo drops the redo -- the count may not change
    auto r = db::verbs::save_layout(*m_journal, l);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: layout '{}' of '{}': {} -> {}", l.name, l.table, did, r ? m_journal->undo_label() : r.error().message);
    m_layout_pick[l.table] = l.name;
    paint_layouts();   // from the solution: a refused edit goes back
    // journal_note: every area's note says it (Browse's would otherwise keep
    // the last step's words).
    if (!r) journal_note("Not kept: " + r.error().message + ".", "error");
    else if (m_journal->size() != before || could_redo != m_journal->can_redo()) journal_note(did + ". Ctrl+Z puts it back.", "ok");
}

void Shell::layout_nudge(int dx, int dy) {  // layouts: the arrows -- every picked object, one step
    if (!m_journal || m_lay_picks.empty()) return;
    const auto l = core::moved_objects(m_lay, m_lay_picks, m_lay_picks.front(), dx, dy, 1);
    if (l == m_lay) return;   // against the edge
    keep_layout(l, "Moved " + core::picks_label(l, m_lay_picks));
    m_lay_canvas.grab_focus();
}

void Shell::layout_remove_object() {  // layouts: Delete -- every picked object, one step
    if (!m_journal || m_lay_picks.empty()) return;
    auto l = m_lay;
    const std::string what = "Removed " + core::picks_label(l, m_lay_picks);
    if (core::remove_objects(l, m_lay_picks) == 0) return;
    m_lay_picks.clear();
    keep_layout(l, what);
    m_lay_canvas.grab_focus();
}

void Shell::layout_pick_all() {  // layouts: s010e -- Ctrl+A: every object in the body
    if (m_lay_table.empty()) return;
    auto all = core::body_objects(m_lay);
    if (all == m_lay_picks) return;
    m_lay_picks = std::move(all);
    m_lay_focus.clear();
    m_lay_canvas.queue_draw();
    paint_inspector();
}

void Shell::on_layout_new() {  // layouts: the default, under a new name
    if (!m_journal || m_lay_table.empty()) return;
    auto l = core::default_layout(m_lay_table, fields_of(m_lay_table));
    l.name = core::unique_layout_name(l.name, layout_names(m_lay_table));
    m_lay_picks.clear();
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
    m_lay_picks.clear();
    paint_layouts();
    if (!r) journal_note("Not deleted: " + r.error().message + ".", "error");
    else journal_note(name == core::default_layout_name(table)
                          ? "Forgot the changes to " + curly(name) + " -- it is the default again. Ctrl+Z brings them back."
                          : "Deleted the layout " + curly(name) + ". Ctrl+Z brings it back.", "ok");
}

}  // namespace acorn
