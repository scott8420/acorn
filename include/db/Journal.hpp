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
//                                          primary key's values, encoded
//     Value Record = TableDef | RowDef. TableDef: the table's shape as
//           introspection reads it, plus its row count. RowDef: every field of
//           one record, by column. Absent = no such table / record.
//
// Store::put turns the pictures into SQL: a table absent -> present is CREATE
// TABLE, present -> absent DROP TABLE; a record absent -> present is INSERT
// (every field, the key included), present -> absent DELETE, changed UPDATE
// (only the fields that differ). So New Table, New Record, a field edit and
// Delete Record are undoable the day they go through run(), with no
// hand-written inverse. A record is found by its PRIMARY KEY: a table without
// one is browsed, never written; a key field is never changed (s006). Two safety rules sit in put()
// and in undo / redo, because a database can change behind Acorn's back:
//
//   * put() never drops a table that holds rows, and never ALTERs (that is the
//     table designer, s007) -- it refuses, and the step does not happen;
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

struct TableDef {
    std::string            name;
    std::vector<ColumnDef> columns;
    std::int64_t           rows = 0;          // live count -- a step never drops data
    bool operator==(const TableDef&) const = default;

    static TableDef from(const Table& t);     // introspection -> picture (rows = 0)
};

// One record: every column of its table, in the table's order (s006).
struct RowDef {
    std::string              table;
    std::vector<std::string> columns;
    std::vector<Value>       values;          // values[i] is columns[i]
    bool operator==(const RowDef&) const = default;

    const Value* field(const std::string& column) const;   // nullptr: no such column
};

using Record = std::variant<TableDef, RowDef>;

// ── the DDL writer: one place, per dialect, shared by preview and act ─────────
// The New Table window shows exactly the string the store will run.
std::string decl_for(Dialect d, FieldType t);          // FieldType::Text -> "TEXT" / "VARCHAR(255)" / "text"
std::string create_table_sql(Dialect d, char quote, const TableDef& t);
std::string drop_table_sql(char quote, const std::string& name);
// A name Acorn will make: not empty, no leading / trailing space, no control
// characters, at most 64 bytes (MariaDB's limit; Postgres truncates at 63 --
// we refuse instead). "" = fine, else the reason in words.
std::string check_name(const std::string& what, const std::string& name);
// The reason this def can't be made, in words ("" = fine): its name, each
// column's name, duplicate names, no columns.
std::string check_new_table(const TableDef& t, const Schema& existing);

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
    // A record by its key values, in key order. "" when a value can't be a key
    // Acorn writes by (NULL, a blob).
    static Id row_id(const std::string& table, const std::vector<db::Value>& key);
    // Back again: false when `id` is not a well-formed row id.
    static bool parse_row_id(const Id& id, std::string& table, std::vector<db::Value>& key);
    // What a step that MAKES a record in `table` declares (the record's own id
    // is not known until the engine assigns its key).
    static Id new_row_id(const std::string& table) { return "new:" + table; }
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
    std::string where_key(const Table& t) const;

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
// Label: Delete Record from “table”.
std::expected<void, Error> delete_record(Journal& j, const Store::Id& row);

struct Verb {
    std::string name;
    // Run the verb once on a fixture database (any engine).
    std::function<std::expected<void, Error>(Journal&)> run;
    // What the fixture needs first (a table, a record) -- run through the door
    // before the walk counts. Optional.
    std::function<std::expected<void, Error>(Journal&)> setup = {};
};
const std::vector<Verb>& all();
}  // namespace verbs

}  // namespace acorn::db
