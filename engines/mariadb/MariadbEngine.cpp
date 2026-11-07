// acorn-engine-mariadb -- MariaDB / MySQL over MariaDB Connector/C (libmariadb,
// LGPL; speaks the MySQL protocol too). Links only libmariadb + acorn_db.
//
// Statements always go through the prepared-statement API, so values are bound
// on the wire and a second statement in one call is a syntax error. Read-only
// is the server's: SET SESSION TRANSACTION READ ONLY refuses DML *and* DDL
// (error 1792) -- verified against MariaDB 10.11 in s003.
//
// Result values: integers -> Integer, FLOAT/DOUBLE -> Real, binary strings /
// BLOBs -> Blob, everything else (DECIMAL, dates, text, ENUM, JSON) -> Text in
// the server's own spelling. DECIMAL stays text on purpose: it is exact.
#include "db/Engine.hpp"
#include "db/Plugin.hpp"

#include <mysql.h>

#include <cctype>
#include <climits>
#include <cstring>
#include <map>

namespace acorn::db::mariadb {
namespace {

constexpr unsigned kBinaryCharset = 63;

Error::Code code_for(unsigned no, const char* sqlstate) {
    switch (no) {
    case 1792: return Error::Code::ReadOnly;           // read-only transaction
    case 1290: return Error::Code::ReadOnly;           // --read-only server
    case 1062: case 1451: case 1452: case 1216: case 1217:
    case 1048: case 1364: case 4025: case 3819:
        return Error::Code::Constraint;
    case 1205: case 1213: return Error::Code::Busy;
    case 1049: return Error::Code::NotFound;           // unknown database
    case 1045: case 1044: case 2002: case 2003: case 2005: case 2006: case 2013:
        return Error::Code::Open;
    default: break;
    }
    if (sqlstate && std::strncmp(sqlstate, "23", 2) == 0) return Error::Code::Constraint;
    if (no >= 2000 && no < 3000) return Error::Code::Internal;   // client-side
    return Error::Code::Syntax;
}

Error conn_error(MYSQL* h, const std::string& ctx = {}) {
    Error e{code_for(mysql_errno(h), mysql_sqlstate(h)), mysql_error(h)};
    if (!ctx.empty()) e.message = ctx + ": " + e.message;
    return e;
}
Error stmt_error(MYSQL_STMT* s) {
    return {code_for(mysql_stmt_errno(s), mysql_stmt_sqlstate(s)), mysql_stmt_error(s)};
}

bool starts_ci(const std::string& sql, std::string_view word) {
    std::size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
    if (sql.size() - i < word.size()) return false;
    for (std::size_t k = 0; k < word.size(); ++k)
        if (std::tolower(static_cast<unsigned char>(sql[i + k])) != word[k]) return false;
    return true;
}

const char* type_name(enum_field_types t) {
    switch (t) {
    case MYSQL_TYPE_TINY: return "tinyint";
    case MYSQL_TYPE_SHORT: return "smallint";
    case MYSQL_TYPE_INT24: return "mediumint";
    case MYSQL_TYPE_LONG: return "int";
    case MYSQL_TYPE_LONGLONG: return "bigint";
    case MYSQL_TYPE_FLOAT: return "float";
    case MYSQL_TYPE_DOUBLE: return "double";
    case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_NEWDECIMAL: return "decimal";
    case MYSQL_TYPE_DATE: case MYSQL_TYPE_NEWDATE: return "date";
    case MYSQL_TYPE_TIME: return "time";
    case MYSQL_TYPE_DATETIME: return "datetime";
    case MYSQL_TYPE_TIMESTAMP: return "timestamp";
    case MYSQL_TYPE_YEAR: return "year";
    case MYSQL_TYPE_BIT: return "bit";
    case MYSQL_TYPE_JSON: return "json";
    case MYSQL_TYPE_ENUM: return "enum";
    case MYSQL_TYPE_SET: return "set";
    case MYSQL_TYPE_TINY_BLOB: case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: case MYSQL_TYPE_BLOB: return "blob";
    case MYSQL_TYPE_VAR_STRING: case MYSQL_TYPE_VARCHAR: return "varchar";
    case MYSQL_TYPE_STRING: return "char";
    default: return "";
    }
}

enum class Fetch { Int, Real, Bytes, Text, Bit };

Fetch fetch_kind(const MYSQL_FIELD& f) {
    switch (f.type) {
    case MYSQL_TYPE_TINY: case MYSQL_TYPE_SHORT: case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG: case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_YEAR:
        return Fetch::Int;
    case MYSQL_TYPE_FLOAT: case MYSQL_TYPE_DOUBLE:
        return Fetch::Real;
    case MYSQL_TYPE_BIT:
        return Fetch::Bit;
    case MYSQL_TYPE_TINY_BLOB: case MYSQL_TYPE_MEDIUM_BLOB: case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB: case MYSQL_TYPE_STRING: case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
        return f.charsetnr == kBinaryCharset ? Fetch::Bytes : Fetch::Text;
    default:
        return Fetch::Text;   // DECIMAL, temporal, JSON, ENUM, SET, GEOMETRY ...
    }
}

struct StmtGuard {
    MYSQL_STMT* s;
    MYSQL_RES*  meta = nullptr;
    ~StmtGuard() { if (meta) mysql_free_result(meta); if (s) mysql_stmt_close(s); }
};

class MariadbConnection final : public Connection {
public:
    MariadbConnection(MYSQL* h, bool ro) : h_(h), ro_(ro) {}
    ~MariadbConnection() override { mysql_close(h_); }

    bool read_only() const override { return ro_; }
    std::string quote_ident(const std::string& n) const override { return quote_ident_with('`', n); }

    std::expected<Result, Error>
    query(const std::string& sql, const std::vector<Value>& params) override {
        StmtGuard g{mysql_stmt_init(h_)};
        if (!g.s) return std::unexpected(conn_error(h_, "stmt_init"));
        if (mysql_stmt_prepare(g.s, sql.c_str(), sql.size()))
            return std::unexpected(stmt_error(g.s));

        const unsigned long want = mysql_stmt_param_count(g.s);
        if (want != params.size())
            return std::unexpected(Error{Error::Code::Syntax,
                "statement has " + std::to_string(want) + " parameter(s), " +
                std::to_string(params.size()) + " given"});

        std::vector<MYSQL_BIND> pb(want);
        std::vector<long long> ints(want);
        std::vector<double> reals(want);
        for (std::size_t i = 0; i < want; ++i) {
            std::memset(&pb[i], 0, sizeof(MYSQL_BIND));
            const Value& v = params[i];
            switch (v.kind()) {
            case Value::Kind::Null:
                pb[i].buffer_type = MYSQL_TYPE_NULL; break;
            case Value::Kind::Integer:
                ints[i] = *v.as_int();
                pb[i].buffer_type = MYSQL_TYPE_LONGLONG; pb[i].buffer = &ints[i]; break;
            case Value::Kind::Real:
                reals[i] = *v.as_real();
                pb[i].buffer_type = MYSQL_TYPE_DOUBLE; pb[i].buffer = &reals[i]; break;
            case Value::Kind::Text:
                pb[i].buffer_type = MYSQL_TYPE_STRING;
                pb[i].buffer = const_cast<char*>(v.as_text()->data());
                pb[i].buffer_length = v.as_text()->size();
                break;
            case Value::Kind::Blob:
                pb[i].buffer_type = MYSQL_TYPE_BLOB;
                pb[i].buffer = const_cast<std::uint8_t*>(v.as_blob()->data());
                pb[i].buffer_length = v.as_blob()->size();
                break;
            }
        }
        if (want && mysql_stmt_bind_param(g.s, pb.data())) return std::unexpected(stmt_error(g.s));
        if (mysql_stmt_execute(g.s)) return std::unexpected(stmt_error(g.s));

        Result r;
        g.meta = mysql_stmt_result_metadata(g.s);
        if (!g.meta) {
            r.affected = static_cast<std::int64_t>(mysql_stmt_affected_rows(g.s));
            auto id = mysql_stmt_insert_id(g.s);
            if (id && (starts_ci(sql, "insert") || starts_ci(sql, "replace")))
                r.last_insert_id = static_cast<std::int64_t>(id);
            return r;
        }

        const unsigned n = mysql_num_fields(g.meta);
        MYSQL_FIELD* fields = mysql_fetch_fields(g.meta);
        std::vector<Fetch> kind(n);
        std::vector<MYSQL_BIND> rb(n);
        std::vector<long long> ri(n);
        std::vector<double> rr(n);
        std::vector<unsigned long> len(n);
        std::vector<my_bool> isnull(n), err(n);
        for (unsigned c = 0; c < n; ++c) {
            r.columns.push_back({fields[c].name, type_name(fields[c].type)});
            kind[c] = fetch_kind(fields[c]);
            std::memset(&rb[c], 0, sizeof(MYSQL_BIND));
            rb[c].is_null = &isnull[c];
            rb[c].error = &err[c];
            rb[c].length = &len[c];
            switch (kind[c]) {
            case Fetch::Int:
                rb[c].buffer_type = MYSQL_TYPE_LONGLONG; rb[c].buffer = &ri[c];
                rb[c].is_unsigned = (fields[c].flags & UNSIGNED_FLAG) != 0;
                break;
            case Fetch::Real:
                rb[c].buffer_type = MYSQL_TYPE_DOUBLE; rb[c].buffer = &rr[c]; break;
            default:   // variable length: learn the length, then fetch_column
                rb[c].buffer_type = (kind[c] == Fetch::Text) ? MYSQL_TYPE_STRING : MYSQL_TYPE_BLOB;
                break;
            }
        }
        if (mysql_stmt_bind_result(g.s, rb.data())) return std::unexpected(stmt_error(g.s));

        for (;;) {
            int rc = mysql_stmt_fetch(g.s);
            if (rc == MYSQL_NO_DATA) break;
            if (rc == 1) return std::unexpected(stmt_error(g.s));
            Row row;
            row.reserve(n);
            for (unsigned c = 0; c < n; ++c) {
                if (isnull[c]) { row.emplace_back(); continue; }
                switch (kind[c]) {
                case Fetch::Int:
                    if (rb[c].is_unsigned && static_cast<unsigned long long>(ri[c]) > LLONG_MAX)
                        row.emplace_back(std::to_string(static_cast<unsigned long long>(ri[c])));
                    else
                        row.emplace_back(static_cast<std::int64_t>(ri[c]));
                    break;
                case Fetch::Real: row.emplace_back(rr[c]); break;
                default: {
                    std::string buf(len[c], '\0');
                    if (len[c]) {
                        MYSQL_BIND one;
                        std::memset(&one, 0, sizeof one);
                        one.buffer_type = rb[c].buffer_type;
                        one.buffer = buf.data();
                        one.buffer_length = len[c];
                        if (mysql_stmt_fetch_column(g.s, &one, c, 0))
                            return std::unexpected(stmt_error(g.s));
                    }
                    if (kind[c] == Fetch::Text) row.emplace_back(std::move(buf));
                    else if (kind[c] == Fetch::Bytes) row.emplace_back(Blob(buf.begin(), buf.end()));
                    else {   // BIT(n): big-endian bytes -> integer
                        std::int64_t v = 0;
                        for (unsigned char ch : buf) v = (v << 8) | ch;
                        row.emplace_back(v);
                    }
                }
                }
            }
            r.rows.push_back(std::move(row));
        }
        return r;
    }

    std::expected<Schema, Error> introspect() override {
        Schema s;
        auto objs = query(
            "SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA = DATABASE() ORDER BY TABLE_TYPE = 'VIEW', BINARY TABLE_NAME", {});
        if (!objs) return std::unexpected(objs.error());
        for (auto& o : objs->rows) {
            Table t;
            t.name = *o[0].as_text();
            t.kind = (*o[1].as_text() == "VIEW") ? Table::Kind::View : Table::Kind::Table;

            auto cols = query(
                "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA "
                "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? "
                "ORDER BY ORDINAL_POSITION", {t.name});
            if (!cols) return std::unexpected(cols.error());
            for (auto& c : cols->rows) {
                Column col;
                col.name = *c[0].as_text();
                col.decl_type = *c[1].as_text();
                col.type = map_type(Dialect::MariaDB, col.decl_type);
                col.nullable = *c[2].as_text() == "YES";
                // MariaDB >= 10.2.7 reports defaults as SQL; "NULL" means none.
                if (auto d = c[3].as_text(); d && *d != "NULL") col.default_sql = *d;
                col.auto_increment = c[4].as_text() &&
                                     c[4].as_text()->find("auto_increment") != std::string::npos;
                t.columns.push_back(std::move(col));
            }

            if (t.kind == Table::Kind::Table) {
                auto idx = query(
                    "SELECT INDEX_NAME, NON_UNIQUE, COLUMN_NAME, SEQ_IN_INDEX "
                    "FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? "
                    "ORDER BY BINARY INDEX_NAME, SEQ_IN_INDEX", {t.name});
                if (!idx) return std::unexpected(idx.error());
                for (auto& i : idx->rows) {
                    const std::string name = *i[0].as_text();
                    if (t.indexes.empty() || t.indexes.back().name != name) {
                        Index ix;
                        ix.name = name;
                        ix.unique = i[1].as_int().value_or(1) == 0;
                        ix.primary = name == "PRIMARY";
                        t.indexes.push_back(std::move(ix));
                    }
                    auto& ix = t.indexes.back();
                    ix.columns.push_back(i[2].as_text() ? *i[2].as_text() : "<expr>");
                    if (ix.primary)
                        if (auto* col = const_cast<Column*>(t.column(ix.columns.back())))
                            col->pk_position = static_cast<int>(ix.columns.size());
                }

                auto fks = query(
                    "SELECT k.CONSTRAINT_NAME, k.COLUMN_NAME, k.REFERENCED_TABLE_NAME, "
                    "       k.REFERENCED_COLUMN_NAME, r.UPDATE_RULE, r.DELETE_RULE "
                    "FROM information_schema.KEY_COLUMN_USAGE k "
                    "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
                    "  ON r.CONSTRAINT_SCHEMA = k.CONSTRAINT_SCHEMA "
                    " AND r.CONSTRAINT_NAME = k.CONSTRAINT_NAME AND r.TABLE_NAME = k.TABLE_NAME "
                    "WHERE k.TABLE_SCHEMA = DATABASE() AND k.TABLE_NAME = ? "
                    "  AND k.REFERENCED_TABLE_NAME IS NOT NULL "
                    "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION", {t.name});
                if (!fks) return std::unexpected(fks.error());
                for (auto& f : fks->rows) {
                    const std::string name = *f[0].as_text();
                    if (t.foreign_keys.empty() || t.foreign_keys.back().name != name) {
                        ForeignKey fk;
                        fk.name = name;
                        fk.ref_table = *f[2].as_text();
                        fk.on_update = *f[4].as_text();
                        fk.on_delete = *f[5].as_text();
                        t.foreign_keys.push_back(std::move(fk));
                    }
                    t.foreign_keys.back().columns.push_back(*f[1].as_text());
                    t.foreign_keys.back().ref_columns.push_back(*f[3].as_text());
                }
            }
            s.tables.push_back(std::move(t));
        }
        return s;
    }

private:
    MYSQL* h_;
    bool ro_;
};

class MariadbEngine final : public Engine {
public:
    std::string id() const override { return "mariadb"; }
    std::string name() const override { return "MariaDB / MySQL"; }
    std::string version() const override { return mysql_get_client_info(); }
    Dialect dialect() const override { return Dialect::MariaDB; }
    Capabilities caps() const override {
        Capabilities c;
        c.needs_server = true;
        c.transactional_ddl = false;      // DDL commits implicitly
        c.returning = false;              // MariaDB 10.5+ has it, MySQL doesn't
        c.schemas = false;                // a database is the namespace
        c.users_privileges = true;
        c.strict_types = true;
        c.named_foreign_keys = true;
        c.partial_indexes = false;
        c.ident_quote = '`';
        c.autoincrement = "AUTO_INCREMENT";
        return c;
    }

    std::expected<std::unique_ptr<Connection>, Error> open(const ConnectParams& p) override {
        MYSQL* h = mysql_init(nullptr);
        if (!h) return std::unexpected(Error{Error::Code::Internal, "mysql_init failed"});
        unsigned timeout = 5;
        mysql_optionsv(h, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_optionsv(h, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        const char* host = p.host.empty() ? "localhost" : p.host.c_str();
        if (!mysql_real_connect(h, host, p.user.c_str(), p.password.c_str(),
                                p.database.empty() ? nullptr : p.database.c_str(),
                                static_cast<unsigned>(p.port), nullptr, 0)) {
            Error e = conn_error(h, host);
            mysql_close(h);
            return std::unexpected(e);
        }
        auto conn = std::make_unique<MariadbConnection>(h, p.read_only);
        const char* mode = p.read_only ? "SET SESSION TRANSACTION READ ONLY"
                                       : "SET SESSION TRANSACTION READ WRITE";
        if (auto r = conn->query(mode, {}); !r) return std::unexpected(r.error());
        return conn;
    }
};

Engine* create() { return new MariadbEngine; }
void destroy(Engine* e) { delete e; }
const AcornEngineEntry kEntry{ACORN_ENGINE_ABI, "mariadb", &create, &destroy};

} // namespace
} // namespace acorn::db::mariadb

ACORN_EXPORT const AcornEngineEntry* acorn_engine_entry(void) {
    return &acorn::db::mariadb::kEntry;
}
