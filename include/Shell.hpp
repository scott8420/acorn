#pragma once
#include "widgets/Widgets.hpp"
#include "core/Connections.hpp"  // s004b: recent connections
#include "Keyring.hpp"             // s004b: passwords in the GNOME keyring
#include "core/Ground.hpp"         // s005c: the skin's base colour
#include <gtkmm/cssprovider.h>
#include "db/Loader.hpp"     // s004: engine plugins
#include "db/Schema.hpp"
#include "db/Journal.hpp"   // s005b: the door

#include <glibmm/dispatcher.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the app window, AND the split-class exemplar.
//
// This class is split across multiple TUs by CATEGORY OF CODE, not by feature
// (CANON: "vocabulary first, file layout second" + "the header is the index").
// The split costs ~nothing while the parts are small; retrofitting it after a
// class grows to thousands of lines is a multi-session migration, so the
// structure is installed on day one. The header below IS the index -- every
// method carries a trailing `// category: ...` tag naming the file its body
// lives in. Read the tag, open that file.
//
// Vocabulary
// ----------
//   Zones    -- construction of named regions: the window frame, header,
//               sidebar+stack, the pattern pages, the menu model.
//               File: src/Shell_zones.cpp
//   Bindings -- what's wired to what: Gio actions, the recents action group,
//               accelerators.  File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*): what a user action actually does.
//               File: src/Shell_handlers.cpp
//   Helpers  -- reusable sync/orchestration called from zones/bindings/handlers.
//               File: src/Shell_helpers.cpp
//   Glue     -- what stays in Shell.cpp: the ctor + build_ui() (the ordered
//               construct -> name/register -> bind pass).
//
// Finding things
// --------------
//   "Where is the window built?"        -> Shell_zones.cpp    (build_shell)
//   "Where are actions registered?"     -> Shell_bindings.cpp (bind_actions)
//   "What does 'Dump registry' do?"     -> Shell_handlers.cpp (on_dump_registry)
//   "Where does the recents list sync?" -> Shell_helpers.cpp  (remember_connection / rebuild_*)
//   "Where does a password come from?"  -> Shell_connect.cpp  (on_connect -> keyring lookup)
//   "Where is a table's detail drawn?"  -> Shell_tables.cpp   (show_table)
//   "Where does a write go / undo run?" -> Shell_journal.cpp  (on_new_table / on_undo)
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class AboutWindow;
class ShortcutsDialog;
class NewTableWindow;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: window + sidebar + stack
    void build_pages();                          // category: zone: Acorn's areas
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions + recents group

    // ── handlers ───────────────────────────────────────────────────────────
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: open the About window (dialog-lifetime exemplar)
    void on_shortcuts();                         // category: handler: open the keyboard reference
    void on_recent_open(const std::string& key); // category: handler: connect to a recent (parameterised)
    void on_recent_forget(const std::string& key); // category: handler: drop one recent + its stored password
    void on_recents_clear();                     // category: handler: drop them all + their passwords

    // ── connect (s004) -- src/Shell_connect.cpp ─────────────────────────────
    void build_connect_page();                   // category: connect: the Connect area
    void on_engine_changed();                    // category: connect: file vs server rows
    void on_open_file();                         // category: connect: Ctrl+O -- SQLite + the file dialog
    void on_browse_file();                       // category: connect: pick an SQLite file
    void on_new_file();                          // category: connect: Ctrl+N -- make an empty SQLite file, open it
    void on_connect();                           // category: connect: read the form -> keyring -> start
    void start_connect(db::Engine* e, db::ConnectParams p);  // category: connect: open + introspect on a worker
    void on_connect_done();                      // category: connect: back on the UI thread
    void show_schema();                          // category: connect: paint the table list
    void set_status(const std::string& text, const char* kind);  // category: connect: "", "ok", "error"
    void fill_form(const core::SavedConnection& c);  // category: connect: a recent -> the form
    db::Engine* engine_by_id(const std::string& id) const;  // category: connect

    // ── tables (s004c) -- src/Shell_tables.cpp ──────────────────────────────
    void build_tables_page();                    // category: tables: the read-only schema browser
    void rebuild_table_list();                   // category: tables: the left list, after a connect
    void show_table(const std::string& name);    // category: tables: paint one table's detail
    void open_table(const std::string& name);    // category: tables: go to the Tables area, that table selected

    // ── journal (s005b) -- src/Shell_journal.cpp ────────────────────────────
    // Every write goes through m_journal (the door); the UI only asks.
    void bind_journal();                         // category: journal: win.undo / win.redo / win.new-table + the window's Ctrl+Z
    void attach_journal(db::Engine* e);          // category: journal: a new connection -> a fresh journal (no step carries over)
    void sync_undo();                            // category: journal: re-label + grey Undo / Redo / New Table
    void on_undo();                              // category: journal: Ctrl+Z
    void on_redo();                              // category: journal: Ctrl+Shift+Z
    void on_new_table();                         // category: journal: the New Table window -> Journal::run
    void reload_schema(const std::string& show); // category: journal: re-read the schema after a step, repaint, select `show`
    bool focus_is_text() const;                  // category: journal: a text box keeps its own Ctrl+Z
    void journal_note(const std::string& text, const char* kind);  // category: journal: what a step did, under the Tables title

    // ── ground (s005c) -- src/Shell_ground.cpp ──────────────────────────────
    void bind_ground();                          // category: ground: win.ground (radio) + the saved choice + the provider
    void apply_ground();                         // category: ground: the choice -> the style provider
    void on_ground(const std::string& id);       // category: ground: a preset, or "custom" -> the colour picker
    std::string ground_file() const;             // category: ground: ~/.config/acorn/ground.json

    // ── helpers ────────────────────────────────────────────────────────────
    std::string connections_file() const;        // category: helper: the XDG path (UI-side resolution)
    void remember_connection(const core::SavedConnection& c);  // category: helper: push + persist + repaint
    void rebuild_recents_menu();                 // category: helper: repaint the submenu
    void rebuild_recents_list();                 // category: helper: repaint the Connect page's list
    std::string engine_name(const std::string& id) const;  // category: helper: "sqlite" -> "SQLite"
    keyring::Account account_for(const core::SavedConnection& c) const;  // category: helper: the keyring's key

    // Titlebar: brand logo (start) + hamburger (end).
    widgets::Button     m_logo_button;   // frameless brand mark; opens About
    widgets::MenuButton m_menu_button;

    // Body: sidebar | stack (each pattern is a stack page).
    widgets::Box          m_body;      // horizontal
    widgets::StackSidebar m_sidebar;
    widgets::Stack        m_stack;

    // s004b -- recent connections. Core data (no passwords); the UI resolves
    // the XDG path and drives the two surfaces: the Connect page's list and
    // the hamburger's "Open recent" submenu (CANON: two surfaces, one mechanism).
    std::vector<core::SavedConnection> m_recents;
    std::string                     m_recents_file;
    Glib::RefPtr<Gio::Menu>         m_recents_menu;   // live submenu, rebuilt in place
    Glib::RefPtr<Gio::SimpleAction> m_recents_clear;  // held to toggle enabled state
    widgets::ListBox                m_recent_list;

    // Keyring callbacks arrive later on the main loop; they hold a weak copy
    // of this and do nothing once the window is gone.
    std::shared_ptr<bool>           m_alive = std::make_shared<bool>(true);

    // s004 -- the Connect area. DECLARATION ORDER MATTERS: the loader owns the
    // plugin code, so it is declared before (destroyed after) the connection.
    db::Loader                      m_loader;
    // s005b: the connection is a Gate -- reads pass, a write with the door
    // shut is refused. The journal holds a reference to it, so it is declared
    // AFTER (destroyed BEFORE) the gate.
    std::unique_ptr<db::Gate>       m_conn;
    std::unique_ptr<db::Journal>    m_journal;
    db::Engine*                     m_conn_engine = nullptr;
    bool                            m_writable = false;   // s005b: only files Acorn made
    db::Schema                      m_schema;
    std::string                     m_conn_label;    // "SQLite -- shop.sqlite"
    std::vector<db::Engine*>        m_engines;       // dropdown order
    Glib::RefPtr<Gtk::StringList>   m_engine_names;
    Gtk::DropDown                   m_engine_pick;
    widgets::Box                    m_connect_page;
    widgets::Box                    m_file_rows;
    widgets::Box                    m_server_rows;
    widgets::Entry                  m_path;
    widgets::Entry                  m_host, m_port, m_database, m_user, m_password;
    widgets::CheckButton            m_remember;      // "Remember the password in the keyring"
    widgets::Button                 m_browse;
    widgets::Button                 m_new_file;      // s005a
    widgets::Button                 m_connect;
    widgets::Label                  m_status;
    widgets::ListBox                m_tables;
    // The worker hands its result back through the dispatcher; the mutex
    // guards the hand-off slots, which the UI thread reads in on_connect_done.
    std::thread                     m_worker;
    Glib::Dispatcher                m_connect_done;
    std::mutex                      m_result_mx;
    std::unique_ptr<db::Connection> m_result_conn;
    db::Schema                      m_result_schema;
    std::string                     m_result_error;
    bool                            m_result_writable = false;
    db::Engine*                     m_pending_engine = nullptr;
    // What the connect in flight was asked for: on success it becomes a
    // recent, and (server, Remember ticked, password typed) the typed password
    // goes to the keyring. The password is dropped as soon as it is handed on.
    core::SavedConnection           m_pending;
    std::string                     m_pending_password;
    bool                            m_pending_remember = false;
    bool                            m_pending_from_keyring = false;

    // s004c -- the Tables area: pick a table on the left, its columns,
    // indexes and links on the right. Read-only; painted from m_schema.
    widgets::Box                    m_tables_page;
    widgets::ListBox                m_table_list;
    widgets::Box                    m_table_detail;
    widgets::Label                  m_tables_where;  // "PostgreSQL -- shop on db.lan · 2 tables, 1 view"
    widgets::Button                 m_new_table_btn; // s005b: "New Table…" (win.new-table)
    widgets::Label                  m_journal_note;  // s005b: "Undid New Table “notes”." / a refusal

    // s005b -- the Edit section of the menu (re-labelled per step) and the
    // actions held so they can be greyed.
    Glib::RefPtr<Gio::Menu>         m_edit_menu;
    Glib::RefPtr<Gio::SimpleAction> m_undo_action, m_redo_action, m_new_table_action;
    std::unique_ptr<NewTableWindow> m_new_table_win;

    // s005c -- the ground: the saved choice, its radio action, and a style
    // provider one step above the skin that redefines the skin's colours.
    core::GroundChoice              m_ground;
    Glib::RefPtr<Gio::SimpleAction> m_ground_action;
    Glib::RefPtr<Gtk::CssProvider>  m_ground_css;
    std::string                     m_table_sel;     // the shown table ("" = none)
    bool                            m_table_painting = false;

    // The About window: a hide-on-close singleton, built lazily on first open.
    std::unique_ptr<AboutWindow>    m_about;
    std::unique_ptr<ShortcutsDialog> m_shortcuts;   // shortcut registry's GTK consumer (same lifetime stone)
};

}  // namespace acorn
