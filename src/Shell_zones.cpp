#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the window frame
// and header, the sidebar+stack body, the areas, and the menu model.

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

void Shell::build_pages() {  // zone: Acorn's areas
    build_connect_page();
    build_tables_page();    // s004c
    build_browse_page();    // s006b
    build_graph_page();     // s008b

    // Per-area tint hooks (REQUIREMENTS §2): the window carries .page-<area>
    // for whichever area is showing.
    m_stack.property_visible_child_name().signal_changed().connect([this] {
        for (const char* a : {"page-connect", "page-tables", "page-browse", "page-graph"}) remove_css_class(a);
        add_css_class("page-" + std::string(m_stack.get_visible_child_name()));
        // s008b: the graph is painted fresh when it comes into view (marks
        // and records may have changed in Browse since).
        if (m_stack.get_visible_child_name() == "graph") paint_graph();
    });
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto file = Gio::Menu::create();
    file->append("New Database File\u2026", "win.new-file");
    file->append("Open a Database File\u2026", "win.open-file");
    m_recents_menu = Gio::Menu::create();
    file->append_submenu("Open Recent", m_recents_menu);
    menu->append_section(file);

    // s005b: Undo / Redo name the step ("Undo New Table “notes”"); the
    // section is refilled by sync_undo whenever the journal changes.
    m_edit_menu = Gio::Menu::create();
    menu->append_section(m_edit_menu);

    auto diag = Gio::Menu::create();
    diag->append("Dump registry", "win.dump-registry");
    menu->append_section(diag);

    // s005c: the ground colour -- GNOME's accent presets (a radio on
    // win.ground) and Custom..., which opens the colour picker.
    auto ground = Gio::Menu::create();
    auto presets = Gio::Menu::create();
    for (const auto& p : core::ground_presets()) {
        auto item = Gio::MenuItem::create(p.name, "");
        item->set_action_and_target("win.ground", Glib::Variant<Glib::ustring>::create(p.id));
        presets->append_item(item);
    }
    ground->append_section(presets);
    auto custom = Gio::MenuItem::create("Custom\u2026", "");
    custom->set_action_and_target("win.ground", Glib::Variant<Glib::ustring>::create("custom"));
    ground->append_item(custom);

    auto app = Gio::Menu::create();
    app->append_submenu("Base Color", ground);
    app->append("About Acorn", "win.about");
    app->append("Quit", "win.quit");
    menu->append_section(app);
    return menu;
}

}  // namespace acorn
