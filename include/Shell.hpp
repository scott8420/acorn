#pragma once
#include "widgets/Widgets.hpp"
#include "core/Stones.hpp"   // s046: the undo stone's demo model + journal
#include "db/Loader.hpp"     // s004: engine plugins
#include "db/Schema.hpp"

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
//   "Where does the recents list sync?" -> Shell_helpers.cpp  (note_recent / rebuild_*)
//   "Where does Ctrl+Z go?"             -> Shell_bindings.cpp (the window's undo keys)
//                                          + Shell_handlers.cpp (on_undo / on_redo)
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class AboutWindow;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: window + sidebar + stack
    void build_pages();                          // category: zone: the pattern pages
    void build_demo_pages();                     // category: zone: Cairn's demo pages (retired s004, not called)
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions + recents group

    // ── handlers ───────────────────────────────────────────────────────────
    void on_pick_folder();                       // category: handler: folder chooser -> remember it
    void on_recent_open(const std::string& p);   // category: handler: open a recent (parameterised)
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: open the About window (dialog-lifetime exemplar)
    void on_shortcuts();   // handler: open the keyboard reference (renders from core::shortcut_registry)
    // s046 -- the undo stone. Every verb on the Undo page goes through the
    // journal (core/Undo.hpp); nothing on it writes the model any other way.
    void on_undo();                              // category: handler: Ctrl+Z / menu / button
    void on_redo();                              // category: handler: Ctrl+Shift+Z, Ctrl+Y / menu / button
    void on_stone_add();                         // category: handler: a new stone below the selection
    void on_stone_delete();                      // category: handler: delete; the neighbour takes the selection
    void on_stone_up();                          // category: handler: swap with the one above
    void on_stone_down();                        // category: handler: swap with the one below
    void on_stone_rename();                      // category: handler: the name field, on Enter / leaving it
    void on_stone_size();                        // category: handler: the slider (keys merge; a drag is one step)

    // ── connect (s004) -- src/Shell_connect.cpp ─────────────────────────────
    void build_connect_page();                   // category: connect: the Connect area
    void on_engine_changed();                    // category: connect: file vs server rows
    void on_browse_file();                       // category: connect: pick an SQLite file
    void on_connect();                           // category: connect: open read-only + introspect on a worker
    void on_connect_done();                      // category: connect: back on the UI thread
    void show_schema();                          // category: connect: paint the table list
    void set_status(const std::string& text, const char* kind);  // category: connect: "", "ok", "error"

    // ── helpers ────────────────────────────────────────────────────────────
    void note_recent(const std::string& path);   // category: helper: push onto the list + persist
    void rebuild_recents_menu();                  // category: helper: repaint the submenu
    void rebuild_recents_page();                  // category: helper: repaint the live list page
    std::string recents_file() const;             // category: helper: the XDG path (UI-side resolution)
    void build_undo_page();                       // category: zone: the Undo page (s046)
    void queue_stones_repaint();                  // category: helper: one repaint per idle, after the writes
    void repaint_stones();                        // category: helper: the list + the detail fields from the model
    void select_stone(const std::string& id, bool keys);   // category: helper: select (+ keyboard) after a repaint
    void update_undo_ui();                        // category: helper: Undo / Redo say what they would do, grey when nothing
    bool focus_is_text() const;                   // category: helper: the keyboard is in a text box (its own Ctrl+Z)

    // Titlebar: brand logo (start) + hamburger (end).
    widgets::Button     m_logo_button;   // frameless brand mark; opens About
    widgets::MenuButton m_menu_button;

    // Body: sidebar | stack (each pattern is a stack page).
    widgets::Box          m_body;      // horizontal
    widgets::StackSidebar m_sidebar;
    widgets::Stack        m_stack;

    // The recents "live page": a label + a scrolling list rebuilt on change.
    widgets::Box            m_recents_page;   // vertical
    widgets::ScrolledWindow m_recents_scroll;
    widgets::ListBox        m_recents_list;

    // Recents state (core data; UI holds it + resolves the path -- the seam).
    std::vector<std::string>        m_recents;
    std::string                     m_recents_file;
    Glib::RefPtr<Gio::Menu>         m_recents_menu;   // live submenu, rebuilt in place
    Glib::RefPtr<Gio::SimpleAction> m_recents_clear;  // held to toggle enabled state

    // s046 -- the undo stone. Declaration order matters: the store before the
    // journal that holds a reference to it.
    core::Stones       m_stones;
    core::StoneJournal m_journal{m_stones};
    std::string        m_sel;                  // the selected stone's id ("" = none)
    bool               m_painting = false;     // a repaint sets fields: not a user choice
    bool               m_repaint_queued = false;
    bool               m_keys_to_list = false; // after the repaint, give the list the keyboard
    widgets::Box       m_undo_page;
    widgets::ListBox   m_stone_list;
    widgets::Entry     m_stone_name;
    widgets::Scale     m_stone_size;
    widgets::Button    m_undo_button;
    widgets::Button    m_redo_button;
    widgets::Label     m_undo_said;            // "Undid: Delete “Jade”"
    Glib::RefPtr<Gio::Menu>         m_edit_menu;   // Undo / Redo items, re-labelled live
    Glib::RefPtr<Gio::SimpleAction> m_act_undo, m_act_redo;

    // s004 -- the Connect area. DECLARATION ORDER MATTERS: the loader owns the
    // plugin code, so it is declared before (destroyed after) the connection.
    db::Loader                      m_loader;
    std::unique_ptr<db::Connection> m_conn;          // read-only until s005's door
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
    widgets::Button                 m_browse;
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

    // The About window: a hide-on-close singleton, built lazily on first open.
    std::unique_ptr<AboutWindow>    m_about;
    std::unique_ptr<ShortcutsDialog> m_shortcuts;   // shortcut registry's GTK consumer (same lifetime stone)
};

}  // namespace acorn
