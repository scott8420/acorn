#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto -- same singleton shape, same dtor requirement
#include "DesignTableWindow.hpp"   // s007b
#include "NewTableWindow.hpp"   // ditto (s005b)
#include "ImportWindow.hpp"     // ditto (s018b)
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
      m_browse_list_btn("shell.browse.list"),
      m_browse_preview_btn("shell.browse.preview"),
      m_browse_prev("shell.browse.prev"),
      m_browse_next("shell.browse.next"),
      m_browse_range("shell.browse.range"),
      m_browse_scroll("shell.browse.scroll"),
      m_browse_note("shell.browse.note"),
      m_browse_new("shell.browse.new_record"),
      m_browse_delete("shell.browse.delete_record"),
      m_browse_find("shell.browse.find_btn"),
      m_browse_show_all("shell.browse.show_all"),
      m_browse_scripts("shell.browse.scripts"),
      m_browse_export("shell.browse.export"),
      m_browse_import("shell.browse.import"),
      m_record_scroll("shell.browse.record"),
      m_accounts_page("shell.accounts.page", Gtk::Orientation::VERTICAL, 12),
      m_accounts_where("shell.accounts.where"),
      m_accounts_scroll("shell.accounts.scroll"),
      m_accounts_note("shell.accounts.note"),
      m_accounts_new("shell.accounts.new"),
      m_accounts_editor("shell.accounts.editor", Gtk::Orientation::VERTICAL, 6),
      m_acct_name("shell.accounts.new_name"),
      m_acct_host("shell.accounts.new_host"),
      m_acct_pw("shell.accounts.new_password"),
      m_acct_pw2("shell.accounts.new_password_again"),
      m_acct_sql("shell.accounts.new_sql"),
      m_acct_say("shell.accounts.new_says"),
      m_acct_make("shell.accounts.new_make"),
      m_acct_cancel("shell.accounts.new_cancel"),
      m_pw_editor("shell.accounts.password_editor", Gtk::Orientation::VERTICAL, 6),   // s021
      m_pw_title("shell.accounts.password_title"),
      m_pw_sql("shell.accounts.password_sql"),
      m_pw_say("shell.accounts.password_says"),
      m_pw1("shell.accounts.password_new"),
      m_pw2("shell.accounts.password_again"),
      m_pw_set("shell.accounts.password_set"),
      m_pw_cancel("shell.accounts.password_cancel"),
      m_graph_page("shell.graph.page", Gtk::Orientation::VERTICAL, 12),
      m_graph_where("shell.graph.where"),
      m_graph_note("shell.graph.note"),
      m_graph_scroll("shell.graph.scroll"),
      m_layouts_page("shell.layouts.page", Gtk::Orientation::VERTICAL, 12),
      m_layouts_where("shell.layouts.where"),
      m_layouts_note("shell.layouts.note"),
      m_layouts_caption("shell.layouts.caption"),
      m_lay_new("shell.layouts.new"),
      m_lay_dup("shell.layouts.duplicate"),
      m_lay_rename("shell.layouts.rename"),
      m_lay_delete("shell.layouts.delete"),
      m_lay_scroll("shell.layouts.scroll"),
      m_lay_inspector("shell.layouts.inspector", Gtk::Orientation::VERTICAL, 8) {
    set_name("shell.window");
}

Shell::~Shell() {
    m_graph_pop.unparent();   // s008b: parented by hand, so let go by hand
    m_lay_pop.unparent();     // s010b: the same
    *m_alive = false;   // keyring callbacks still in flight become no-ops
    // A connect in flight finishes on its own (it has a timeout); wait for it
    // so no thread outlives the loader whose plugin code it is running.
    if (m_worker.joinable()) m_worker.join();
    // s020b: a long read is asked to stop and waited for (its work holds m_conn).
    if (m_long_ticket) m_long_ticket->stop = true;
    if (m_long.joinable()) m_long.join();
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
    bind_startup();    // s010f: the sample Address Book (opened once the window is up, when on)

    // The recents list was loaded in bind_actions (after its action group
    // exists, so a click can route); paint both surfaces.
    rebuild_recents_menu();
    rebuild_recents_list();

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} widget(s) registered", 0);  // dump-registry shows the real count
}

}  // namespace acorn
