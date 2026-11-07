// acorn-engine-sqlite -- the SQLite engine plugin. Links only libsqlite3 (and
// acorn_db for the shared value / schema / type-map code).
//
// Read-only is SQLite's own: SQLITE_OPEN_READONLY plus PRAGMA query_only, so a
// write is refused by the engine, not by Acorn's good manners. A read-only
// open of a missing file fails (it never creates one).
#include "db/Engine.hpp"
#include "db/Plugin.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

namespace acorn::db::sqlite {
namespace {

Error make_error(sqlite3* h, int rc, const std::string& context = {}) {
    Error e;
    switch (rc & 0xff) {
    case SQLITE_READONLY:   e.code = Error::Code::ReadOnly; break;
    case SQLITE_CONSTRAINT: e.code = Error::Code::Constraint; break;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:     e.code = Error::Code::Busy; break;
    case SQLITE_CANTOPEN:   e.code = Error::Code::Open; break;
    case SQLITE_NOTADB:     e.code = Error::Code::Open; break;
    case SQLITE_ERROR:      e.code = Error::Code::Syntax; break;
    default:                e.code = Error::Code::Internal; break;
    }
    e.message = h ? sqlite3_errmsg(h) : sqlite3_errstr(rc);
    if (!context.empty()) e.message = context + ": " + e.message;
    return e;
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() { if (s) sqlite3_finalize(s); }
};

bool only_space(const char* p) {
    for (; p && *p; ++p) {
        if (*p == ';') continue;
        if (!std::isspace(static_cast<unsigned char>(*p))) return false;
    }
    return true;
}

bool starts_ci(const std::string& sql, std::string_view word) {
    std::size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
    if (sql.size() - i < word.size()) return false;
    for (std::size_t k = 0; k < word.size(); ++k)
        if (std::tolower(static_cast<unsigned char>(sql[i + k])) != word[k]) return false;
    return true;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class SqliteConnection final : public Connection {
public:
    SqliteConnection(sqlite3* h, bool ro) : h_(h), ro_(ro) {}
    ~SqliteConnection() override { sqlite3_close_v2(h_); }

    bool read_only() const override { return ro_; }
    std::string quote_ident(const std::string& n) const override { return quote_ident_with('"', n); }

    std::expected<Result, Error>
    query(const std::string& sql, const std::vector<Value>& params) override {
        Stmt st;
        const char* tail = nullptr;
        int rc = sqlite3_prepare_v2(h_, sql.c_str(), static_cast<int>(sql.size()), &st.s, &tail);
        if (rc != SQLITE_OK) return std::unexpected(make_error(h_, rc));
        if (!st.s) return Result{};                       // empty / comment-only
        if (!only_space(tail))
            return std::unexpected(Error{Error::Code::Syntax,
                                         "one statement per call (found more after the first)"});

        const int want = sqlite3_bind_parameter_count(st.s);
        if (want != static_cast<int>(params.size()))
            return std::unexpected(Error{Error::Code::Syntax,
                "statement has " + std::to_string(want) + " parameter(s), " +
                std::to_string(params.size()) + " given"});
        for (int i = 0; i < want; ++i) {
            const Value& v = params[static_cast<std::size_t>(i)];
            int b = SQLITE_OK;
            switch (v.kind()) {
            case Value::Kind::Null:    b = sqlite3_bind_null(st.s, i + 1); break;
            case Value::Kind::Integer: b = sqlite3_bind_int64(st.s, i + 1, *v.as_int()); break;
            case Value::Kind::Real:    b = sqlite3_bind_double(st.s, i + 1, *v.as_real()); break;
            case Value::Kind::Text:
                b = sqlite3_bind_text64(st.s, i + 1, v.as_text()->data(), v.as_text()->size(),
                                        SQLITE_TRANSIENT, SQLITE_UTF8);
                break;
            case Value::Kind::Blob:
                b = sqlite3_bind_blob64(st.s, i + 1, v.as_blob()->data(), v.as_blob()->size(),
                                        SQLITE_TRANSIENT);
                break;
            }
            if (b != SQLITE_OK) return std::unexpected(make_error(h_, b, "bind"));
        }

        Result r;
        const int ncol = sqlite3_column_count(st.s);
        for (int c = 0; c < ncol; ++c) {
            const char* dt = sqlite3_column_decltype(st.s, c);
            r.columns.push_back({sqlite3_column_name(st.s, c), dt ? dt : ""});
        }
        for (;;) {
            rc = sqlite3_step(st.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) return std::unexpected(make_error(h_, rc));
            Row row;
            row.reserve(static_cast<std::size_t>(ncol));
            for (int c = 0; c < ncol; ++c) {
                switch (sqlite3_column_type(st.s, c)) {
                case SQLITE_INTEGER: row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(st.s, c))); break;
                case SQLITE_FLOAT:   row.emplace_back(sqlite3_column_double(st.s, c)); break;
                case SQLITE_TEXT: {
                    auto p = reinterpret_cast<const char*>(sqlite3_column_text(st.s, c));
                    row.emplace_back(std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(st.s, c))));
                    break;
                }
                case SQLITE_BLOB: {
                    auto p = static_cast<const std::uint8_t*>(sqlite3_column_blob(st.s, c));
                    auto n = static_cast<std::size_t>(sqlite3_column_bytes(st.s, c));
                    row.emplace_back(Blob(p, p + n));
                    break;
                }
                default: row.emplace_back(); break;
                }
            }
            r.rows.push_back(std::move(row));
        }
        if (!sqlite3_stmt_readonly(st.s)) {
            r.affected = sqlite3_changes64(h_);
            if ((starts_ci(sql, "insert") || starts_ci(sql, "replace")) && r.affected > 0)
                r.last_insert_id = sqlite3_last_insert_rowid(h_);
        }
        return r;
    }

    std::expected<Schema, Error> introspect() override {
        Schema s;
        auto objs = query(
            "SELECT name, type FROM sqlite_master "
            "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' "
            "ORDER BY type = 'view', name", {});
        if (!objs) return std::unexpected(objs.error());

        for (auto& o : objs->rows) {
            Table t;
            t.name = *o[0].as_text();
            t.kind = (*o[1].as_text() == "view") ? Table::Kind::View : Table::Kind::Table;

            auto cols = query(
                "SELECT name, type, \"notnull\", dflt_value, pk, hidden "
                "FROM pragma_table_xinfo(?) ORDER BY cid", {t.name});
            if (!cols) return std::unexpected(cols.error());
            for (auto& c : cols->rows) {
                if (c[5].as_int().value_or(0) == 1) continue;   // virtual-table hidden column
                Column col;
                col.name = *c[0].as_text();
                col.decl_type = c[1].as_text() ? *c[1].as_text() : "";
                col.type = map_type(Dialect::SQLite, col.decl_type);
                col.nullable = c[2].as_int().value_or(0) == 0;
                if (auto d = c[3].as_text()) col.default_sql = *d;
                col.pk_position = static_cast<int>(c[4].as_int().value_or(0));
                t.columns.push_back(std::move(col));
            }

            if (t.kind == Table::Kind::Table) {
                // Indexes. origin: 'c' CREATE INDEX, 'u' UNIQUE constraint, 'pk' PRIMARY KEY.
                auto idx = query(
                    "SELECT name, \"unique\", origin, partial FROM pragma_index_list(?) ORDER BY name",
                    {t.name});
                if (!idx) return std::unexpected(idx.error());
                bool pk_index = false;
                for (auto& i : idx->rows) {
                    Index ix;
                    ix.name = *i[0].as_text();
                    ix.unique = i[1].as_int().value_or(0) != 0;
                    ix.primary = *i[2].as_text() == "pk";
                    ix.partial = i[3].as_int().value_or(0) != 0;
                    pk_index = pk_index || ix.primary;
                    auto parts = query(
                        "SELECT name FROM pragma_index_info(?) ORDER BY seqno", {ix.name});
                    if (!parts) return std::unexpected(parts.error());
                    for (auto& p : parts->rows)
                        ix.columns.push_back(p[0].as_text() ? *p[0].as_text() : "<expr>");
                    t.indexes.push_back(std::move(ix));
                }

                // A rowid alias -- the one INTEGER PRIMARY KEY of a rowid table --
                // is SQLite's auto-increment. A WITHOUT ROWID table has a 'pk' index
                // instead, and its INTEGER key is not an alias.
                auto pk = t.primary_key();
                if (pk.size() == 1 && !pk_index) {
                    for (auto& c : t.columns)
                        if (c.name == pk[0] && upper(c.decl_type) == "INTEGER")
                            c.auto_increment = true;
                }

                auto fks = query(
                    "SELECT id, \"table\", \"from\", \"to\", on_update, on_delete "
                    "FROM pragma_foreign_key_list(?) ORDER BY id, seq", {t.name});
                if (!fks) return std::unexpected(fks.error());
                std::map<std::int64_t, ForeignKey> by_id;
                for (auto& f : fks->rows) {
                    auto& fk = by_id[*f[0].as_int()];
                    fk.ref_table = *f[1].as_text();
                    fk.columns.push_back(*f[2].as_text());
                    if (auto to = f[3].as_text()) fk.ref_columns.push_back(*to);
                    fk.on_update = *f[4].as_text();
                    fk.on_delete = *f[5].as_text();
                }
                for (auto& [id, fk] : by_id) t.foreign_keys.push_back(std::move(fk));
            }
            s.tables.push_back(std::move(t));
        }
        return s;
    }

private:
    sqlite3* h_;
    bool ro_;
};

class SqliteEngine final : public Engine {
public:
    std::string id() const override { return "sqlite"; }
    std::string name() const override { return "SQLite"; }
    std::string version() const override { return sqlite3_libversion(); }
    Dialect dialect() const override { return Dialect::SQLite; }
    Capabilities caps() const override {
        Capabilities c;
        c.needs_server = false;
        c.transactional_ddl = true;
        c.returning = sqlite3_libversion_number() >= 3035000;
        c.schemas = false;
        c.users_privileges = false;
        c.strict_types = false;
        c.named_foreign_keys = false;
        c.partial_indexes = true;
        c.ident_quote = '"';
        c.autoincrement = "AUTOINCREMENT";
        return c;
    }

    std::expected<std::unique_ptr<Connection>, Error> open(const ConnectParams& p) override {
        if (p.path.empty())
            return std::unexpected(Error{Error::Code::Open, "no database file given"});
        std::error_code ec;
        const bool exists = std::filesystem::exists(p.path, ec);
        if (!exists && !p.create)
            return std::unexpected(Error{Error::Code::NotFound, "no such file: " + p.path});

        const bool ro = p.read_only && !p.create;
        int flags = ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
        if (p.create) flags |= SQLITE_OPEN_CREATE;
        flags |= SQLITE_OPEN_NOMUTEX;   // one connection, one thread at a time

        sqlite3* h = nullptr;
        int rc = sqlite3_open_v2(p.path.c_str(), &h, flags, nullptr);
        if (rc != SQLITE_OK) {
            Error e = make_error(h, rc, p.path);
            sqlite3_close_v2(h);
            return std::unexpected(e);
        }
        sqlite3_extended_result_codes(h, 1);
        sqlite3_busy_timeout(h, 2000);

        auto conn = std::make_unique<SqliteConnection>(h, ro);
        // Make sure it really is a database (sqlite opens anything lazily), and
        // turn on what Acorn relies on.
        if (auto r = conn->query("SELECT count(*) FROM sqlite_master", {}); !r)
            return std::unexpected(Error{Error::Code::Open, p.path + ": " + r.error().message});
        if (ro) {
            if (auto r = conn->query("PRAGMA query_only = 1", {}); !r) return std::unexpected(r.error());
        } else {
            if (auto r = conn->query("PRAGMA foreign_keys = ON", {}); !r) return std::unexpected(r.error());
        }
        return conn;
    }
};

Engine* create() { return new SqliteEngine; }
void destroy(Engine* e) { delete e; }

const AcornEngineEntry kEntry{ACORN_ENGINE_ABI, "sqlite", &create, &destroy};

} // namespace
} // namespace acorn::db::sqlite

ACORN_EXPORT const AcornEngineEntry* acorn_engine_entry(void) {
    return &acorn::db::sqlite::kEntry;
}
