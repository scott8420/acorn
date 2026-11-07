// acorn-engine-postgres -- PostgreSQL over libpq. Links only libpq + acorn_db.
//
// Acorn's SQL uses `?` placeholders on every engine; this plugin rewrites them
// to $1..$n outside string literals, quoted identifiers, comments and
// dollar-quoted bodies. (So Postgres's jsonb `?` operators must be written as
// jsonb_exists() and friends in Acorn SQL.) PQexecParams refuses a second
// statement in one call on its own.
//
// Read-only is the server's: the session starts with
// default_transaction_read_only=on, which refuses DML and DDL (SQLSTATE 25006).
//
// Result values (text format, decoded by type OID): int2/4/8, oid -> Integer;
// float4/8 -> Real; bool -> Integer 0/1 (the same shape MariaDB and SQLite
// give a boolean); bytea -> Blob; everything else, numeric included -> Text.
#include "db/Engine.hpp"
#include "db/Plugin.hpp"

#include <libpq-fe.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acorn::db::postgres {
namespace {

enum Oid_ : Oid {
    BOOLOID = 16, BYTEAOID = 17, INT8OID = 20, INT2OID = 21, INT4OID = 23, TEXTOID = 25,
    OIDOID = 26, FLOAT4OID = 700, FLOAT8OID = 701, BPCHAROID = 1042, VARCHAROID = 1043,
    DATEOID = 1082, TIMEOID = 1083, TIMESTAMPOID = 1114, TIMESTAMPTZOID = 1184,
    NUMERICOID = 1700,
};

const char* type_name(Oid o) {
    switch (o) {
    case BOOLOID: return "boolean"; case BYTEAOID: return "bytea";
    case INT8OID: return "bigint"; case INT2OID: return "smallint"; case INT4OID: return "integer";
    case TEXTOID: return "text"; case OIDOID: return "oid";
    case FLOAT4OID: return "real"; case FLOAT8OID: return "double precision";
    case BPCHAROID: return "character"; case VARCHAROID: return "character varying";
    case DATEOID: return "date"; case TIMEOID: return "time without time zone";
    case TIMESTAMPOID: return "timestamp without time zone";
    case TIMESTAMPTZOID: return "timestamp with time zone";
    case NUMERICOID: return "numeric";
    default: return "";
    }
}

Error::Code code_for(const char* sqlstate) {
    if (!sqlstate) return Error::Code::Internal;
    std::string s = sqlstate;
    if (s == "25006") return Error::Code::ReadOnly;
    if (s == "40P01" || s == "55P03" || s == "40001") return Error::Code::Busy;
    if (s == "3D000") return Error::Code::NotFound;
    if (s.rfind("23", 0) == 0 || s.rfind("22", 0) == 0) return Error::Code::Constraint;
    if (s.rfind("08", 0) == 0 || s.rfind("28", 0) == 0) return Error::Code::Open;
    if (s.rfind("42", 0) == 0) return Error::Code::Syntax;
    return Error::Code::Internal;
}

std::string trim_msg(const char* m) {
    std::string s = m ? m : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    if (s.rfind("ERROR:  ", 0) == 0) s.erase(0, 8);
    // One line: libpq's LINE/DETAIL continuation lines joined with " | ".
    std::string out;
    for (char c : s) {
        if (c == '\n') { while (!out.empty() && out.back() == ' ') out.pop_back(); out += " | "; }
        else if (!(c == ' ' && !out.empty() && out.back() == ' ')) out += c;
    }
    return out;
}

// `?` -> $n, skipping '...', E'...', "...", -- comments, /* */ comments and
// $tag$...$tag$ bodies. Returns the count of placeholders found.
int rewrite_placeholders(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size() + 8);
    int n = 0;
    const std::size_t N = in.size();
    for (std::size_t i = 0; i < N;) {
        char c = in[i];
        if (c == '\'' || c == '"') {
            const bool esc = c == '\'' && i > 0 && (in[i - 1] == 'E' || in[i - 1] == 'e');
            out += c; ++i;
            while (i < N) {
                if (esc && in[i] == '\\' && i + 1 < N) { out += in[i]; out += in[i + 1]; i += 2; continue; }
                out += in[i];
                if (in[i] == c) {
                    if (i + 1 < N && in[i + 1] == c) { out += c; i += 2; continue; }   // doubled
                    ++i; break;
                }
                ++i;
            }
        } else if (c == '-' && i + 1 < N && in[i + 1] == '-') {
            while (i < N && in[i] != '\n') out += in[i++];
        } else if (c == '/' && i + 1 < N && in[i + 1] == '*') {
            int depth = 0;
            do {
                if (in.compare(i, 2, "/*") == 0) { ++depth; out += "/*"; i += 2; }
                else if (in.compare(i, 2, "*/") == 0) { --depth; out += "*/"; i += 2; }
                else out += in[i++];
            } while (i < N && depth > 0);
        } else if (c == '$') {
            // $tag$ (tag may be empty) -- but $1 is a positional param, not a quote.
            std::size_t j = i + 1;
            while (j < N && (std::isalnum(static_cast<unsigned char>(in[j])) || in[j] == '_')) ++j;
            const bool digits_only = j > i + 1 && std::isdigit(static_cast<unsigned char>(in[i + 1]));
            if (j < N && in[j] == '$' && !digits_only) {
                const std::string tag = in.substr(i, j - i + 1);
                auto end = in.find(tag, j + 1);
                end = (end == std::string::npos) ? N : end + tag.size();
                out.append(in, i, end - i);
                i = end;
            } else {
                out += in[i++];
            }
        } else if (c == '?') {
            out += '$'; out += std::to_string(++n); ++i;
        } else {
            out += in[i++];
        }
    }
    return n;
}

struct ResGuard {
    PGresult* r;
    ~ResGuard() { if (r) PQclear(r); }
};

class PgConnection final : public Connection {
public:
    PgConnection(PGconn* h, bool ro) : h_(h), ro_(ro) {}
    ~PgConnection() override { PQfinish(h_); }

    bool read_only() const override { return ro_; }
    std::string quote_ident(const std::string& n) const override { return quote_ident_with('"', n); }

    std::expected<Result, Error>
    query(const std::string& sql, const std::vector<Value>& params) override {
        std::string text;
        const int want = rewrite_placeholders(sql, text);
        if (want != static_cast<int>(params.size()))
            return std::unexpected(Error{Error::Code::Syntax,
                "statement has " + std::to_string(want) + " parameter(s), " +
                std::to_string(params.size()) + " given"});

        std::vector<std::string> store(params.size());
        std::vector<const char*> vals(params.size());
        std::vector<int> lens(params.size()), fmts(params.size());
        std::vector<Oid> types(params.size());
        for (std::size_t i = 0; i < params.size(); ++i) {
            const Value& v = params[i];
            types[i] = 0;            // let the server infer from context
            switch (v.kind()) {
            case Value::Kind::Null: vals[i] = nullptr; break;
            case Value::Kind::Integer: store[i] = std::to_string(*v.as_int()); break;
            case Value::Kind::Real: {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", *v.as_real());
                store[i] = buf;
                break;
            }
            case Value::Kind::Text: store[i] = *v.as_text(); break;
            case Value::Kind::Blob:
                store[i].assign(v.as_blob()->begin(), v.as_blob()->end());
                types[i] = BYTEAOID; fmts[i] = 1; break;
            }
            if (!v.is_null()) {
                vals[i] = store[i].data();
                lens[i] = static_cast<int>(store[i].size());
            }
        }

        ResGuard g{PQexecParams(h_, text.c_str(), static_cast<int>(params.size()),
                                types.data(), vals.data(), lens.data(), fmts.data(), 0)};
        if (!g.r) return std::unexpected(Error{Error::Code::Internal, trim_msg(PQerrorMessage(h_))});

        Result r;
        switch (PQresultStatus(g.r)) {
        case PGRES_EMPTY_QUERY: return r;
        case PGRES_COMMAND_OK: {
            const char* t = PQcmdTuples(g.r);
            r.affected = (t && *t) ? std::atoll(t) : 0;
            return r;
        }
        case PGRES_TUPLES_OK: break;
        default:
            return std::unexpected(Error{code_for(PQresultErrorField(g.r, PG_DIAG_SQLSTATE)),
                                         trim_msg(PQresultErrorMessage(g.r))});
        }

        const int nf = PQnfields(g.r), nt = PQntuples(g.r);
        std::vector<Oid> ft(static_cast<std::size_t>(nf));
        for (int c = 0; c < nf; ++c) {
            ft[static_cast<std::size_t>(c)] = PQftype(g.r, c);
            r.columns.push_back({PQfname(g.r, c), type_name(ft[static_cast<std::size_t>(c)])});
        }
        r.rows.reserve(static_cast<std::size_t>(nt));
        for (int t = 0; t < nt; ++t) {
            Row row;
            row.reserve(static_cast<std::size_t>(nf));
            for (int c = 0; c < nf; ++c) {
                if (PQgetisnull(g.r, t, c)) { row.emplace_back(); continue; }
                const char* s = PQgetvalue(g.r, t, c);
                switch (ft[static_cast<std::size_t>(c)]) {
                case INT2OID: case INT4OID: case INT8OID: case OIDOID:
                    row.emplace_back(static_cast<std::int64_t>(std::strtoll(s, nullptr, 10))); break;
                case FLOAT4OID: case FLOAT8OID:
                    row.emplace_back(std::strtod(s, nullptr)); break;
                case BOOLOID:
                    row.emplace_back(static_cast<std::int64_t>(s[0] == 't' ? 1 : 0)); break;
                case BYTEAOID: {
                    std::size_t n = 0;
                    unsigned char* b = PQunescapeBytea(reinterpret_cast<const unsigned char*>(s), &n);
                    if (!b) return std::unexpected(Error{Error::Code::Internal, "bytea decode failed"});
                    row.emplace_back(Blob(b, b + n));
                    PQfreemem(b);
                    break;
                }
                default:
                    row.emplace_back(std::string(s, static_cast<std::size_t>(PQgetlength(g.r, t, c))));
                }
            }
            r.rows.push_back(std::move(row));
        }
        return r;
    }

    std::expected<Schema, Error> introspect() override {
        static const char* kAction[] = {"NO ACTION", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"};
        auto action = [](const std::string& c) -> std::string {
            switch (c.empty() ? 'a' : c[0]) {
            case 'r': return kAction[1]; case 'c': return kAction[2];
            case 'n': return kAction[3]; case 'd': return kAction[4];
            default:  return kAction[0];
            }
        };

        Schema s;
        auto objs = query(
            "SELECT c.oid::int8, c.relname::text, c.relkind::text FROM pg_class c "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = current_schema() AND c.relkind IN ('r','p','v','m') "
            "ORDER BY c.relkind IN ('v','m'), c.relname::text COLLATE \"C\"", {});
        if (!objs) return std::unexpected(objs.error());
        for (auto& o : objs->rows) {
            const Value oid = o[0];
            Table t;
            t.name = *o[1].as_text();
            const std::string kind = *o[2].as_text();
            t.kind = (kind == "v" || kind == "m") ? Table::Kind::View : Table::Kind::Table;

            auto cols = query(
                "SELECT a.attname::text, format_type(a.atttypid, a.atttypmod), a.attnotnull, "
                "       pg_get_expr(d.adbin, d.adrelid), a.attidentity::text "
                "FROM pg_attribute a LEFT JOIN pg_attrdef d "
                "  ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
                "WHERE a.attrelid = ?::int8::oid AND a.attnum > 0 AND NOT a.attisdropped "
                "ORDER BY a.attnum", {oid});
            if (!cols) return std::unexpected(cols.error());
            for (auto& c : cols->rows) {
                Column col;
                col.name = *c[0].as_text();
                col.decl_type = *c[1].as_text();
                col.type = map_type(Dialect::PostgreSQL, col.decl_type);
                col.nullable = c[2].as_int().value_or(0) == 0;
                if (auto d = c[3].as_text()) col.default_sql = *d;
                const std::string ident = c[4].as_text() ? *c[4].as_text() : "";
                col.auto_increment = !ident.empty() ||
                    (col.default_sql && col.default_sql->rfind("nextval(", 0) == 0);
                t.columns.push_back(std::move(col));
            }

            if (t.kind == Table::Kind::Table) {
                auto idx = query(
                    "SELECT ic.relname::text, i.indisunique, i.indisprimary, i.indpred IS NOT NULL, "
                    "       a.attname::text "
                    "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
                    "CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY AS u(attnum, ord) "
                    "LEFT JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = u.attnum "
                    "WHERE i.indrelid = ?::int8::oid AND u.ord <= i.indnkeyatts "
                    "ORDER BY ic.relname::text COLLATE \"C\", u.ord", {oid});
                if (!idx) return std::unexpected(idx.error());
                for (auto& i : idx->rows) {
                    const std::string name = *i[0].as_text();
                    if (t.indexes.empty() || t.indexes.back().name != name) {
                        Index ix;
                        ix.name = name;
                        ix.unique = i[1].as_int().value_or(0) != 0;
                        ix.primary = i[2].as_int().value_or(0) != 0;
                        ix.partial = i[3].as_int().value_or(0) != 0;
                        t.indexes.push_back(std::move(ix));
                    }
                    auto& ix = t.indexes.back();
                    ix.columns.push_back(i[4].as_text() ? *i[4].as_text() : "<expr>");
                    if (ix.primary)
                        for (auto& col : t.columns)
                            if (col.name == ix.columns.back())
                                col.pk_position = static_cast<int>(ix.columns.size());
                }

                auto fks = query(
                    "SELECT con.conname::text, fc.relname::text, con.confupdtype::text, "
                    "       con.confdeltype::text, a.attname::text, fa.attname::text "
                    "FROM pg_constraint con JOIN pg_class fc ON fc.oid = con.confrelid "
                    "CROSS JOIN LATERAL unnest(con.conkey, con.confkey) WITH ORDINALITY AS u(k, fk, ord) "
                    "JOIN pg_attribute a  ON a.attrelid = con.conrelid  AND a.attnum = u.k "
                    "JOIN pg_attribute fa ON fa.attrelid = con.confrelid AND fa.attnum = u.fk "
                    "WHERE con.conrelid = ?::int8::oid AND con.contype = 'f' "
                    "ORDER BY con.conname, u.ord", {oid});
                if (!fks) return std::unexpected(fks.error());
                for (auto& f : fks->rows) {
                    const std::string name = *f[0].as_text();
                    if (t.foreign_keys.empty() || t.foreign_keys.back().name != name) {
                        ForeignKey fk;
                        fk.name = name;
                        fk.ref_table = *f[1].as_text();
                        fk.on_update = action(*f[2].as_text());
                        fk.on_delete = action(*f[3].as_text());
                        t.foreign_keys.push_back(std::move(fk));
                    }
                    t.foreign_keys.back().columns.push_back(*f[4].as_text());
                    t.foreign_keys.back().ref_columns.push_back(*f[5].as_text());
                }
            }
            s.tables.push_back(std::move(t));
        }
        return s;
    }

private:
    PGconn* h_;
    bool ro_;
};

class PgEngine final : public Engine {
public:
    std::string id() const override { return "postgres"; }
    std::string name() const override { return "PostgreSQL"; }
    std::string version() const override {
        int v = PQlibVersion();
        return std::to_string(v / 10000) + "." + std::to_string(v % 10000);
    }
    Dialect dialect() const override { return Dialect::PostgreSQL; }
    Capabilities caps() const override {
        Capabilities c;
        c.needs_server = true;
        c.transactional_ddl = true;
        c.returning = true;
        c.schemas = true;
        c.users_privileges = true;
        c.strict_types = true;
        c.named_foreign_keys = true;
        c.partial_indexes = true;
        c.ident_quote = '"';
        c.autoincrement = "GENERATED BY DEFAULT AS IDENTITY";
        return c;
    }

    std::expected<std::unique_ptr<Connection>, Error> open(const ConnectParams& p) override {
        const std::string port = p.port ? std::to_string(p.port) : "";
        const char* keys[] = {"host", "port", "dbname", "user", "password", "connect_timeout",
                              "client_encoding", "application_name", "options", nullptr};
        const char* vals[] = {p.host.empty() ? nullptr : p.host.c_str(),
                              port.empty() ? nullptr : port.c_str(),
                              p.database.empty() ? nullptr : p.database.c_str(),
                              p.user.empty() ? nullptr : p.user.c_str(),
                              p.password.empty() ? nullptr : p.password.c_str(),
                              "5", "UTF8", "Acorn",
                              p.read_only ? "-c default_transaction_read_only=on" : nullptr,
                              nullptr};
        PGconn* h = PQconnectdbParams(keys, vals, 0);
        if (!h) return std::unexpected(Error{Error::Code::Internal, "out of memory"});
        if (PQstatus(h) != CONNECTION_OK) {
            std::string msg = trim_msg(PQerrorMessage(h));
            PQfinish(h);
            const bool missing = msg.find("does not exist") != std::string::npos &&
                                 msg.find("database") != std::string::npos;
            return std::unexpected(Error{missing ? Error::Code::NotFound : Error::Code::Open, msg});
        }
        // Server NOTICEs ("... does not exist, skipping") are not errors and
        // must not land on Acorn's stderr.
        PQsetNoticeProcessor(h, [](void*, const char*) {}, nullptr);
        return std::make_unique<PgConnection>(h, p.read_only);
    }
};

Engine* create() { return new PgEngine; }
void destroy(Engine* e) { delete e; }
const AcornEngineEntry kEntry{ACORN_ENGINE_ABI, "postgres", &create, &destroy};

} // namespace
} // namespace acorn::db::postgres

ACORN_EXPORT const AcornEngineEntry* acorn_engine_entry(void) {
    return &acorn::db::postgres::kEntry;
}
