#pragma once
#include <gtkmm/checkbutton.h>
#include "db/Import.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

#include <expected>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ImportWindow -- Browse -> Import… (s018b). The file already read
// (db::read_delimited); this shows its first rows, a field (or "-- skip --")
// over each column, matched by name to start with, and a live line saying
// what Import will do ("Will add 40 records to “contacts” -- 2 rows can't go
// in as they are"). Import hands the plan to the caller (Shell), which runs
// db::import_records through the journal; the window then turns into the
// report -- what went in, and each refused row by its line number.
//
// Nothing here writes. Lifetime: the About shape -- a hide-on-close singleton
// Shell builds once; start() resets it.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class ImportWindow : public Gtk::Window {
public:
    // s020c: the import runs on Shell's worker. Import starts it (or refuses
    // at once through `done`); `done` comes back on the UI thread with what
    // went in -- the window then turns into the report. The data is the
    // window's own and is read by reference while it runs (nothing can call
    // start() then: the main window is shut).
    using Done   = std::function<void(std::expected<db::ImportResult, db::Error>)>;
    using Import = std::function<void(const db::ImportPlan&, const db::Delimited&, Done)>;

    explicit ImportWindow(Gtk::Window& parent);
    ~ImportWindow() override;

    void start(const db::Table& t, const std::string& file_name, db::Delimited d, Import import);
    // s020c: Shell says the worker started: the window says how far it's got
    // (`count`: rows looked at so far), Cancel becomes Stop (and Esc).
    void running(std::function<std::int64_t()> count, std::function<void()> stop);

private:
    void build_grid();
    db::ImportPlan plan() const;
    void refresh();   // the header row's look, the live line, Import's sensitivity
    void on_import();
    void finish(const std::expected<db::ImportResult, db::Error>& r);   // s020c: back from the worker
    void set_running(bool on);

    db::Table                   m_table;
    std::string                 m_file;
    db::Delimited               m_data;
    Import                      m_import;
    std::vector<std::string>    m_choices;   // "" (skip) then the fields
    std::vector<Gtk::DropDown*> m_picks;     // per column
    std::vector<Gtk::Widget*>   m_first;     // the first line's cells (names when the box is ticked)
    bool                        m_resetting = false;
    Gtk::Label*                 m_hint = nullptr;   // the plan's how-to line (hidden in the report)
    Gtk::Box*                   m_match_row = nullptr;   // s024a: Match by ... / add the rest
    Gtk::DropDown*              m_match = nullptr;       // "-- add every row --" then the fields
    Gtk::CheckButton*           m_add_rest = nullptr;
    std::int64_t                m_rows = 0;         // rows Import will look at (the live line's count)
    bool                        m_running = false;  // s020c: the worker has it
    std::function<std::int64_t()> m_count;
    std::function<void()>       m_stop;
    sigc::connection            m_tick;

    widgets::Label          m_head;
    widgets::CheckButton    m_header;
    widgets::ScrolledWindow m_scroll;
    Gtk::Grid               m_grid;
    widgets::Label          m_says;
    widgets::Label          m_reason;
    widgets::Box            m_plan_buttons;
    widgets::Button         m_cancel, m_ok;
    widgets::Label          m_done;
    Gtk::TextView           m_refused;
    widgets::ScrolledWindow m_refused_scroll;
    widgets::Button         m_close;
};

}  // namespace acorn
