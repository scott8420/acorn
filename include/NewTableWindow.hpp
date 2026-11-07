#pragma once
#include "db/Journal.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

#include <expected>
#include <functional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// NewTableWindow -- the first write Acorn offers (s005b). Deliberately lean:
// a name, an optional counting key, columns (name + Acorn field type), and the
// EXACT SQL previewed live underneath -- the string db::create_table_sql gives
// the store, so what you read is what runs (RULES: schema changes are
// previewed as SQL). The full table designer -- keys, indexes, links, changes
// to an existing table -- is s007.
//
// Create hands the TableDef to the caller (Shell), which runs it through the
// change journal; this window never touches a connection. On a refusal the
// reason shows here and the window stays open.
//
// Lifetime: the About shape -- a hide-on-close singleton Shell builds once and
// re-presents, so its named children register once. start() resets it.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class NewTableWindow : public Gtk::Window {
public:
    using Create = std::function<std::expected<void, db::Error>(const db::TableDef&)>;

    explicit NewTableWindow(Gtk::Window& parent);
    ~NewTableWindow() override;

    // Reset and show, for this connection.
    void start(db::Dialect d, char quote, const db::Schema& existing, Create create);

private:
    struct ColumnRow {
        Gtk::Box*      box = nullptr;
        Gtk::Entry*    name = nullptr;
        Gtk::DropDown* type = nullptr;
    };

    void add_column(const std::string& name = {}, db::FieldType t = db::FieldType::Text);
    void remove_column(Gtk::Box* box);
    db::TableDef def() const;
    void refresh();          // preview + the Create button's sensitivity + the reason
    void on_create();

    db::Dialect             m_dialect = db::Dialect::SQLite;
    char                    m_quote = '"';
    db::Schema              m_existing;
    Create                  m_create;
    std::vector<ColumnRow>  m_rows;
    bool                    m_resetting = false;

    widgets::Entry          m_name;
    widgets::CheckButton    m_key;
    widgets::Box            m_columns;
    widgets::Button         m_add;
    Gtk::TextView           m_preview;
    widgets::Label          m_reason;
    widgets::Button         m_cancel;
    widgets::Button         m_ok;
};

}  // namespace acorn
