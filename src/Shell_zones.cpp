#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/eventcontrollerfocus.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the window frame
// and header, the sidebar+stack body, the pattern pages, and the menu model.

namespace acorn {

void Shell::build_shell() {  // zone: window + sidebar + stack
    set_title("Acorn");
    set_default_size(960, 640);
    add_css_class("acorn");          // s004: the skin styles window.acorn only
    add_css_class("page-connect");

    auto header = Gtk::make_managed<Gtk::HeaderBar>();

    // Brand logo at the start: a frameless button whose icon is a symbolic
    // resource resolved from the compiled-in bundle (App registers it), so this
    // one button exercises the resource pipeline. Click opens the About window
    // (the dialog-lifetime exemplar).
    m_logo_button.set_icon_name("acorn-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.add_css_class("acorn-logo");
    m_logo_button.set_tooltip_text("About Acorn");
    m_logo_button.set_action_name("win.about");
    header->pack_start(m_logo_button);

    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    header->pack_end(m_menu_button);
    set_titlebar(*header);

    // StackSidebar drives the Stack: adding a page with a title makes a sidebar
    // row automatically. That IS the Stack/StackSidebar paradigm -- no manual
    // row wiring.
    m_sidebar.set_stack(m_stack);
    m_sidebar.set_size_request(160, -1);
    m_stack.set_hexpand(true);
    m_stack.set_vexpand(true);

    m_body.append(m_sidebar);
    m_body.append(m_stack);
    set_child(m_body);
}

void Shell::build_pages() {  // zone: the pattern pages
    // s004: Acorn's own areas. Cairn's demo pages (Recent menu, Registry,
    // Undo) are no longer on the sidebar; their code stays until the real
    // recents (s004b) and the change journal's UI (s005) replace it.
    build_connect_page();
}

// Cairn's demo pages, kept buildable but not called (see build_pages).
void Shell::build_demo_pages() {  // zone: Cairn's retired demo pages
    // Page 1 -- Recents: a live list + explanation. The menu's Open-recent
    // submenu and this list are the same data, two surfaces (CANON: two
    // surfaces, one mechanism).
    auto intro = Gtk::make_managed<Gtk::Label>();
    intro->set_wrap(true);
    intro->set_xalign(0.0f);
    intro->set_margin(12);
    intro->set_markup(
        "<b>Recent menu</b> -- actions + a JSON pump, composed.\n\n"
        "Use the menu (top-right) -> \"Remember a folder...\" to add one. It "
        "appears here and under \"Open recent\", persists to a JSON file in the "
        "XDG data dir, and prunes entries whose folder has since vanished.\n\n"
        "The list is core data; the UI resolves the path and drives the menu. "
        "See docs/two-layer-seam.md and core/Recents.hpp.");
    m_recents_page.append(*intro);

    m_recents_list.set_selection_mode(Gtk::SelectionMode::NONE);
    m_recents_scroll.set_child(m_recents_list);
    m_recents_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_recents_scroll.set_vexpand(true);
    m_recents_page.append(m_recents_scroll);
    m_stack.add(m_recents_page, "recents", "Recent menu");

    // Page 2 -- Registry: the debuggability payoff. The button dumps the live
    // name->widget tree to the log (also callable from gdb as registry::dump()).
    auto reg = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    reg->set_margin(12);
    auto reg_intro = Gtk::make_managed<Gtk::Label>();
    reg_intro->set_wrap(true);
    reg_intro->set_xalign(0.0f);
    reg_intro->set_markup(
        "<b>Registry</b> -- the live address book Named&lt;W&gt; fills.\n\n"
        "Every widget here was named at construction and registered by "
        "Named&lt;W&gt;. \"Dump\" prints the live tree -- the same call is your "
        "friend in gdb when a backtrace frame is otherwise a bare pointer. See "
        "docs/widget-naming.md for why this is the LIGHT half of naming.");
    auto dump_btn = Gtk::make_managed<Gtk::Button>("Dump registry to the log");
    dump_btn->set_halign(Gtk::Align::START);
    dump_btn->set_action_name("win.dump-registry");
    reg->append(*reg_intro);
    reg->append(*dump_btn);
    m_stack.add(*reg, "registry", "Registry");

    build_undo_page();   // s046
}

// ─────────────────────────────────────────────────────────────────────────────
// The Undo page (s046) -- the undo stone, exercised. A list of stones; every
// button, the name field and the size slider are user choices, and every one
// is a step (core/Undo.hpp, docs/undo.md). Try: delete a stone and press
// Ctrl+Z without clicking anything; drag the slider and undo once; tap the
// arrow keys on the slider and undo once.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_undo_page() {  // zone: the Undo page
    m_undo_page.set_margin(12);

    auto intro = Gtk::make_managed<Gtk::Label>();
    intro->set_wrap(true);
    intro->set_xalign(0.0f);
    intro->set_markup(
        "<b>Undo</b> -- every user choice is a step, through one door.\n\n"
        "Add, delete, move, rename, resize: <b>Ctrl+Z</b> takes any of them back, "
        "from anywhere but a text box (the name field keeps its own). A slider "
        "<i>drag</i> is one step; arrow presses within 1.5 s merge into one. See "
        "core/Undo.hpp and docs/undo.md.");
    m_undo_page.append(*intro);

    // Verbs.
    auto verbs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    const auto verb = [&](const char* label, const char* action, const char* name) {
        auto b = Gtk::make_managed<Gtk::Button>(label);
        b->set_name(name);
        b->set_action_name(action);
        verbs->append(*b);
    };
    verb("Add", "win.stone-add", "shell.undo.add");
    verb("Delete", "win.stone-delete", "shell.undo.delete");
    verb("Up", "win.stone-up", "shell.undo.up");
    verb("Down", "win.stone-down", "shell.undo.down");
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    verbs->append(*spacer);
    m_undo_button.set_label("Undo");
    m_undo_button.set_action_name("win.undo");
    m_redo_button.set_label("Redo");
    m_redo_button.set_action_name("win.redo");
    verbs->append(m_undo_button);
    verbs->append(m_redo_button);
    m_undo_page.append(*verbs);

    // The list.
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    scroll->set_child(m_stone_list);
    m_stone_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_stone_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (m_painting) return;
        m_sel = row ? std::string(row->get_name()) : std::string{};
        repaint_stones();   // the detail fields follow the selection
    });
    m_undo_page.append(*scroll);

    // Detail: name + size.
    auto detail = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto nl = Gtk::make_managed<Gtk::Label>("Name");
    m_stone_name.set_hexpand(true);
    m_stone_name.set_valign(Gtk::Align::CENTER);
    m_stone_name.signal_activate().connect(sigc::mem_fun(*this, &Shell::on_stone_rename));
    auto leave = Gtk::EventControllerFocus::create();
    leave->signal_leave().connect(sigc::mem_fun(*this, &Shell::on_stone_rename));
    m_stone_name.add_controller(leave);
    auto sl = Gtk::make_managed<Gtk::Label>("Size");
    m_stone_size.set_range(1, 10);
    m_stone_size.set_increments(1, 1);
    m_stone_size.set_digits(0);
    m_stone_size.set_round_digits(0);
    m_stone_size.set_draw_value(true);
    m_stone_size.set_size_request(180, -1);
    m_stone_size.set_valign(Gtk::Align::CENTER);
    m_stone_size.signal_value_changed().connect(sigc::mem_fun(*this, &Shell::on_stone_size));
    // A drag is ONE step: open the gesture at press, close it at release.
    // CAPTURE, so it sees the press before the slider's own gesture claims
    // it; a claimed sequence arrives here as cancel -- also the end.
    auto drag = Gtk::GestureDrag::create();
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->signal_drag_begin().connect([this](double, double) {
        if (!m_sel.empty()) core::stones::drag_begin(m_journal, m_stones, m_sel);
    });
    const auto end = [this]() { core::stones::drag_end(m_journal); };
    drag->signal_drag_end().connect([end](double, double) { end(); });
    drag->signal_cancel().connect([end](const Gdk::EventSequence*) { end(); });
    m_stone_size.add_controller(drag);
    detail->append(*nl);
    detail->append(m_stone_name);
    detail->append(*sl);
    detail->append(m_stone_size);
    m_undo_page.append(*detail);

    m_undo_said.set_xalign(0.0f);
    m_undo_said.add_css_class("dim-label");
    m_undo_page.append(m_undo_said);

    m_stack.add(m_undo_page, "undo", "Undo");
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto file = Gio::Menu::create();
    file->append("Remember a folder\u2026", "win.pick-folder");
    m_recents_menu = Gio::Menu::create();
    file->append_submenu("Open recent", m_recents_menu);
    menu->append_section(file);

    // s046: Undo / Redo, re-labelled live ("Undo Delete “Jade”") and greyed
    // when there is nothing to do -- update_undo_ui() owns this section.
    m_edit_menu = Gio::Menu::create();
    m_edit_menu->append("Undo", "win.undo");
    m_edit_menu->append("Redo", "win.redo");
    menu->append_section(m_edit_menu);

    auto diag = Gio::Menu::create();
    diag->append("Dump registry", "win.dump-registry");
    menu->append_section(diag);

    auto app = Gio::Menu::create();
    app->append("About Acorn", "win.about");
    app->append("Quit", "win.quit");
    menu->append_section(app);
    return menu;
}

}  // namespace acorn
