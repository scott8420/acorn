#pragma once
#include "db/Journal.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

#include <expected>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// DesignTableWindow -- the table designer (s007b). One table's fields and
// indexes; the edits ACCUMULATE here (rename a field in place, add fields,
// mark fields / indexes to delete, add an index), the exact SQL shows live
// underneath -- what db::verbs::preview gives, which is what the store runs --
// and Apply hands the whole picture to Shell as ONE step (db::verbs::
// design_table): one Ctrl+Z undoes it.
//
// RULES (the data is sacred): a change that takes values with it says so in
// words, with an "I understand" that is never ticked for you; Apply stays grey
// until it is. s007c: a field's type, required and default are edited in its
// row (values that won't fit the new kind, empties a "required" fills -- said
// in words first). The key is shown, not editable (s007d).
//
// The window never touches a connection: Preview / Losses / Apply are Shell's.
// Lifetime: the NewTableWindow shape -- a hide-on-close singleton; start()
// resets it for a table.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

class DesignTableWindow : public Gtk::Window {
public:
    using Preview = std::function<std::expected<std::vector<std::string>, db::Error>(const db::TableDef&)>;
    using Losses  = std::function<std::expected<std::vector<std::string>, db::Error>(const db::TableDef&)>;
    using Apply   = std::function<std::expected<void, db::Error>(const db::TableDef&)>;

    explicit DesignTableWindow(Gtk::Window& parent);
    ~DesignTableWindow() override;

    void start(db::Dialect d, const db::TableDef& live, Preview preview, Losses losses, Apply apply);

private:
    struct FieldRow {
        std::string    orig;               // "" = a new field
        Gtk::Box*      box = nullptr;
        Gtk::Entry*    name = nullptr;
        Gtk::DropDown* type = nullptr;     // s007c: every field but the key
        Gtk::CheckButton* required = nullptr;
        Gtk::Entry*    dflt = nullptr;     // the default as typed ("" = none)
        Gtk::Button*   del = nullptr;
        bool           deleted = false;
    };
    struct IndexRow {
        db::IndexDef   ix;                 // as live has it (orig) or as made here
        bool           made = false;       // added in this window
        Gtk::Box*      box = nullptr;
        Gtk::Label*    fields = nullptr;
        Gtk::Button*   del = nullptr;
        bool           deleted = false;
    };

    void add_field_row(const db::ColumnDef* live);
    void add_index_row(const db::IndexDef& ix, bool made);
    void rebuild_index_fields();           // the New Index field picker follows the fields
    std::string renamed(const std::string& orig) const;   // orig -> its name in the window
    bool field_going(const std::string& orig) const;
    db::TableDef want() const;
    std::string why_not() const;           // the window's own refusals, in words ("" = fine)
    // s007c: the field a row asks for (its type, required, default); the
    // reason in words when the typed default won't do.
    std::expected<db::ColumnDef, std::string> field_of(const FieldRow& f) const;
    void refresh();
    void on_apply();

    db::Dialect             m_dialect = db::Dialect::SQLite;
    db::TableDef            m_live;
    Preview                 m_preview_fn;
    Losses                  m_losses_fn;
    Apply                   m_apply_fn;
    std::vector<FieldRow>   m_fields;
    std::vector<IndexRow>   m_indexes;
    std::vector<std::string> m_pick_names;   // the New Index picker's rows
    bool                    m_resetting = false;

    widgets::Label          m_title;
    widgets::Box            m_field_box;
    widgets::Button         m_add_field;
    widgets::Box            m_index_box;
    widgets::Entry          m_ix_name;
    Gtk::DropDown*          m_ix_field = nullptr;
    widgets::CheckButton    m_ix_unique;
    widgets::Button         m_ix_add;
    Gtk::TextView           m_sql;
    widgets::Label          m_losses;
    widgets::CheckButton    m_understand;
    widgets::Label          m_reason;
    widgets::Button         m_cancel;
    widgets::Button         m_ok;
};

}  // namespace acorn
