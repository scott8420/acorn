#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto -- same singleton shape, same dtor requirement
#include "DesignTableWindow.hpp"   // s007b
#include "NewTableWindow.hpp"   // ditto (s005b)
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
      m_recent_list("shell.connect.recents"),
      m_connect_page("shell.connect.page", Gtk::Orientation::VERTICAL, 14),
      m_file_rows("shell.connect.file_rows", Gtk::Orientation::VERTICAL, 6),
      m_server_rows("shell.connect.server_rows", Gtk::Orientation::VERTICAL, 6),
      m_path("shell.connect.path"),
      m_host("shell.connect.host"),
      m_port("shell.connect.port"),
      m_database("shell.connect.database"),
      m_user("shell.connect.user"),
      m_password("shell.connect.password"),
      m_remember("shell.connect.remember"),
      m_browse("shell.connect.browse"),
      m_new_file("shell.connect.new_file"),
      m_connect("shell.connect.connect"),
      m_status("shell.connect.status"),
      m_tables("shell.connect.tables"),
      m_tables_page("shell.tables.page", Gtk::Orientation::VERTICAL, 12),
      m_table_list("shell.tables.list"),
      m_table_detail("shell.tables.detail", Gtk::Orientation::VERTICAL, 10),
      m_tables_where("shell.tables.where"),
      m_new_table_btn("shell.tables.new_table"),
      m_journal_note("shell.tables.journal_note"),
      m_browse_page("shell.browse.page", Gtk::Orientation::VERTICAL, 12),
      m_browse_where("shell.browse.where"),
      m_browse_cards_btn("shell.browse.cards"),
      m_browse_grid_btn("shell.browse.grid"),
      m_browse_form_btn("shell.browse.form"),
      m_browse_prev("shell.browse.prev"),
      m_browse_next("shell.browse.next"),
      m_browse_range("shell.browse.range"),
      m_browse_scroll("shell.browse.scroll"),
      m_browse_note("shell.browse.note"),
      m_browse_new("shell.browse.new_record"),
      m_browse_delete("shell.browse.delete_record"),
      m_record_scroll("shell.browse.record"),
      m_graph_page("shell.graph.page", Gtk::Orientation::VERTICAL, 12),
      m_graph_where("shell.graph.where"),
      m_graph_note("shell.graph.note"),
      m_graph_scroll("shell.graph.scroll") {
    set_name("shell.window");
}

Shell::~Shell() {
    m_graph_pop.unparent();   // s008b: parented by hand, so let go by hand
    *m_alive = false;   // keyring callbacks still in flight become no-ops
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
    bind_journal();    // s005b: undo / redo / new table + the window's Ctrl+Z
    bind_ground();     // s005c: the skin's base colour, as saved

    // The recents list was loaded in bind_actions (after its action group
    // exists, so a click can route); paint both surfaces.
    rebuild_recents_menu();
    rebuild_recents_list();

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} widget(s) registered", 0);  // dump-registry shows the real count
}

}  // namespace acorn
