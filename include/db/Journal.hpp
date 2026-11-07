// db/Journal.hpp -- THE DOOR (REQUIREMENTS §1.5, §6 s005): every write Acorn
// makes to a database goes through db::Journal::run -- preview, act in a
// transaction, record, undo.
//
// It is core::Journal (docs/undo.md -- "a step is two pictures, not an
// inverse") over a database-shaped store:
//
//     Id    "table:<name>"            (s006 adds "row:<table>:<key>")
//     Value TableDef -- the table's shape as introspection reads it, plus its
//           row count. Absent = no such table.
//
// Store::put turns the pictures into DDL: absent -> present is CREATE TABLE,
// present -> absent is DROP TABLE. So New Table is undoable the day it goes
// through run(), with no hand-written inverse. Two safety rules sit in put()
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
// the statement back (SQLite, PostgreSQL: transactional DDL). MariaDB commits
// DDL implicitly, so there a step may declare ONE table -- one statement,
// atomic by itself -- and a step that declares more is refused before it runs.
// On every engine a step writes only the records it declared.
#pragma once

#include "core/Undo.hpp"
#include "db/Engine.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
    using Value = TableDef;

    Store(Connection& c, Dialect d) : m_conn(c), m_dialect(d) {}

    static Id table_id(const std::string& name) { return "table:" + name; }

    // The journal's contract.
    std::optional<TableDef> get(const Id& id) const;
    void put(const Id& id, const std::optional<TableDef>& v);

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
    // The records the running step declared; put() refuses any other.
    void set_allowed(std::vector<Id> ids) { m_allowed = std::move(ids); }

private:
    void fail(Error::Code c, std::string msg) const { refuse(Error{c, std::move(msg)}); }
    bool exec(const std::string& sql);

    Connection&           m_conn;
    Dialect               m_dialect;
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
    std::expected<void, Error> begin();
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

struct Verb {
    std::string name;
    // Run the verb once on a fixture database (any engine).
    std::function<std::expected<void, Error>(Journal&)> run;
};
const std::vector<Verb>& all();
}  // namespace verbs

}  // namespace acorn::db
