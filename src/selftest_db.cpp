// selftest_db.cpp -- the database layer, headless (s002, s003).
//
// RULES "the data is sacred": every database here is a fixture this file
// creates and removes -- a temp SQLite file, or a throwaway database named
// acorn_selftest_<pid> on a server named by ACORN_TEST_MARIADB /
// ACORN_TEST_POSTGRES (see scripts/acorn_servers.sh). One suite runs against
// every engine; an engine that can't run is reported SKIPPED, never a pass.
#include "db/Engine.hpp"
#include "db/Loader.hpp"
#include "db/Types.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace acorn::db;

namespace acorn::selftest {

using CheckFn = void (*)(const std::string&, bool, const std::string&);

namespace {

CheckFn check;
int*    skips;

void skip(const std::string& name, const std::string& why) {
    ++*skips;
    std::cout << "  SKIP  " << name << "  (" << why << ")\n";
}

struct TempDir {
    fs::path p;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "acorn-selftest-XXXXXX").string();
        p = mkdtemp(tmpl.data());
    }
    ~TempDir() { std::error_code ec; fs::remove_all(p, ec); }
};

std::string slurp(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// ── Types and values: pure, no engine ─────────────────────────────────────────
void test_types() {
    std::cout << "\n-- db: values + type map --\n";

    Value n, i(std::int64_t{42}), r(2.5), t("héllo"), b(Blob{0x00, 0xff});
    check("value: kinds", n.is_null() && i.kind() == Value::Kind::Integer &&
          r.kind() == Value::Kind::Real && t.kind() == Value::Kind::Text &&
          b.kind() == Value::Kind::Blob, "");
    check("value: exact accessors refuse other kinds",
          !t.as_int() && !i.as_text() && !n.as_real() && i.as_real() == 42.0, "");
    check("value: display", n.to_display() == "NULL" && i.to_display() == "42" &&
          t.to_display() == "'héllo'" && b.to_display() == "x'00ff'", b.to_display());
    check("quote_ident doubles the quote char",
          quote_ident_with('"', "we\"ird") == "\"we\"\"ird\"" &&
          quote_ident_with('`', "a`b") == "`a``b`", "");

    struct Case { Dialect d; const char* decl; FieldType want; };
    const Case cases[] = {
        // SQLite: affinity rules, with date/time/bool names first.
        {Dialect::SQLite, "INTEGER", FieldType::Integer},
        {Dialect::SQLite, "BIGINT UNSIGNED", FieldType::Integer},
        {Dialect::SQLite, "VARCHAR(40)", FieldType::Text},
        {Dialect::SQLite, "CLOB", FieldType::Text},
        {Dialect::SQLite, "BLOB", FieldType::Container},
        {Dialect::SQLite, "REAL", FieldType::Number},
        {Dialect::SQLite, "DOUBLE PRECISION", FieldType::Number},
        {Dialect::SQLite, "DECIMAL(10,2)", FieldType::Number},
        {Dialect::SQLite, "DATE", FieldType::Date},
        {Dialect::SQLite, "DATETIME", FieldType::Timestamp},
        {Dialect::SQLite, "timestamp", FieldType::Timestamp},
        {Dialect::SQLite, "BOOLEAN", FieldType::Boolean},
        {Dialect::SQLite, "", FieldType::Unknown},
        // MariaDB (exercised for real in s003; the map is shared).
        {Dialect::MariaDB, "tinyint(1)", FieldType::Boolean},
        {Dialect::MariaDB, "tinyint(4)", FieldType::Integer},
        {Dialect::MariaDB, "int(11) unsigned", FieldType::Integer},
        {Dialect::MariaDB, "decimal(10,2)", FieldType::Number},
        {Dialect::MariaDB, "varchar(255)", FieldType::Text},
        {Dialect::MariaDB, "enum('a','b')", FieldType::Text},
        {Dialect::MariaDB, "longblob", FieldType::Container},
        {Dialect::MariaDB, "datetime(6)", FieldType::Timestamp},
        // PostgreSQL.
        {Dialect::PostgreSQL, "integer", FieldType::Integer},
        {Dialect::PostgreSQL, "bigserial", FieldType::Integer},
        {Dialect::PostgreSQL, "double precision", FieldType::Number},
        {Dialect::PostgreSQL, "numeric(12,4)", FieldType::Number},
        {Dialect::PostgreSQL, "character varying(80)", FieldType::Text},
        {Dialect::PostgreSQL, "bytea", FieldType::Container},
        {Dialect::PostgreSQL, "timestamp with time zone", FieldType::Timestamp},
        {Dialect::PostgreSQL, "time without time zone", FieldType::Time},
        {Dialect::PostgreSQL, "boolean", FieldType::Boolean},
    };
    int bad = 0;
    std::string first_bad;
    for (auto& c : cases) {
        auto got = map_type(c.d, c.decl);
        if (got != c.want) {
            if (!bad++) first_bad = std::string(to_string(c.d)) + " '" + c.decl + "' -> " +
                                    to_string(got) + ", want " + to_string(c.want);
        }
    }
    check("type map: " + std::to_string(std::size(cases)) + " declared types", bad == 0, first_bad);
}

// ── The loader ────────────────────────────────────────────────────────────────
void test_loader() {
    std::cout << "\n-- db: plugin loader --\n";
    {
        Loader L;
        int n = L.scan({"/nonexistent/acorn/engines"});
        check("loader: a missing dir loads nothing, rejects nothing",
              n == 0 && L.engines().empty() && L.rejected().empty(), "");
    }
    {
        Loader L;
        L.scan({ACORN_TEST_BADABI_DIR});
        bool refused = L.engines().empty() && L.rejected().size() == 1 &&
                       L.rejected()[0].reason.find("ABI") != std::string::npos;
        check("loader: refuses a plugin built for another ABI", refused,
              L.rejected().empty() ? "nothing rejected" : L.rejected()[0].reason);
    }
    {
        Loader once, twice;
        once.scan({ACORN_TEST_ENGINE_DIR});
        twice.scan({ACORN_TEST_ENGINE_DIR, ACORN_TEST_ENGINE_DIR});
        std::size_t dups = 0;
        for (auto& r : twice.rejected()) dups += r.reason.find("duplicate") != std::string::npos;
        check("loader: the same engines twice -> first wins, each duplicate reported",
              !once.engines().empty() && twice.engines().size() == once.engines().size() &&
              dups == once.engines().size(),
              std::to_string(twice.engines().size()) + " loaded, " + std::to_string(dups) + " dup");
    }
    {
        // Default search order: $ACORN_ENGINE_PATH, then <exe>/engines (build
        // tree), then <exe>/../lib/acorn/engines (installed by install.sh).
        setenv("ACORN_ENGINE_PATH", "/a/one::/a/two", 1);
        auto d = Loader::default_dirs();
        unsetenv("ACORN_ENGINE_PATH");
        fs::path exe = fs::read_symlink("/proc/self/exe").parent_path();
        bool ok = d.size() == 4 && d[0] == "/a/one" && d[1] == "/a/two" &&
                  d[2] == exe / "engines" &&
                  d[3] == (exe.parent_path() / "lib" / "acorn" / "engines");
        check("loader: default dirs = env, build tree, <prefix>/lib/acorn/engines", ok,
              d.size() > 3 ? d[3].string() : std::to_string(d.size()) + " dirs");
    }
}

// ── One suite, every engine (s003) ────────────────────────────────────────────
// The same logical schema in each dialect. Where engines legitimately differ
// (auto-increment spelling, BLOB vs BYTEA, partial indexes, WITHOUT ROWID) the
// DDL differs; the expectations below are shared, gated only by Capabilities.
std::vector<std::string> fixture(Dialect d) {
    const bool lite = d == Dialect::SQLite, my = d == Dialect::MariaDB;
    const std::string id = lite ? "id INTEGER PRIMARY KEY"
                         : my   ? "id INTEGER AUTO_INCREMENT PRIMARY KEY"
                                : "id INTEGER GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY";
    const std::string oid = lite ? "id INTEGER PRIMARY KEY AUTOINCREMENT"
                          : my   ? "id BIGINT AUTO_INCREMENT PRIMARY KEY"
                                 : "id BIGSERIAL PRIMARY KEY";
    const std::string blob = lite ? "BLOB" : my ? "LONGBLOB" : "BYTEA";
    const std::string ts = lite || my ? "DATETIME" : "TIMESTAMP";
    const std::string dbl = lite ? "REAL" : "DOUBLE PRECISION";
    const std::string today = my ? "(CURRENT_DATE)" : "CURRENT_DATE";
    std::vector<std::string> v = {
        "CREATE TABLE customers (" + id + ","
        "  name VARCHAR(80) NOT NULL,"
        "  email VARCHAR(120) UNIQUE,"
        "  balance DECIMAL(10,2) DEFAULT 0,"
        "  joined DATE DEFAULT " + today + ","
        "  active BOOLEAN NOT NULL DEFAULT " + (lite ? "1" : "TRUE") + ","
        "  photo " + blob + ","
        "  notes" + (lite ? "" : " TEXT") + ")",
        "CREATE TABLE orders (" + oid + ","
        "  customer_id INTEGER NOT NULL,"
        "  placed " + ts + " NOT NULL,"
        "  total " + dbl + ","
        "  CONSTRAINT orders_customer_fk FOREIGN KEY (customer_id)"
        "    REFERENCES customers(id) ON DELETE CASCADE)",
        "CREATE INDEX orders_by_customer ON orders(customer_id, placed)",
        "CREATE TABLE order_lines ("
        "  order_id " + std::string(lite ? "INTEGER" : "BIGINT") + ", line INTEGER,"
        "  sku VARCHAR(40), qty INTEGER CHECK (qty > 0),"
        "  PRIMARY KEY (order_id, line),"
        "  CONSTRAINT order_lines_order_fk FOREIGN KEY (order_id)"
        "    REFERENCES orders(id) ON UPDATE CASCADE ON DELETE RESTRICT)",
        std::string("CREATE TABLE tags (code INTEGER PRIMARY KEY, label TEXT)") +
            (lite ? " WITHOUT ROWID" : ""),
        "CREATE VIEW big_spenders AS SELECT c.name, sum(o.total) AS spent"
        "  FROM customers c JOIN orders o ON o.customer_id = c.id GROUP BY c.id, c.name",
    };
    if (d != Dialect::MariaDB)
        v.insert(v.begin() + 3, "CREATE INDEX orders_big ON orders(total) WHERE total > 1000");
    return v;
}

// A number however the engine shaped it: SQLite's REAL, a server's exact
// DECIMAL text, or an integer.
std::optional<double> num(const Value& v) {
    if (auto r = v.as_real()) return r;
    if (auto t = v.as_text()) { try { return std::stod(*t); } catch (...) {} }
    return std::nullopt;
}

struct Target {
    Engine*       e;
    ConnectParams rw, ro;
    std::string   tag;          // "sqlite", "mariadb", "postgres"
};

void test_suite(const Target& T) {
    Engine* e = T.e;
    const Capabilities caps = e->caps();
    const std::string P = T.tag + ": ";
    const std::size_t want_tables = 5;

    {   // Build the fixture read-write.
        auto c = e->open(T.rw);
        check(P + "open read-write fixture", c.has_value(), c ? "" : c.error().message);
        if (!c) return;
        auto& C = **c;
        check(P + "fixture connection is read-write", !C.read_only(), "");
        bool ok = true; std::string why;
        for (auto& sql : fixture(e->dialect()))
            if (auto r = C.query(sql); !r) { ok = false; why = r.error().message; break; }
        check(P + "fixture schema created", ok, why);
        if (!ok) return;

        auto ins = C.query("INSERT INTO customers(name, email, balance, photo, notes) VALUES (?,?,?,?,?)",
                           {Value("Ada"), Value("ada@example.org"), Value(12.5),
                            Value(Blob{1, 2, 0, 255}), Value()});
        check(P + "insert: affected 1, id via last_insert_id or RETURNING",
              ins && ins->affected == 1 &&
              (ins->last_insert_id == 1 || (!ins->last_insert_id && caps.returning)),
              ins ? "" : ins.error().message);
        if (caps.returning) {
            auto ret = C.query("INSERT INTO tags(code, label) VALUES (?, ?) RETURNING code", {Value(7), Value("seven")});
            check(P + "INSERT ... RETURNING", ret && ret->rows.size() == 1 &&
                  ret->rows[0][0] == Value(std::int64_t{7}), ret ? "" : ret.error().message);
        }
        auto sel = C.query("SELECT id, name, balance, photo, notes, active FROM customers WHERE email = ?",
                           {Value("ada@example.org")});
        bool rt = sel && sel->rows.size() == 1 &&
                  sel->rows[0][0] == Value(std::int64_t{1}) &&
                  sel->rows[0][1] == Value("Ada") &&
                  num(sel->rows[0][2]) == 12.5 &&
                  sel->rows[0][3] == Value(Blob{1, 2, 0, 255}) &&
                  sel->rows[0][4].is_null() &&
                  sel->rows[0][5] == Value(std::int64_t{1});
        check(P + "null / int / decimal / text / blob / boolean round-trip", rt,
              sel ? (sel->rows.empty() ? "no row" : sel->rows[0][2].to_display() + " " +
                     sel->rows[0][5].to_display()) : sel.error().message);
        check(P + "result carries column names", sel && sel->columns.size() == 6 &&
              sel->column_index("balance") == 2, "");

        const std::string uni = "Zoë — 🌰 acorn";
        auto u = C.query("INSERT INTO customers(name) VALUES (?)", {Value(uni)});
        auto ub = C.query("SELECT name FROM customers WHERE name = ?", {Value(uni)});
        check(P + "UTF-8 (incl. 4-byte) round-trips", u && ub && ub->rows.size() == 1 &&
              ub->rows[0][0] == Value(uni), ub ? "" : ub.error().message);

        auto evil = C.query("INSERT INTO customers(name) VALUES (?)",
                            {Value("x'); DROP TABLE customers; --")});
        auto still = C.query("SELECT count(*) FROM customers");
        check(P + "an injection-shaped value is stored as data",
              evil && still && num(still->rows[0][0]) == 3.0, "");

        auto ph = C.query("SELECT ? AS a, '?' AS b /* ? */ -- ?\n", {Value(7)});
        check(P + "? inside quotes / comments is not a placeholder",
              ph && ph->rows.size() == 1 && num(ph->rows[0][0]) == 7.0 && ph->rows[0][1] == Value("?"),
              ph ? "" : ph.error().message);

        auto two = C.query("SELECT 1; SELECT 2");
        check(P + "two statements in one call are refused", !two, "");
        auto cnt = C.query("SELECT ?", {});
        check(P + "parameter count mismatch is refused", !cnt, "");
        auto syn = C.query("SELEC nonsense");
        check(P + "a syntax error maps to Syntax with the engine's words",
              !syn && syn.error().code == Error::Code::Syntax && !syn.error().message.empty(),
              syn ? "" : syn.error().message);
        auto fk = C.query("INSERT INTO orders(customer_id, placed) VALUES (999, '2026-01-01 00:00:00')");
        check(P + "foreign keys are enforced (Constraint)",
              !fk && fk.error().code == Error::Code::Constraint, fk ? "inserted" : fk.error().message);
        check(P + "quote_ident", C.quote_ident("order") ==
              std::string(1, caps.ident_quote) + "order" + caps.ident_quote, "");
    }

    {   // Introspection, on a read-only connection.
        auto c = e->open(T.ro);
        check(P + "reopen read-only", c && (*c)->read_only(), c ? "" : c.error().message);
        if (!c) return;
        auto& C = **c;

        auto w = C.query("INSERT INTO customers(name) VALUES ('nope')");
        check(P + "a write on a read-only connection is refused by the engine",
              !w && w.error().code == Error::Code::ReadOnly, w ? "written!" : w.error().message);
        auto ddl = C.query("CREATE TABLE sneaky(x INTEGER)");
        check(P + "DDL on a read-only connection is refused", !ddl, ddl ? "created!" : ddl.error().message);

        auto s = C.introspect();
        check(P + "introspect", s.has_value(), s ? "" : s.error().message);
        if (!s) return;

        std::string names;
        for (auto& t : s->tables) names += t.name + (t.kind == Table::Kind::View ? "(v) " : " ");
        check(P + "tables then views, by name, nothing internal",
              names == "customers order_lines orders tags big_spenders(v) ", names);

        const Table* cu = s->table("customers");
        if (!cu) return;
        const Column* id = cu->column("id");
        check(P + "the customers key is auto-increment",
              id && id->pk_position == 1 && id->auto_increment && id->type == FieldType::Integer, "");
        const Column* name = cu->column("name");
        check(P + "NOT NULL + declared type kept",
              name && !name->nullable && name->type == FieldType::Text &&
              name->decl_type.find("80") != std::string::npos, name ? name->decl_type : "");
        const Column* bal = cu->column("balance");
        const Column* joined = cu->column("joined");
        check(P + "defaults kept as SQL text",
              bal && bal->default_sql && num(Value(*bal->default_sql)) == 0.0 &&
              joined && joined->default_sql && !joined->default_sql->empty(),
              (bal && bal->default_sql ? *bal->default_sql : "-") + " / " +
              (joined && joined->default_sql ? *joined->default_sql : "-"));
        check(P + "boolean / container / date / number columns",
              cu->column("active")->type == FieldType::Boolean &&
              cu->column("photo")->type == FieldType::Container &&
              joined->type == FieldType::Date && bal->type == FieldType::Number,
              cu->column("active")->decl_type + ", " + cu->column("photo")->decl_type);
        bool email_unique = false;
        for (auto& ix : cu->indexes)
            email_unique = email_unique || (ix.unique && ix.columns == std::vector<std::string>{"email"});
        check(P + "UNIQUE column shows as a unique index", email_unique, "");

        const Table* ord = s->table("orders");
        check(P + "orders key auto-increment", ord && ord->column("id")->auto_increment, "");
        bool fk_ok = ord && ord->foreign_keys.size() == 1 &&
                     ord->foreign_keys[0].ref_table == "customers" &&
                     ord->foreign_keys[0].columns == std::vector<std::string>{"customer_id"} &&
                     ord->foreign_keys[0].ref_columns == std::vector<std::string>{"id"} &&
                     ord->foreign_keys[0].on_delete == "CASCADE";
        check(P + "foreign key with ON DELETE CASCADE", fk_ok, "");
        if (caps.named_foreign_keys)
            check(P + "foreign key keeps its name",
                  fk_ok && ord->foreign_keys[0].name == "orders_customer_fk", "");
        bool multi = false, partial = false;
        for (auto& ix : ord->indexes) {
            if (ix.name == "orders_by_customer")
                multi = !ix.unique && ix.columns == std::vector<std::string>{"customer_id", "placed"};
            if (ix.name == "orders_big") partial = ix.partial;
        }
        check(P + "multi-column index, in key order", multi, "");
        if (caps.partial_indexes) check(P + "partial index flagged", partial, "");
        check(P + "DATETIME / TIMESTAMP -> timestamp", ord->column("placed")->type == FieldType::Timestamp,
              ord->column("placed")->decl_type);

        const Table* ol = s->table("order_lines");
        check(P + "composite primary key, in key order",
              ol && ol->primary_key() == std::vector<std::string>{"order_id", "line"} &&
              !ol->column("order_id")->auto_increment, "");
        check(P + "FK ON UPDATE CASCADE ON DELETE RESTRICT",
              ol && ol->foreign_keys.size() == 1 && ol->foreign_keys[0].on_update == "CASCADE" &&
              ol->foreign_keys[0].on_delete == "RESTRICT",
              ol && !ol->foreign_keys.empty() ? ol->foreign_keys[0].on_update + "/" + ol->foreign_keys[0].on_delete : "");

        const Table* tg = s->table("tags");
        bool pk_ix = false;
        for (auto& ix : tg->indexes) pk_ix = pk_ix || ix.primary;
        check(P + "a plain INTEGER key is NOT auto-increment; its index is primary",
              tg && tg->column("code")->pk_position == 1 && !tg->column("code")->auto_increment && pk_ix, "");

        const Table* v = s->table("big_spenders");
        check(P + "view columns read, no keys",
              v && v->kind == Table::Kind::View && v->columns.size() == 2 &&
              v->column("spent") && v->indexes.empty() && v->foreign_keys.empty(), "");

        auto after = C.query("SELECT count(*) FROM customers");
        auto s2 = C.introspect();
        check(P + "nothing changed during the read-only session",
              after && num(after->rows[0][0]) == 3.0 && s2 && s2->tables.size() == want_tables, "");
    }
}

// SQLite-only behaviour: files.
void test_sqlite_files(Engine* e, const fs::path& dir) {
    {
        ConnectParams p; p.path = (dir / "missing.sqlite").string();
        auto c = e->open(p);
        check("sqlite: read-only open of a missing file fails, creates nothing",
              !c && c.error().code == Error::Code::NotFound && !fs::exists(p.path), "");
    }
    {
        const fs::path junk = dir / "junk.sqlite";
        std::ofstream(junk) << "this is not a database, just text long enough to have a header\n";
        ConnectParams p; p.path = junk.string();
        auto c = e->open(p);
        check("sqlite: a non-database file is refused at open", !c && c.error().code == Error::Code::Open,
              c ? "opened" : c.error().message);
    }
}

// "host=127.0.0.1 port=3307 user=acorn password=acorn" -> ConnectParams.
ConnectParams parse_env(const char* s) {
    ConnectParams p;
    std::string str = s;
    std::size_t i = 0;
    while (i < str.size()) {
        auto sp = str.find(' ', i);
        if (sp == std::string::npos) sp = str.size();
        auto kv = str.substr(i, sp - i);
        auto eq = kv.find('=');
        if (eq != std::string::npos) {
            auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "host") p.host = v;
            else if (k == "port") p.port = std::atoi(v.c_str());
            else if (k == "user") p.user = v;
            else if (k == "password") p.password = v;
        }
        i = sp + 1;
    }
    return p;
}

// A throwaway database on a server: create acorn_selftest_<pid>, run the
// suite inside it, drop it. Only runs when ACORN_TEST_<ENGINE> is set -- the
// selftest never goes looking for a server on its own.
void test_server(Engine* e, const char* env_name, const char* admin_db) {
    const char* env = std::getenv(env_name);
    if (!env) { skip(e->id() + " suite", std::string(env_name) + " not set"); return; }

    ConnectParams admin = parse_env(env);
    admin.read_only = false;
    admin.database = admin_db;
    auto a = e->open(admin);
    if (!a) { skip(e->id() + " suite", "server unreachable: " + a.error().message); return; }

    const std::string dbname = "acorn_selftest_" + std::to_string(getpid());
    const std::string q = (*a)->quote_ident(dbname);
    (void)(*a)->query("DROP DATABASE IF EXISTS " + q);
    std::string create = "CREATE DATABASE " + q;
    if (e->dialect() == Dialect::MariaDB) create += " CHARACTER SET utf8mb4 COLLATE utf8mb4_bin";
    else create += " ENCODING 'UTF8' TEMPLATE template0";
    auto made = (*a)->query(create);
    check(e->id() + ": create throwaway database " + dbname, made.has_value(),
          made ? "" : made.error().message);
    if (!made) return;
    a->reset();   // Postgres refuses to drop a database while anyone is attached

    Target T{e, admin, admin, e->id()};
    T.rw.database = dbname;
    T.ro.database = dbname;
    T.ro.read_only = true;
    test_suite(T);

    auto a2 = e->open(admin);
    std::string why = a2 ? "" : a2.error().message;
    bool dropped = false;
    if (a2) {
        auto d = (*a2)->query("DROP DATABASE " + q);
        dropped = d.has_value();
        if (!d) why = d.error().message;
    }
    check(e->id() + ": throwaway database dropped", dropped, why);
}

} // namespace

void db_tests(CheckFn fn, int& skip_count) {
    check = fn;
    skips = &skip_count;

    test_types();
    test_loader();

    Loader L;
    L.scan({ACORN_TEST_ENGINE_DIR});
    for (auto& r : L.rejected())
        std::cout << "  note  rejected " << r.file.filename().string() << ": " << r.reason << "\n";

    // RULES: every engine, every test. A missing engine is a SKIP, never a pass.
    if (Engine* e = L.find("sqlite")) {
        std::cout << "\n-- db: sqlite (client " << e->version() << ") --\n";
        auto c = e->caps();
        check("sqlite: capabilities", !c.needs_server && c.transactional_ddl && !c.strict_types &&
              c.partial_indexes && c.ident_quote == '"' && c.autoincrement == "AUTOINCREMENT" &&
              e->dialect() == Dialect::SQLite, "");
        TempDir tmp;
        test_sqlite_files(e, tmp.p);
        Target T{e, {}, {}, "sqlite"};
        T.rw.path = (tmp.p / "fixture.sqlite").string();
        T.rw.create = true; T.rw.read_only = false;
        T.ro.path = T.rw.path;                       // read_only defaults to true
        test_suite(T);
        // The file-level proof: a read-only session leaves the bytes alone.
        const std::string before = slurp(T.rw.path);
        { auto ro = e->open(T.ro); if (ro) (void)(*ro)->introspect(); }
        check("sqlite: the file is byte-identical after a read-only session", slurp(T.rw.path) == before, "");
    } else {
        skip("sqlite engine", "acorn-engine-sqlite.so not built or not loadable");
    }

    if (Engine* e = L.find("mariadb")) {
        std::cout << "\n-- db: mariadb (client " << e->version() << ") --\n";
        auto c = e->caps();
        check("mariadb: capabilities", c.needs_server && !c.transactional_ddl && c.strict_types &&
              !c.partial_indexes && c.ident_quote == '`' && c.autoincrement == "AUTO_INCREMENT", "");
        test_server(e, "ACORN_TEST_MARIADB", "");
    } else {
        skip("mariadb engine", "acorn-engine-mariadb.so not built or not loadable");
    }

    if (Engine* e = L.find("postgres")) {
        std::cout << "\n-- db: postgres (client " << e->version() << ") --\n";
        auto c = e->caps();
        check("postgres: capabilities", c.needs_server && c.transactional_ddl && c.returning &&
              c.partial_indexes && c.ident_quote == '"', "");
        test_server(e, "ACORN_TEST_POSTGRES", "postgres");
    } else {
        skip("postgres engine", "acorn-engine-postgres.so not built or not loadable");
    }
}

} // namespace acorn::selftest
