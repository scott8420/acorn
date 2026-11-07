#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto -- same singleton shape, same dtor requirement
#include "Log.hpp"

// Shell.cpp -- GLUE. The ctor and the ordered build_ui() pass live here; every
// other method body lives in a category file (see the index in Shell.hpp).

namespace acorn {

Shell::Shell()
    : m_logo_button("shell.logo_button"),
      m_menu_button("shell.menu_button"),
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_sidebar("shell.sidebar"),
      m_stack("shell.stack"),
      m_recents_page("shell.recents_page", Gtk::Orientation::VERTICAL),
      m_recents_scroll("shell.recents_scroll"),
      m_recents_list("shell.recents_list"),
      m_undo_page("shell.undo.page", Gtk::Orientation::VERTICAL, 8),
      m_stone_list("shell.undo.list"),
      m_stone_name("shell.undo.name"),
      m_stone_size("shell.undo.size", Gtk::Orientation::HORIZONTAL),
      m_undo_button("shell.undo.undo"),
      m_redo_button("shell.undo.redo"),
      m_undo_said("shell.undo.said"),
      m_connect_page("shell.connect.page", Gtk::Orientation::VERTICAL, 14),
      m_file_rows("shell.connect.file_rows", Gtk::Orientation::VERTICAL, 6),
      m_server_rows("shell.connect.server_rows", Gtk::Orientation::VERTICAL, 6),
      m_path("shell.connect.path"),
      m_host("shell.connect.host"),
      m_port("shell.connect.port"),
      m_database("shell.connect.database"),
      m_user("shell.connect.user"),
      m_password("shell.connect.password"),
      m_browse("shell.connect.browse"),
      m_connect("shell.connect.connect"),
      m_status("shell.connect.status"),
      m_tables("shell.connect.tables") {
    set_name("shell.window");
}

Shell::~Shell() {
    // A connect in flight finishes on its own (it has a timeout); wait for it
    // so no thread outlives the loader whose plugin code it is running.
    if (m_worker.joinable()) m_worker.join();
}

// The ordered pass Scott's Controller/MainWindow shape names: construct the
// shell, build the pages, bind actions + accelerators, load persisted state,
// paint. Each step is one call into a category file.
void Shell::build_ui() {
    build_shell();     // zone
    build_pages();     // zone
    bind_actions();    // bindings

    // Load persisted recents (the UI resolves the XDG path -- the seam -- and
    // hands the core a plain string), then paint both surfaces.
    m_recents_file = recents_file();
    // m_recents filled by note_recent / load; load happens in bind_actions after
    // the action group exists so a click can route. Paint whatever's there.
    rebuild_recents_menu();
    rebuild_recents_page();

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} widget(s) registered", 0);  // dump-registry shows the real count
}

}  // namespace acorn
