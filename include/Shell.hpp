#pragma once
#include "widgets/Widgets.hpp"
#include "core/Connections.hpp"  // s004b: recent connections
#include "Keyring.hpp"             // s004b: passwords in the GNOME keyring
#include "core/Ground.hpp"         // s005c: the skin's base colour
#include "core/Solution.hpp"       // s009: the .acorn solution document
#include <gtkmm/cssprovider.h>
#include "db/Loader.hpp"     // s004: engine plugins
#include "db/Schema.hpp"
#include "db/Journal.hpp"   // s005b: the door
#include "db/Browse.hpp"    // s006b: a table's records, a page at a time
#include <gtkmm/togglebutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/entry.h>
#include <map>

#include <glibmm/dispatcher.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/drawingarea.h>   // s010b
#include <gtkmm/fixed.h>
#include <gtkmm/overlay.h>
#include <gtkmm/stringlist.h>

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>

#include <memory>
#include <optional>
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
//   "Where are records shown?"          -> Shell_browse.cpp   (paint_browse)
//   "Where are links drawn / made?"     -> Shell_graph.cpp    (paint_graph / on_graph_drop)
//   "Where are layouts edited?"         -> Shell_layouts.cpp  (paint_layouts / keep_layout)
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class AboutWindow;
class ShortcutsDialog;
class NewTableWindow;
class DesignTableWindow;

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
    void on_new_address_book();                  // category: connect: s009b -- a new file with the Address Book starter
    void new_file_dialog(bool starter);          // category: connect: the save dialog -> db::new_file -> connect
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
    void on_design_table();                      // category: journal: the designer (s007b) -> verbs::design_table
    void reload_schema(const std::string& show); // category: journal: re-read the schema after a step, repaint, select `show`
    bool focus_is_text() const;                  // category: journal: a text box keeps its own Ctrl+Z
    void journal_note(const std::string& text, const char* kind);  // category: journal: what a step did, under the Tables title

    // ── browse (s006b) -- src/Shell_browse.cpp ──────────────────────────────
    void build_browse_page();                    // category: browse: the Browse area (cards / grid, paged)
    void rebuild_browse_pick();                  // category: browse: the table picker, after a connect / a step
    void browse_table(const std::string& name);  // category: browse: show this table's first page
    void open_browse(const std::string& name);   // category: browse: go to Browse, that table showing
    void load_browse_page(bool keep_panel = false);  // category: browse: read the page m_browse_req names, repaint (keep_panel: a field just saved, its box keeps the focus)
    void paint_browse();                         // category: browse: cards or grid from m_browse_data
    void paint_cards();                          // category: browse: one calm card per record
    void paint_grid();                           // category: browse: the dense grid, sortable headers
    // s010a -- the Form view: the picked record through its layout (src/Shell_form.cpp).
    void paint_form();                           // category: form: the picked record, drawn from its layout, editable in place
    core::Layout form_layout(bool* kept = nullptr) const;  // category: form: the shown table's layout -- kept in the solution, else the default
    void form_step(int delta);                   // category: form: the previous / next record (across pages)
    void browse_select(int index);               // category: browse: the picked record (kept across views / pages)
    const db::Table* browse_info() const;        // category: browse: the shown table in m_schema (nullptr: none)

    // ── records (s006c) -- src/Shell_records.cpp ────────────────────────────
    // Edits in Browse: the record panel beside the cards / grid. Every write
    // is a verb through m_journal (set_field / new_record / delete_record).
    void paint_record();                         // category: records: the panel -- the picked record, or a new one
    void commit_field(const std::string& row, const std::string& column, const std::string& typed);  // category: records: Enter / leave -> verbs::set_field
    void on_new_record();                        // category: records: win.new-record -> the panel, blank
    void on_add_record();                        // category: records: the panel's Add -> verbs::new_record
    void on_delete_record();                     // category: records: win.delete-record -> verbs::delete_record
    bool browse_writable() const;                // category: records: Acorn-made + a table with a key
    void browse_note(const std::string& text, const char* kind);  // category: records: what a step did, under the Browse title
    const db::Value* browse_value(const std::string& column) const;  // category: records: the picked record's field as last read
    // s010a: Enter / leave saves, Esc (or Ctrl+Z with typing pending) puts the
    // box back -- the panel's and the Form's boxes alike. `counter` / `gen`:
    // a box from an older paint never saves.
    void wire_field_entry(Gtk::Entry* e, const std::string& row_id, const std::string& column,
                          const unsigned* counter, unsigned gen);

    // ── graph (s008b) -- src/Shell_graph.cpp ────────────────────────────────
    // Tables as boxes, links as lines; a drag makes a link, a line's handle
    // changes or unlinks it. Every change is a verb through m_journal.
    void build_graph_page();                     // category: graph: the Graph area
    void paint_graph();                          // category: graph: boxes laid out by depth, lines drawn
    void graph_note(const std::string& text, const char* kind);  // category: graph: under the Graph title
    void on_graph_drop(const std::string& st, const std::string& sf, const std::string& dt, const std::string& df,
                       Gtk::Widget& anchor);     // category: graph: a drop -- which table is the parent?
    void offer_link(const std::string& child, const std::string& field, const std::string& parent,
                    Gtk::Widget& anchor);        // category: graph: the popover for a new link
    void offer_line(std::size_t idx);            // category: graph: a line's handle -> change / unlink
    void build_link_popover(const std::string& child, const std::vector<std::string>& fields, const std::string& parent,
                            db::OnDelete rule, bool existing);  // category: graph: the popover's body
    void refresh_marks();                        // category: graph: marked records re-read (fixed ones drop off)
    std::string mark_of(const std::string& table, const std::string& id) const;  // category: graph: why a record is marked ("" = not)
    void paint_marks_note();                     // category: graph: Browse shows the marks
    void open_solution(const core::SavedConnection& c);  // category: graph: s009 -- the connection's .acorn, read (or a fresh one)
    void on_open_solution();                     // category: graph: s009c -- pick an .acorn, connect to what it points at
    void on_repoint_solution();                  // category: graph: s009c -- the next connection takes this solution along

    // ── layouts (s010b) -- src/Shell_layouts.cpp ────────────────────────────
    // The layout editor: a table's layouts, a canvas, an inspector. Every edit
    // is verbs::save_layout (one step on the same Ctrl+Z list); kept in the
    // solution, never the database -- so it works on a read-only connection.
    void build_layouts_page();                   // category: layouts: the Layouts area
    void paint_layouts();                        // category: layouts: pickers, canvas and inspector from the solution
    void paint_inspector();                      // category: layouts: the picked object's field / words / place
    void layouts_note(const std::string& text, const char* kind);  // category: layouts: under the Layouts title
    void draw_layout(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);  // category: layouts: the canvas
    void keep_layout(core::Layout l, const std::string& did);  // category: layouts: verbs::save_layout, the note, a repaint
    void layout_nudge(int dx, int dy);           // category: layouts: the arrows -- the picked object by 8 (Shift: 1)
    void layout_remove_object();                 // category: layouts: Delete -- the picked object off the layout
    void on_layout_new();                        // category: layouts: the default, under a new name
    void on_layout_duplicate();                  // category: layouts: the shown layout, under a new name
    void on_layout_rename();                     // category: layouts: a popover -> verbs::rename_layout
    void on_layout_delete();                     // category: layouts: verbs::delete_layout
    std::vector<std::string> layout_names(const std::string& table) const;  // category: layouts: the default first, then the kept ones
    std::string picked_layout(const std::string& table) const;  // category: layouts: the name last picked for the table (else the default)
    core::Layout layout_of(const std::string& table, const std::string& name, bool* kept = nullptr) const;  // category: layouts: kept, else the computed default
    std::vector<std::string> fields_of(const std::string& table) const;  // category: layouts: the live table's fields, in order

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
    bool                            m_pending_starter = false;   // s009b: fill the new file with the Address Book

    // s004c -- the Tables area: pick a table on the left, its columns,
    // indexes and links on the right. Read-only; painted from m_schema.
    widgets::Box                    m_tables_page;
    widgets::ListBox                m_table_list;
    widgets::Box                    m_table_detail;
    widgets::Label                  m_tables_where;  // "PostgreSQL -- shop on db.lan · 2 tables, 1 view"
    widgets::Button                 m_new_table_btn; // s005b: "New Table…" (win.new-table)
    widgets::Label                  m_journal_note;  // s005b: "Undid New Table “notes”." / a refusal

    // s006b -- the Browse area: one table's records, a page at a time, as
    // cards or a grid. Read through the Gate (SELECTs pass with the door shut).
    widgets::Box                    m_browse_page;
    widgets::Label                  m_browse_where;   // "Connect to a database first" / "“people” -- 312 records"
    Glib::RefPtr<Gtk::StringList>   m_browse_names;
    Gtk::DropDown                   m_browse_pick;
    widgets::Named<Gtk::ToggleButton> m_browse_cards_btn, m_browse_grid_btn, m_browse_form_btn;
    widgets::Button                 m_browse_prev, m_browse_next;
    widgets::Label                  m_browse_range;   // "51–100 of 312"
    widgets::ScrolledWindow         m_browse_scroll;
    std::string                     m_browse_table;   // "" = none showing
    db::PageRequest                 m_browse_req;
    db::Page                        m_browse_data;
    std::string                     m_browse_sel;     // the picked record's journal id ("" = none)
    int                             m_browse_sel_index = -1;
    enum class BrowseView { Cards, Grid, Form };
    BrowseView                      m_browse_view = BrowseView::Cards;   // s010a: + Form
    std::string                     m_form_focus;     // s010a: the Form box last focused (kept across a repaint)
    bool                            m_browse_painting = false;
    // s006c -- the record panel and its verbs.
    widgets::Label                  m_browse_note;    // "Changed “age”. Ctrl+Z undoes it." / a refusal
    widgets::Button                 m_browse_new, m_browse_delete;
    widgets::ScrolledWindow         m_record_scroll;
    bool                            m_record_new = false;   // the panel is a blank new record
    // Bumped by every paint_record: a box from an older paint never saves (a
    // box torn down by a repaint loses focus, and its leave would otherwise
    // save its stale text as a new step -- s006c drive: an undo re-done).
    unsigned                        m_record_gen = 0;
    unsigned                        m_view_gen = 0;   // the same, for the cards / grid (a torn-down list says "nothing selected")
    std::vector<std::pair<std::string, Gtk::Widget*>> m_record_inputs;  // new mode: column -> Entry / CheckButton
    Glib::RefPtr<Gio::SimpleAction> m_new_record_action, m_delete_record_action;

    // s008b -- the Graph area.
    struct GraphLine {
        std::string child, parent;
        std::vector<std::string> fields, refs;
        db::OnDelete rule = db::OnDelete::Restrict;
        struct { double x = 0, y = 0, w = 0, h = 0; } mutable hit;   // the handle, as last drawn (canvas coords)
    };
    // Records that stop a link: by child table, the records and why.
    struct Mark {
        std::vector<std::string> ids;
        std::string parent;
        std::vector<std::string> fields;
        std::string why;
    };
    widgets::Box                    m_graph_page;
    widgets::Label                  m_graph_where, m_graph_note;
    widgets::ScrolledWindow         m_graph_scroll;
    Gtk::Popover                    m_graph_pop;
    std::vector<GraphLine>          m_graph_lines;
    std::map<std::pair<std::string, std::string>, Gtk::Widget*> m_graph_rows;   // (table, field) -> its row
    std::map<std::string, Mark>     m_marks;
    core::Solution                  m_solution;        // s009: the open connection's .acorn (box places)
    std::string                     m_solution_path;
    std::string                     m_forced_solution;   // s009c: Open Solution... -- this file, not the derived one
    std::optional<core::Solution>   m_repoint;           // s009c: carried to the next connection
    unsigned                        m_graph_gen = 0;
    Gtk::Widget*                    m_graph_canvas = nullptr;   // the Fixed of boxes (handles are hit on it)

    // s010b -- the Layouts area.
    widgets::Box                    m_layouts_page;
    widgets::Label                  m_layouts_where, m_layouts_note, m_layouts_caption;
    Glib::RefPtr<Gtk::StringList>   m_lay_tables, m_lay_names;
    Gtk::DropDown                   m_lay_table_pick, m_lay_name_pick;
    widgets::Button                 m_lay_new, m_lay_dup, m_lay_rename, m_lay_delete;
    widgets::ScrolledWindow         m_lay_scroll;
    Gtk::Overlay                    m_lay_overlay;
    Gtk::Fixed                      m_lay_fixed;     // sizes the canvas (room to drag past the body)
    Gtk::Box                        m_lay_paper;     // the body, styled as the Form's
    Gtk::DrawingArea                m_lay_canvas;    // the objects, drawn; every press lands here
    widgets::Box                    m_lay_inspector;
    Gtk::Popover                    m_lay_pop;       // Rename
    std::string                     m_lay_table;     // the table whose layouts are shown
    std::map<std::string, std::string> m_layout_pick;   // table -> the layout last picked (Layouts and Form share it)
    core::Layout                    m_lay;           // the shown layout (a drag changes it live, then keeps it)
    bool                            m_lay_kept = false;
    int                             m_lay_sel = -1;  // the picked object
    core::Grab                      m_lay_grab = core::Grab::None;
    core::LayoutObject              m_lay_from;      // the picked object as the drag began
    int                             m_lay_line = 0;  // s010d: a part line under the drag -- 1 header, 2 footer, 3 the bottom edge
    core::Layout                    m_lay_line_from; // the layout as that drag began
    bool                            m_lay_painting = false;
    unsigned                        m_lay_gen = 0;   // an inspector box from an older paint never saves
    std::string                     m_lay_focus;     // the inspector box last focused (kept across a repaint)

    // s005b -- the Edit section of the menu (re-labelled per step) and the
    // actions held so they can be greyed.
    Glib::RefPtr<Gio::Menu>         m_edit_menu;
    Glib::RefPtr<Gio::SimpleAction> m_undo_action, m_redo_action, m_new_table_action;
    std::unique_ptr<NewTableWindow> m_new_table_win;
    std::unique_ptr<DesignTableWindow> m_design_win;              // s007b
    Glib::RefPtr<Gio::SimpleAction> m_design_table_action;        // s007b: win.design-table

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
