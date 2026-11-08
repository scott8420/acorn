// db/Journal.hpp -- THE DOOR (REQUIREMENTS §1.5, §6 s005): every write Acorn
// makes to a database goes through db::Journal::run -- preview, act in a
// transaction, record, undo.
//
// It is core::Journal (docs/undo.md -- "a step is two pictures, not an
// inverse") over a database-shaped store:
//
//     Id    "table:<name>"                 a table
//           "row:<n>:<table>:<key>"        a record (s006): n = the table
//                                          name's byte length, key = the
//                                          primary key's fields BY NAME and
//                                          their values, encoded (s007d: a
//                                          step taken under another key is
//                                          refused, never applied to the
//                                          wrong record)
//           "field:<n>:<table>:<column>"   one field's values in every record
//                                          (s007) -- what Delete Field keeps
//                                          so undo can put them back
//     Value Record = TableDef | RowDef | FieldData. TableDef: the table's
//           shape as introspection reads it (fields, indexes), plus its row
//           count. RowDef: every field of one record, by column. FieldData: a
//           field's non-NULL values by record key. Absent = no such table /
//           record / field.
//
// A table's identity is its fields BY NAME (s007): their order is how they
// are shown, not what they are. Undoing Delete Field puts the field back in
// its old place where the engine can (s007e: SQLite makes the table again in
// the picture's order, MariaDB adds it AFTER the field before it); PostgreSQL
// can only add at the end, and SQLite falls back to the end when the table
// can't be made again. Either way the pictures match.
//
// Store::put turns the pictures into SQL: a table absent -> present is CREATE
// TABLE, present -> absent DROP TABLE, present -> a different shape is ALTER
// (s007: plan_alter -- fields added / renamed / dropped, indexes made /
// dropped); a record absent -> present is INSERT
// (every field, the key included), present -> absent DELETE, changed UPDATE
// (only the fields that differ). So New Table, New Record, a field edit and
// Delete Record are undoable the day they go through run(), with no
// hand-written inverse. A record is found by its PRIMARY KEY: a table without
// one is browsed, never written; a key field is never changed (s006). Two safety rules sit in put()
// and in undo / redo, because a database can change behind Acorn's back:
//
//   * put() never drops a table that holds rows, and never drops a field that
//     holds values unless the step keeps them (a "field:" record, s007) -- it
//     refuses, and the step does not happen;
//   * undo / redo first check the live records still look exactly like the
//     step left them. Anything different -> refused, nothing touched.
//
// THE GATE. The connection the UI holds is a Gate: reads pass, and a write
// (anything not SELECT / SHOW / EXPLAIN / a read PRAGMA) made while the door is
// shut is REFUSED and counted (strays()). The selftest fails on a stray. So
// "forgot to go through the journal" cannot quietly write.
//
// Transactions. Each step runs inside BEGIN .. COMMIT where the engine can roll
// the statement back (SQLite, PostgreSQL: transactional DDL; records on every
// engine). MariaDB commits DDL implicitly, so a step that touches a TABLE there
// may declare only that one table -- one statement, atomic by itself -- and a
// step that declares more is refused before it runs. On every engine a step
// writes only the records it declared; a step that makes a record declares
// new_row_id(table) and the made record joins the step (Store::insert).
#pragma once

#include "core/Undo.hpp"
#include "core/Solution.hpp"
#include "db/Engine.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace acorn::db {

// ── the pictures ──────────────────────────────────────────────────────────────
struct ColumnDef {
    std::string name;
    std::string decl_type;                    // the engine's own words ("bigint")
    bool        nullable = true;
    std::optional<std::string> default_sql;
    int         pk_position = 0;              // 1-based; 0 = not in the key
    bool        auto_increment = false;
    bool operator==(const ColumnDef&) const = default;
};

// An index Acorn can make (s007): name, fields in key order, unique. The index
// backing the primary key is not one of these -- it is the key.
struct IndexDef {
    std::string              name;
    std::vector<std::string> columns;
    bool                     unique = false;
    bool                     partial = false;  // read from the engine; Acorn never makes one
    bool operator==(const IndexDef&) const = default;
};

// s008 -- a link: this table's fields that point at another table's key (a
// foreign key). The child holds the link; the parent is pointed at. Acorn
// always points a link at the parent's KEY, written out field by field.
// Compared without its name (the engine names it; PostgreSQL / MariaDB drop
// it by that name). NO ACTION and RESTRICT read as the same rule.
enum class OnDelete { Restrict, Cascade, SetEmpty };
struct LinkDef {
    std::string              name;            // the engine's; "" for SQLite (and a link not made yet)
    std::vector<std::string> columns;         // the child's fields, in the parent key's order
    std::string              ref_table;       // the parent
    std::vector<std::string> ref_columns;     // the parent's key fields
    std::string              on_delete = "RESTRICT";
    std::string              on_update = "NO ACTION";
    bool operator==(const LinkDef& o) const;
    OnDelete deleting() const;                // on_delete as Acorn's three choices (SET DEFAULT -> Restrict)
};
std::string on_delete_sql(OnDelete o);        // "RESTRICT" / "CASCADE" / "SET NULL"
std::string on_delete_words(OnDelete o);      // "Restrict" / "Cascade" / "Set Empty"

struct TableDef {
    std::string            name;
    std::vector<ColumnDef> columns;
    std::int64_t           rows = 0;          // live count -- a step never drops data
    std::vector<IndexDef>  indexes;           // by name (s007)
    std::vector<LinkDef>   links;             // s008: this table's links out, by fields
    std::string            key_name;          // the engine's name for the key's own index / constraint
                                              // (s007d: PostgreSQL drops it by name); not compared
    // Same fields by name (order is display only), same indexes, same links,
    // same rows.
    bool operator==(const TableDef& o) const;

    // introspection -> picture (rows = 0). With the schema, a SQLite link
    // written `REFERENCES t` (no fields) reads as t's key fields.
    static TableDef from(const Table& t, const Schema* s = nullptr);
    const LinkDef* link(const std::vector<std::string>& columns) const;   // by its fields
    const ColumnDef* column(const std::string& n) const;
};

// One record: every column of its table, in the table's order (s006).
struct RowDef {
    std::string              table;
    std::vector<std::string> columns;
    std::vector<Value>       values;          // values[i] is columns[i]
    // Same table, same fields BY NAME holding the same values (s007: a field
    // put back by undo comes back last; the record is still the same).
    bool operator==(const RowDef& o) const;

    const Value* field(const std::string& column) const;   // nullptr: no such column
};

// One field's values in every record of its table, by key (s007). Only the
// non-NULL ones -- a field added back starts all NULL.
struct FieldData {
    std::string                     table;
    std::string                     column;
    std::vector<std::string>        key_fields;   // what `keys` are values of (s007d)
    std::vector<std::vector<Value>> keys;     // keys[i] holds values[i]; in key order
    std::vector<Value>              values;
    bool operator==(const FieldData&) const = default;
};

// s009: where a table's box sits in the Graph -- kept in the SOLUTION (the
// .acorn file), never in the database. A step like any other, so Ctrl+Z
// moves it back; absent = not placed (the automatic layout places it).
struct BoxDef {
    std::string table;
    double      x = 0, y = 0;
    bool operator==(const BoxDef&) const = default;
};

// s010: a layout -- also kept in the solution, never the database. Its own
// table and name say which (core::Layout).
// s014a: a calculation field -- in the solution too (core::Calc).
// s014b: a summary -- in the solution too (core::Summary).
using Record = std::variant<TableDef, RowDef, FieldData, BoxDef, core::Layout, core::Calc, core::Summary>;

// ── the DDL writer: one place, per dialect, shared by preview and act ─────────
// The New Table window shows exactly the string the store will run.
std::string decl_for(Dialect d, FieldType t);          // FieldType::Text -> "TEXT" / "VARCHAR(255)" / "text"
std::string create_table_sql(Dialect d, char quote, const TableDef& t);   // links written in (s008)
// s008: "FOREIGN KEY ("customer_id") REFERENCES "customers" ("id") ON DELETE RESTRICT"
std::string link_sql(char quote, const LinkDef& l);
std::string drop_table_sql(char quote, const std::string& name);
// A name Acorn will make: not empty, no leading / trailing space, no control
// characters, at most 64 bytes (MariaDB's limit; Postgres truncates at 63 --
// we refuse instead). "" = fine, else the reason in words.
std::string check_name(const std::string& what, const std::string& name);
// The reason this def can't be made, in words ("" = fine): its name, each
// column's name, duplicate names, no columns.
std::string check_new_table(const TableDef& t, const Schema& existing);

// s007 -- the designer's SQL. The statements that turn the table `live` into
// `want`, in order -- what the designer previews and exactly what put() runs.
// One step changes the fields one way: a field whose name alone changed is a
// RENAME (one field gone + one come, otherwise identical); new fields are
// added at the end; gone fields are dropped; indexes no longer wanted are
// dropped first and new ones made last; a field's type / required / default
// change (s007c) and the key (s007d: a step of its own) as described at
// their verbs. Refusals come back in words. MariaDB gets ONE
// ALTER TABLE (it can't roll DDL back, so one statement is the step).
// s007e: a field that comes back before fields the table keeps (undo of
// Delete Field) goes in its place -- SQLite by making the table again when
// `in_place` (the caller has asked the database: no trigger / view / link /
// rule the rebuild would lose), else ADD COLUMN at the end; MariaDB ADD
// COLUMN .. AFTER / FIRST; PostgreSQL at the end.
std::expected<std::vector<std::string>, std::string>
plan_alter(Dialect d, char quote, const TableDef& live, const TableDef& want, bool in_place = true);
std::string create_index_sql(Dialect d, char quote, const std::string& table, const IndexDef& ix);
std::string drop_index_sql(Dialect d, char quote, const std::string& table, const std::string& index);
// Every index name in the schema (SQLite and PostgreSQL keep them per database,
// not per table -- a new index must not reuse one).
bool index_name_taken(const Schema& s, const std::string& name);

// ── the gate ──────────────────────────────────────────────────────────────────
// True for statements that only read (SELECT, WITH .. SELECT is NOT counted as
// a read -- Postgres lets WITH write; SHOW, EXPLAIN, DESCRIBE, PRAGMA without
// '='). Acorn's own reads; user SQL is not run through here in s005.
bool is_read(const std::string& sql);

class Gate final : public Connection {
public:
    explicit Gate(std::unique_ptr<Connection> inner) : m_inner(std::move(inner)) {}
    std::expected<Result, Error> query(const std::string& sql, const std::vector<Value>& params = {}) override;
    std::expected<Schema, Error> introspect() override { return m_inner->introspect(); }
    bool read_only() const override { return m_inner->read_only(); }
    std::string quote_ident(const std::string& n) const override { return m_inner->quote_ident(n); }

    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    Connection& inner() { return *m_inner; }

private:
    std::unique_ptr<Connection> m_inner;
    std::function<bool()>       m_door;
    int                         m_strays = 0;
};

// ── the store the journal writes through ──────────────────────────────────────
class Store {
public:
    using Id    = std::string;
    using Value = Record;

    Store(Connection& c, Dialect d, const Capabilities& caps) : m_conn(c), m_dialect(d), m_caps(caps) {}

    static Id table_id(const std::string& name) { return "table:" + name; }
    // A record by its key: the key's fields and their values, in key order. ""
    // when a value can't be a key Acorn writes by (NULL, a blob), or the
    // fields and values don't pair up.
    static Id row_id(const std::string& table, const std::vector<std::string>& key_fields,
                     const std::vector<db::Value>& key);
    // Back again: false when `id` is not a well-formed row id.
    static bool parse_row_id(const Id& id, std::string& table, std::vector<db::Value>& key);
    static bool parse_row_id(const Id& id, std::string& table, std::vector<std::string>& key_fields,
                             std::vector<db::Value>& key);
    // What a step that MAKES a record in `table` declares (the record's own id
    // is not known until the engine assigns its key).
    static Id new_row_id(const std::string& table) { return "new:" + table; }
    // One field's values (s007): what Delete Field declares to keep them.
    static Id field_id(const std::string& table, const std::string& column);
    static bool parse_field_id(const Id& id, std::string& table, std::string& column);
    // s009: a table's box in the Graph -- a record of the solution, not the database.
    static Id box_id(const std::string& table) { return "box:" + table; }
    static bool is_box(const Id& id) { return id.rfind("box:", 0) == 0; }
    // s010: a layout, by table and name -- "layout:<bytes in table>:<table>/<name>"
    // (the length lets either hold a '/').
    static Id layout_id(const std::string& table, const std::string& name);
    static bool parse_layout_id(const Id& id, std::string& table, std::string& name);
    static bool is_layout(const Id& id) { return id.rfind("layout:", 0) == 0; }
    // A record of the solution, not the database: a step of only these needs
    // no transaction and works on a read-only connection.
    static bool is_local(const Id& id) { return is_box(id) || is_layout(id) || is_calc(id) || is_summary(id); }
    // s014b: a summary, by table and name -- "summary:<bytes in table>:<table>/<name>".
    static Id summary_id(const std::string& table, const std::string& name);
    static bool parse_summary_id(const Id& id, std::string& table, std::string& name);
    static bool is_summary(const Id& id) { return id.rfind("summary:", 0) == 0; }
    // s014a: a calculation field, by table and name -- "calc:<bytes in table>:<table>/<name>".
    static Id calc_id(const std::string& table, const std::string& name);
    static bool parse_calc_id(const Id& id, std::string& table, std::string& name);
    static bool is_calc(const Id& id) { return id.rfind("calc:", 0) == 0; }
    // The solution the box / layout records live in, and how to save it
    // (false: not saved -- the step is refused). None set: such a step is refused.
    void set_solution(core::Solution* s, std::function<bool()> save) { m_solution = s; m_save_solution = std::move(save); }
    const core::Solution* solution() const { return m_solution; }   // s014a: the calc verbs check against it
    // "“customers” record 7" -- for messages.
    static std::string describe(const Id& id);

    // The journal's contract.
    std::optional<Record> get(const Id& id) const;
    void put(const Id& id, const std::optional<Record>& v);

    // Make a record in `table` from the given fields (the rest take their
    // defaults; a counting key is assigned by the engine). The step must have
    // declared new_row_id(table). Returns the made record's id ("" + failed()
    // if it was refused); the record joins the step.
    Id insert(const std::string& table, const std::vector<std::string>& columns,
              const std::vector<db::Value>& values);
    std::vector<Id> take_made() { return std::exchange(m_made, {}); }

    // The table a row id names, as the live schema has it this step (nullptr:
    // no such table). Why a table can't take record writes, in words ("" =
    // it can): a view, no primary key, a key Acorn can't address.
    const Table* table_info(const std::string& name) const;
    static std::string why_not_writable(const Table& t);
    void forget_schema() const { m_schema.reset(); }

    // Fallible: a put that did not happen says why. The journal keeps no step.
    bool         failed() const { return m_error.has_value(); }
    const Error& error() const { return *m_error; }
    void         clear_failed() { m_error.reset(); }
    void         refuse(Error e) const { if (!m_error) m_error = std::move(e); }
    // Make the step's writes final -- the journal's COMMIT (core::Journal
    // calls this after a step's writes). false = they did not stick.
    bool settle() { return m_settle ? m_settle() : true; }
    void set_settle(std::function<bool()> f) { m_settle = std::move(f); }

    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    Dialect dialect() const { return m_dialect; }
    const Capabilities& caps() const { return m_caps; }
    Connection& connection() const { return m_conn; }
    // The records the running step declared; put() refuses any other.
    void set_allowed(std::vector<Id> ids) { m_allowed = std::move(ids); }

private:
    void fail(Error::Code c, std::string msg) const { refuse(Error{c, std::move(msg)}); }
    bool exec(const std::string& sql);
    bool door_and_declared(const Id& id);
    std::optional<Record> get_table(const std::string& name) const;
    std::optional<Record> get_row(const Id& id) const;
    void put_table(const Id& id, const std::optional<TableDef>& v);
    void put_row(const Id& id, const RowDef* v);
    std::optional<Record> get_field(const Id& id) const;
    void put_field(const Id& id, const FieldData* v);
    void put_local(const Id& id, const std::optional<Record>& v);   // s009 / s010: the solution
    bool exec_all(const std::vector<std::string>& sql);
    std::int64_t count_where(const std::string& table, const std::string& column, const char* cond) const;   // -1: failed
    std::vector<std::string> primary_key_of(const std::string& table) const;
    bool write_back(const std::string& table, const ColumnDef& from, const ColumnDef& to, const FieldData& before);
    std::string where_key(const Table& t) const;
    bool same_key(const std::string& table, const Table& t, const std::vector<std::string>& named) const;

    Connection&           m_conn;
    Dialect               m_dialect;
    Capabilities          m_caps;
    mutable std::optional<Schema> m_schema;   // this step's view; forgotten at each step
    std::vector<Id>       m_made;
    mutable std::optional<Error> m_error;
    std::function<bool()> m_door;
    std::function<bool()> m_settle;
    std::vector<Id>       m_allowed;
    int                   m_strays = 0;
    core::Solution*       m_solution = nullptr;   // s009
    std::function<bool()> m_save_solution;
};

// ── the journal ───────────────────────────────────────────────────────────────
class Journal {
public:
    using Core = core::Journal<Store>;

    // `gate` is the connection the UI holds; the journal opens its door.
    Journal(Gate& gate, Dialect d, const Capabilities& caps);
    ~Journal();   // shuts the gate's door for good
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    // THE ONE DOOR. `op` writes through the store (store.put). One step, in one
    // transaction where the engine can roll back. On any failure: rolled back,
    // no step, the reason returned.
    std::expected<void, Error> run(const std::string& label, const std::vector<Store::Id>& ids,
                                   const std::function<void(Store&)>& op);

    // The label of what was undone / redone. Refused (nothing touched) if the
    // database no longer looks the way the step left it.
    std::expected<std::string, Error> undo();
    std::expected<std::string, Error> redo();

    bool        can_undo() const { return m_core.can_undo(); }
    bool        can_redo() const { return m_core.can_redo(); }
    std::string undo_label() const { return m_core.undo_label(); }
    std::string redo_label() const { return m_core.redo_label(); }
    std::size_t size() const { return m_core.size(); }
    void        clear() { m_core.clear(); }
    void        set_on_change(std::function<void()> f) { m_core.set_on_change(std::move(f)); }

    bool   open() const { return m_txn || m_core.open(); }
    Store& store() { return m_store; }
    int    strays() const { return m_gate.strays() + m_store.strays(); }

private:
    std::expected<void, Error> begin(const std::vector<Store::Id>& ids);
    bool commit();
    void rollback();
    std::expected<void, Error> verify(const Core::Step& st, bool undoing);

    Gate&        m_gate;
    Dialect      m_dialect;
    Capabilities m_caps;
    Store        m_store;
    Core         m_core;
    bool         m_txn = false;    // inside run / undo / redo: the door is open
    bool         m_in_txn = false; // a transaction is open on the connection
};

// ── the verbs: every write the UI offers ──────────────────────────────────────
// Each goes through Journal::run. The selftest walks verbs() on every engine:
// a step is kept, nothing strays, undo puts it back exactly, redo reproduces
// it. A button whose verb is not in this table is the gap the gate can't see --
// add it here in the same change that adds the button (RULES: undo from the
// first write).
namespace verbs {
// New Table: name + columns, created empty. Label: New Table “name”.
std::expected<void, Error> new_table(Journal& j, const TableDef& t);

// s006 -- records. New Record: the given fields (may be none: every column its
// default). Returns the made record's id. Label: New Record in “table”.
std::expected<Store::Id, Error> new_record(Journal& j, const std::string& table,
                                           const std::vector<std::string>& columns = {},
                                           const std::vector<Value>& values = {});
// One field of one record. Setting it to what it already holds is no step.
// A key field is refused. Label: Edit “column”.
std::expected<void, Error> set_field(Journal& j, const Store::Id& row, const std::string& column,
                                     const Value& v);
// Label: Delete Record from “table”. s008: records of other tables pointing
// at it -- a Restrict link refuses in words ("2 “orders” point at this
// record -- delete them first, or change the link"); Cascade deletes them and
// Set Empty empties their link fields, IN the step (declared, so Ctrl+Z puts
// every one back), never by the engine behind the journal's back.
std::expected<void, Error> delete_record(Journal& j, const Store::Id& row);

// s007 -- the table designer. Each is one step; undo puts the table back.
// New Field “c” in “t”: added at the end, NULL in every record (a required
// field needs a default). Not a key field (s007c).
std::expected<void, Error> new_field(Journal& j, const std::string& table, const ColumnDef& c);
// Rename Field “a” to “b”: its values, indexes and links go with it.
std::expected<void, Error> rename_field(Journal& j, const std::string& table, const std::string& from,
                                        const std::string& to);
// Delete Field “c” from “t”: the indexes on it go too; its values are KEPT in
// the step, so undo puts them back. Never a key field.
std::expected<void, Error> delete_field(Journal& j, const std::string& table, const std::string& column);
// New Index “i” on “t” / Delete Index “i” from “t”.
std::expected<void, Error> new_index(Journal& j, const std::string& table, const IndexDef& ix);
std::expected<void, Error> delete_index(Journal& j, const std::string& table, const std::string& index);
// s007b -- the designer window: every change it holds, as ONE step. Fields
// it drops that hold values are kept in the step (as delete_field does).
// Label: the one change's own name when there is one change ("Rename Field
// “a” to “b”"), else Change Table “t”. Pictures carry no field identity, so
// one field gone + one identical field come IS a rename; deleting one field
// and adding another of the same kind in one step is refused (two steps) --
// the designer window keeps them apart before it gets here.
std::expected<void, Error> design_table(Journal& j, const TableDef& want);
std::string design_label(const TableDef& live, const TableDef& want);
// What the change takes with it, in words -- one line per field that holds
// values ("“notes” holds 37 values -- they go with the field"), and (s007c)
// per changed field: values that won't fit its new kind ("2 of “age”'s 5
// values won't fit a whole number -- they become empty"), empty records a
// new "required" fills ("3 records have no “name” -- they take the default
// “none”"). Empty: nothing is lost. The designer never pre-ticks its "I
// understand". Refused (in words): required with empties and no default;
// on MariaDB any emptying or filling (it can't roll the ALTER back).
std::expected<std::vector<std::string>, Error> design_losses(Journal& j, const TableDef& want);
// s007c -- Change Field “c” in “t”: `c` (by name) as the field should be --
// its type, required, default. Values are converted by Acorn's rules
// (db/Convert) and KEPT in the step, so undo puts back exactly what was there.
// Not the key (s007d). A design_table step of one change.
std::expected<void, Error> change_field(Journal& j, const std::string& table, const ColumnDef& c);
// s007d -- Change Key of “t”: records are found by `fields` (in key order)
// from now on; `counting` makes a lone whole-number key count up (or stop).
// Fields joining the key become required; fields leaving it stay, required,
// values intact. Refused in words: a field empty in some record, values shared
// by two records, a counting key that isn't one whole-number field (SQLite: a
// lone INTEGER key always counts). The key changes in a step of its own.
// Label: Change Key of “t” to “a”, “b” -- or Make “id” Count Up / Stop “id”
// Counting Up when only the counting changes.
std::expected<void, Error> change_key(Journal& j, const std::string& table, const std::vector<std::string>& fields,
                                      bool counting);
// The table as change_key would make it (the designer builds its picture
// with this, so the window and the verb agree). Refused: no such field.
std::expected<TableDef, std::string> keyed(const TableDef& live, const std::vector<std::string>& fields, bool counting);
// s008 -- links. Each is a step on the CHILD table (the one that points);
// the parent is only read. SQLite makes the child again (it can't add a link
// any other way) -- refused in words where that would lose something (a
// trigger, a view, a table that links to the child, a link to itself);
// PostgreSQL ADD / DROP CONSTRAINT; MariaDB one ALTER TABLE.
// Refused in words, before anything runs: the parent has no key, the fields
// don't match its key in number or kind, Set Empty on a required field, and
// records that point at a parent record that isn't there ("3 records in
// “orders” point at a “customers” record that isn't there -- fix them
// first"; unmatched() names them, so they can be marked). Nothing is ever
// deleted or emptied to make a link fit.
// A link changes in a step of its own. Label: Link “orders” to “customers”.
std::expected<void, Error> new_link(Journal& j, const std::string& table, const std::vector<std::string>& fields,
                                    const std::string& parent, OnDelete on_delete = OnDelete::Restrict);
// Unlink “orders” from “customers”: the link by its fields.
std::expected<void, Error> delete_link(Journal& j, const std::string& table, const std::vector<std::string>& fields);
// What deleting a parent record does to the records pointing at it.
// Label: Change Link “orders” to “customers”.
std::expected<void, Error> change_link(Journal& j, const std::string& table, const std::vector<std::string>& fields,
                                       OnDelete on_delete);
// The table as new_link would make it (the graph window builds its picture
// with this, so window and verb agree). Refused: no such field / parent.
std::expected<TableDef, std::string> linked(const TableDef& live, const Table& parent,
                                            const std::vector<std::string>& fields, OnDelete on_delete);
// The records of `table` whose `fields` point at a `parent` record that isn't
// there (all fields filled, no parent record with that key) -- by record id,
// in key order, at most `limit`. A read; no step.
std::expected<std::vector<Store::Id>, Error> unmatched(Journal& j, const std::string& table,
                                                       const std::vector<std::string>& fields,
                                                       const std::string& parent, std::size_t limit = 1000);

// s009 -- Move “t”: the table's box to (x, y) in the Graph. Kept in the
// solution, not the database: works on a read-only connection too. Moving it
// where it already is: no step.
std::expected<void, Error> move_box(Journal& j, const std::string& table, double x, double y);

// s010 -- layouts, kept in the solution like the boxes (read-only connections
// too). Save Layout puts `l` under its table and name: "New Layout “n”" when
// there was none by that name, else "Change Layout “n”". The layout editor
// (s010b) sends every edit through it, so each is a step on the same Ctrl+Z
// list. Saving what is already kept: no step. Refused in words: a bad name,
// an object with no place or size (core::check_layout).
std::expected<void, Error> save_layout(Journal& j, const core::Layout& l);
// Delete Layout “n”. Refused in words when there's none by that name.
std::expected<void, Error> delete_layout(Journal& j, const std::string& table, const std::string& name);
// s010b -- Rename Layout “a” to “b”: one step (the old name gone, the new one
// kept). Refused in words: no layout “a”, a layout “b” already there, a bad
// name. The same name: no step.
std::expected<void, Error> rename_layout(Journal& j, const std::string& table, const std::string& from,
                                         const std::string& to);

// s014a -- calculation fields, kept in the solution (read-only connections
// too). Save Calculation keeps `c` under its table and name: "New Calculation
// “n”" when there was none, else "Change Calculation “n”". `was` (a name the
// table has a calculation by) makes it a rename in the same step: "Change
// Calculation “was”" -- the old name gone, `c` kept. Refused in words: no
// such table, anything core::check_calc refuses (a bad name, a field's name,
// an expression that doesn't parse, a name it reads that the table hasn't, a
// loop), a calculation by the new name already there. The same: no step.
std::expected<void, Error> save_calc(Journal& j, const core::Calc& c, const std::string& was = {});
// Delete Calculation “n”. Refused in words when there's none by that name.
// Calculations and layouts that read it are left as they are (they show "?"
// and say why, until it comes back -- Ctrl+Z).
std::expected<void, Error> delete_calc(Journal& j, const std::string& table, const std::string& name);

// s014b -- summaries, kept in the solution (read-only connections too). Save
// Summary keeps `s`: "New Summary “n”" / "Change Summary “n”" (`was`: a new
// name in the same step, as save_calc). Refused in words: no such table,
// anything core::check_summary refuses, a name a calculation or another
// summary has, Total / Average of a field that isn't a number, any summary
// of a binary field. The same: no step.
std::expected<void, Error> save_summary(Journal& j, const core::Summary& s, const std::string& was = {});
// Delete Summary “n”. Refused in words when there's none by that name.
std::expected<void, Error> delete_summary(Journal& j, const std::string& table, const std::string& name);

// What a design step would do to the live table, as the SQL put() will run
// (the designer's preview): `want` is the table as the designer has it.
std::expected<std::vector<std::string>, Error> preview(Journal& j, const TableDef& want);

struct Verb {
    std::string name;
    // Run the verb once on a fixture database (any engine).
    std::function<std::expected<void, Error>(Journal&)> run;
    // What the fixture needs first (a table, a record) -- run through the door
    // before the walk counts. Optional.
    std::function<std::expected<void, Error>(Journal&)> setup = {};
    bool local = false;   // s009: changes the solution, not the database (the walk checks the solution)
};
const std::vector<Verb>& all();
}  // namespace verbs

}  // namespace acorn::db
