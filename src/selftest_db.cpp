// selftest_db.cpp -- the database layer, headless (s002, s003).
//
// RULES "the data is sacred": every database here is a fixture this file
// creates and removes -- a temp SQLite file, or a throwaway database named
// acorn_selftest_<pid> on a server named by ACORN_TEST_MARIADB /
// ACORN_TEST_POSTGRES (see scripts/acorn_servers.sh). One suite runs against
// every engine; an engine that can't run is reported SKIPPED, never a pass.
#include "db/Engine.hpp"
#include "db/Journal.hpp"
#include "db/Browse.hpp"
#include "db/Loader.hpp"
#include "db/NewFile.hpp"
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

        // s004c -- what the Tables browser's "links in" reads.
        {
            const auto in_c = s->links_to("customers");
            const auto in_o = s->links_to("orders");
            check(P + "links in: customers <- orders.customer_id",
                  in_c.size() == 1 && in_c[0].from->name == "orders" &&
                  in_c[0].fk->columns == std::vector<std::string>{"customer_id"}, "");
            check(P + "links in: orders <- order_lines",
                  in_o.size() == 1 && in_o[0].from->name == "order_lines", "");
            check(P + "links in: none into tags", s->links_to("tags").empty(), "");
            check(P + "link targets resolve to columns",
                  ol && !ol->foreign_keys.empty() &&
                  s->ref_columns(ol->foreign_keys[0]) == std::vector<std::string>{"id"}, "");
        }

        auto after = C.query("SELECT count(*) FROM customers");
        auto s2 = C.introspect();
        check(P + "nothing changed during the read-only session",
              after && num(after->rows[0][0]) == 3.0 && s2 && s2->tables.size() == want_tables, "");
    }
}

// ── s005b: the door -- pure parts ─────────────────────────────────────────────
void test_journal_pure() {
    std::cout << "\n-- db: the change journal (pure) --\n";
    const char* reads[]  = {"SELECT 1", "  select * from t", "-- note\nSELECT 1", "/* x */ SHOW TABLES",
                            "EXPLAIN SELECT 1", "PRAGMA table_info(t)"};
    const char* writes[] = {"INSERT INTO t VALUES (1)", "CREATE TABLE t (x INTEGER)", "drop table t",
                            "PRAGMA foreign_keys = ON", "WITH x AS (SELECT 1) DELETE FROM t", "BEGIN",
                            "/* SELECT */ UPDATE t SET x = 1", "", "   "};
    bool ok = true; std::string bad;
    for (auto* r : reads)  if (!is_read(r)) { ok = false; bad = r; }
    for (auto* w : writes) if (is_read(w))  { ok = false; bad = w; }
    check("gate: reads pass, writes (incl. WITH .. DELETE, PRAGMA x = y) are writes", ok, bad);

    TableDef t;
    t.name = "notes";
    t.columns = {{"id", decl_for(Dialect::SQLite, FieldType::Integer), false, std::nullopt, 1, true},
                 {"body", decl_for(Dialect::SQLite, FieldType::Text), true, std::nullopt, 0, false}};
    const std::string lite = create_table_sql(Dialect::SQLite, '"', t);
    check("ddl: SQLite counting key inline",
          lite == "CREATE TABLE \"notes\" (\n  \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,\n  \"body\" TEXT\n)", lite);
    t.columns[0].decl_type = decl_for(Dialect::MariaDB, FieldType::Integer);
    t.columns[1].decl_type = decl_for(Dialect::MariaDB, FieldType::Text);
    const std::string maria = create_table_sql(Dialect::MariaDB, '`', t);
    check("ddl: MariaDB AUTO_INCREMENT + PRIMARY KEY",
          maria == "CREATE TABLE `notes` (\n  `id` BIGINT NOT NULL AUTO_INCREMENT,\n  `body` VARCHAR(255),\n"
                   "  PRIMARY KEY (`id`)\n)", maria);
    t.columns[0].decl_type = decl_for(Dialect::PostgreSQL, FieldType::Integer);
    t.columns[1].decl_type = decl_for(Dialect::PostgreSQL, FieldType::Text);
    const std::string pg = create_table_sql(Dialect::PostgreSQL, '"', t);
    check("ddl: PostgreSQL identity + PRIMARY KEY",
          pg == "CREATE TABLE \"notes\" (\n  \"id\" bigint NOT NULL GENERATED BY DEFAULT AS IDENTITY,\n"
                "  \"body\" text,\n  PRIMARY KEY (\"id\")\n)", pg);
    TableDef weird{"we\"ird", {{"a b", "TEXT", true, std::string("'x'"), 0, false}}, 0};
    check("ddl: names quoted, defaults kept",
          create_table_sql(Dialect::SQLite, '"', weird) == "CREATE TABLE \"we\"\"ird\" (\n  \"a b\" TEXT DEFAULT 'x'\n)", "");
    check("ddl: drop", drop_table_sql('`', "a`b") == "DROP TABLE `a``b`", "");

    Schema s; s.tables.push_back({}); s.tables[0].name = "Customers";
    TableDef n{"notes", {{"id", "INTEGER", true, std::nullopt, 0, false}}, 0};
    check("new table: a good one passes", check_new_table(n, s).empty(), check_new_table(n, s));
    auto bad_name = n; bad_name.name = "";
    auto spaced = n; spaced.name = " notes";
    auto taken = n; taken.name = "customers";
    auto internal = n; internal.name = "sqlite_stuff";
    auto empty = n; empty.columns.clear();
    auto dup = n; dup.columns.push_back({"ID", "TEXT", true, std::nullopt, 0, false});
    auto unnamed = n; unnamed.columns.push_back({"", "TEXT", true, std::nullopt, 0, false});
    auto longname = n; longname.name = std::string(65, 'x');
    check("new table: refuses no name / edge spaces / taken (any case) / sqlite_ / no columns / "
          "duplicate column (any case) / unnamed column / > 64 bytes",
          !check_new_table(bad_name, s).empty() && !check_new_table(spaced, s).empty() &&
          !check_new_table(taken, s).empty() && !check_new_table(internal, s).empty() &&
          !check_new_table(empty, s).empty() && !check_new_table(dup, s).empty() &&
          !check_new_table(unnamed, s).empty() && !check_new_table(longname, s).empty(), "");
}

// ── s006: record ids ─────────────────────────────────────────────────────────
void test_row_ids() {
    auto round = [](const std::string& t, std::vector<Value> k) {
        const auto id = Store::row_id(t, k);
        std::string t2;
        std::vector<Value> k2;
        return !id.empty() && Store::parse_row_id(id, t2, k2) && t2 == t && k2 == k;
    };
    check("row id: integer key round-trips", round("customers", {Value(std::int64_t{7})}), Store::row_id("customers", {Value(7)}));
    check("row id: text with : ; and quotes, a table with : in its name",
          round("odd:table;x", {Value("a:b;c\"d"), Value(std::int64_t{-3})}), "");
    check("row id: composite (text, real, int) round-trips",
          round("t", {Value("k"), Value(2.5), Value(std::int64_t{9})}), "");
    check("row id: empty text key round-trips", round("t", {Value("")}), "");
    check("row id: NULL / blob / no key -> no id",
          Store::row_id("t", {Value()}).empty() && Store::row_id("t", {Value(Blob{1})}).empty() &&
          Store::row_id("t", {}).empty(), "");
    std::string t; std::vector<Value> k;
    check("row id: garbage is not a row id",
          !Store::parse_row_id("table:x", t, k) && !Store::parse_row_id("row:99:x:i1;", t, k) &&
          !Store::parse_row_id("row:1:x:q1;", t, k) && !Store::parse_row_id("row:1:x:", t, k) &&
          !Store::parse_row_id("row:1:x:i1", t, k), "");
    check("row id: describe() says table and key",
          Store::describe(Store::row_id("customers", {Value(7)})) == "“customers” record 7",
          Store::describe(Store::row_id("customers", {Value(7)})));
}

// s006: records through the door -- every engine.
void test_records(Journal& j, Gate& g, Engine* e, const ConnectParams& ro, const std::string& P) {
    const Dialect d = e->dialect();
    TableDef t;
    t.name = "acorn_people";
    t.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"name", decl_for(d, FieldType::Text), false, std::nullopt, 0, false},
                 {"age", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}};
    auto mk = verbs::new_table(j, t);
    check(P + "records: fixture table", mk.has_value(), mk ? "" : mk.error().message);
    if (!mk) return;

    auto a = verbs::new_record(j, "acorn_people", {"name", "age"}, {Value("Ada"), Value(36)});
    auto b = verbs::new_record(j, "acorn_people", {"name"}, {Value("Grace")});
    check(P + "records: New Record learns the engine's key (1, 2)",
          a && b && *a == Store::row_id("acorn_people", {Value(1)}) && *b == Store::row_id("acorn_people", {Value(2)}),
          a ? (b ? *b : b.error().message) : a.error().message);
    if (!a || !b) return;
    check(P + "records: the label names the table", j.undo_label() == "New Record in “acorn_people”", j.undo_label());

    const auto steps = j.size();
    auto same = verbs::set_field(j, *a, "name", Value("Ada"));
    check(P + "records: a field set to what it holds is no step", same && j.size() == steps,
          same ? "" : same.error().message);
    auto ed = verbs::set_field(j, *a, "age", Value(37));
    auto age = g.query("SELECT " + g.quote_ident("age") + " FROM " + g.quote_ident("acorn_people") + " WHERE " +
                       g.quote_ident("id") + " = 1");
    check(P + "records: Edit writes one field, one step, named",
          ed && j.size() == steps + 1 && age && num(age->rows[0][0]) == 37.0 && j.undo_label() == "Edit “age”",
          ed ? j.undo_label() : ed.error().message);

    auto key = verbs::set_field(j, *a, "id", Value(99));
    check(P + "records: a key field is refused, no step", !key && j.size() == steps + 1,
          key ? "changed!" : key.error().message);
    auto nul = verbs::set_field(j, *a, "name", Value());
    auto name = g.query("SELECT " + g.quote_ident("name") + " FROM " + g.quote_ident("acorn_people") + " WHERE " +
                        g.quote_ident("id") + " = 1");
    check(P + "records: an engine refusal (NOT NULL) rolls back, no step, the reason returned",
          !nul && j.size() == steps + 1 && name && name->rows[0][0] == Value("Ada") && !nul.error().message.empty(),
          nul ? "written!" : nul.error().message);

    // Another tool changes the record: undo refuses, touches nothing.
    (void)g.inner().query("UPDATE " + g.quote_ident("acorn_people") + " SET " + g.quote_ident("age") +
                          " = 50 WHERE " + g.quote_ident("id") + " = 1");
    auto ur = j.undo();
    auto age2 = g.query("SELECT " + g.quote_ident("age") + " FROM " + g.quote_ident("acorn_people") + " WHERE " +
                        g.quote_ident("id") + " = 1");
    check(P + "records: undo refused when the record changed since -- nothing touched",
          !ur && ur.error().code == Error::Code::Changed && age2 && num(age2->rows[0][0]) == 50.0 && j.can_undo(),
          ur ? "undone!" : ur.error().message);
    (void)g.inner().query("UPDATE " + g.quote_ident("acorn_people") + " SET " + g.quote_ident("age") +
                          " = 37 WHERE " + g.quote_ident("id") + " = 1");
    auto ok = j.undo();
    auto age3 = g.query("SELECT " + g.quote_ident("age") + " FROM " + g.quote_ident("acorn_people") + " WHERE " +
                        g.quote_ident("id") + " = 1");
    check(P + "records: put back as the step left it, undo runs", ok && age3 && num(age3->rows[0][0]) == 36.0,
          ok ? "" : ok.error().message);

    // Delete, then another tool takes the key: redo refuses... and undo of the
    // delete refuses while the key is taken.
    auto del = verbs::delete_record(j, *b);
    check(P + "records: Delete Record removes it, named",
          del && !j.store().get(*b) && j.undo_label() == "Delete Record from “acorn_people”",
          del ? j.undo_label() : del.error().message);
    (void)g.inner().query("INSERT INTO " + g.quote_ident("acorn_people") + " (" + g.quote_ident("id") + ", " +
                          g.quote_ident("name") + ") VALUES (2, 'Impostor')");
    auto ud = j.undo();
    auto who = g.query("SELECT " + g.quote_ident("name") + " FROM " + g.quote_ident("acorn_people") + " WHERE " +
                       g.quote_ident("id") + " = 2");
    check(P + "records: undo of a delete refused when the key was taken since -- the other record kept",
          !ud && ud.error().code == Error::Code::Changed && who && who->rows.size() == 1 &&
          who->rows[0][0] == Value("Impostor"), ud ? "undone!" : ud.error().message);
    (void)g.inner().query("DELETE FROM " + g.quote_ident("acorn_people") + " WHERE " + g.quote_ident("id") + " = 2");
    auto ud2 = j.undo();
    auto back = j.store().get(*b);
    check(P + "records: the key free again, undo brings Grace back exactly",
          ud2 && back && std::get<RowDef>(*back).field("name") && *std::get<RowDef>(*back).field("name") == Value("Grace"),
          ud2 ? "" : ud2.error().message);

    // A table with no primary key: read, never written.
    (void)j.run("No key", {Store::table_id("acorn_nokey")}, [&](Store& s) {
        s.put(Store::table_id("acorn_nokey"), TableDef{"acorn_nokey", {{"x", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}}, 0});
    });
    auto nk = verbs::new_record(j, "acorn_nokey", {"x"}, {Value(1)});
    auto nkc = g.query("SELECT COUNT(*) FROM " + g.quote_ident("acorn_nokey"));
    check(P + "records: a table with no key is refused in words, nothing written",
          !nk && nk.error().code == Error::Code::Unsupported && nk.error().message.find("no primary key") != std::string::npos &&
          nkc && num(nkc->rows[0][0]) == 0.0, nk ? "written!" : nk.error().message);

    // A step that makes a record but never declared it.
    auto sneak = j.run("Sneaky record", {Store::table_id("acorn_people")}, [&](Store& s) {
        (void)s.insert("acorn_people", {"name"}, {Value("Sneak")});
    });
    auto sc = g.query("SELECT COUNT(*) FROM " + g.quote_ident("acorn_people") + " WHERE " + g.quote_ident("name") + " = 'Sneak'");
    check(P + "records: a record made without declaring new_row_id is refused",
          !sneak && sc && num(sc->rows[0][0]) == 0.0, sneak ? "made!" : sneak.error().message);

    // Read-only: refused before anything runs.
    if (auto r2 = e->open(ro)) {
        Gate rg(std::move(*r2));
        Journal rj(rg, d, e->caps());
        auto w = verbs::new_record(rj, "acorn_people", {"name"}, {Value("Nope")});
        check(P + "records: read-only connection refuses New Record", !w && w.error().code == Error::Code::ReadOnly, "");
    }
    // Leave nothing behind for the tests after.
    (void)g.inner().query("DROP TABLE " + g.quote_ident("acorn_people"));
    (void)g.inner().query("DROP TABLE " + g.quote_ident("acorn_nokey"));
    j.clear();
}

// ── s006b: browse ────────────────────────────────────────────────────────────
void test_browse_pure() {
    check("browse: cell text -- NULL empty, int, real, text, yes/no, blob",
          cell_text(Value(), FieldType::Text).empty() && cell_text(Value(42), FieldType::Integer) == "42" &&
          cell_text(Value(12.5), FieldType::Number) == "12.5" && cell_text(Value(0.1), FieldType::Number) == "0.1" &&
          cell_text(Value("hi"), FieldType::Text) == "hi" && cell_text(Value(1), FieldType::Boolean) == "Yes" &&
          cell_text(Value(0), FieldType::Boolean) == "No" && cell_text(Value("t"), FieldType::Boolean) == "Yes" &&
          cell_text(Value(Blob{1, 2, 3}), FieldType::Container) == "(3 bytes)" &&
          cell_text(Value(Blob{9}), FieldType::Container) == "(1 byte)", cell_text(Value(0.1), FieldType::Number));
    Table t;
    t.name = "people";
    t.columns = {{"id", "INTEGER", FieldType::Integer, false, std::nullopt, 1, true},
                 {"age", "INTEGER", FieldType::Integer, true, std::nullopt, 0, false},
                 {"name", "TEXT", FieldType::Text, true, std::nullopt, 0, false}};
    check("browse: a card leads with the first non-key text, else #key",
          card_title(t, {Value(3), Value(40), Value("Ada")}) == "Ada" &&
          card_title(t, {Value(3), Value(40), Value()}) == "#3" && card_title(t, {Value(3), Value(40), Value("")}) == "#3",
          card_title(t, {Value(3), Value(40), Value()}));
    Page p; p.total = 312; p.offset = 50; p.rows.resize(50);
    Page e;
    Column age{"age", "INTEGER", FieldType::Integer, true, std::nullopt, 0, false};
    Column need{"qty", "INTEGER", FieldType::Integer, false, std::nullopt, 0, false};
    Column bal{"balance", "REAL", FieldType::Number, true, std::nullopt, 0, false};
    Column yes{"active", "BOOLEAN", FieldType::Boolean, true, std::nullopt, 0, false};
    Column nm{"name", "TEXT", FieldType::Text, false, std::nullopt, 0, false};
    Column nt{"notes", "TEXT", FieldType::Text, true, std::nullopt, 0, false};
    Column ph{"photo", "BLOB", FieldType::Container, true, std::nullopt, 0, false};
    check("edit: typed -> value (int, number, yes/no, text keeps spaces, empty -> NULL / \"\")",
          parse_field(" 42 ", age) == Value(42) && parse_field("12.5", bal) == Value(12.5) &&
          parse_field("Yes", yes) == Value(1) && parse_field("false", yes) == Value(0) &&
          parse_field(" Ada ", nm) == Value(" Ada ") && parse_field("", age) == Value() &&
          parse_field("", nt) == Value() && parse_field("", nm) == Value(""), "");
    check("edit: refused in words (not a number, 4x, maybe, empty NOT NULL number, a blob)",
          !parse_field("abc", age) && parse_field("4x", age).error() == "\u201Cage\u201D needs a whole number" &&
          !parse_field("1.2.3", bal) && !parse_field("maybe", yes) && !parse_field("", need) &&
          !parse_field("x", ph), "");
    check("browse: the range line", page_range(p) == "51–100 of 312" && page_range(e) == "No records", page_range(p));
}

void test_browse(Journal& j, Gate& g, Engine* e, const std::string& P) {
    const Dialect d = e->dialect();
    TableDef t;
    t.name = "acorn_browse";
    t.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"name", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"rank", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}};
    if (auto mk = verbs::new_table(j, t); !mk) { check(P + "browse: fixture", false, mk.error().message); return; }
    const char* names[] = {"Grace", "Ada", "Linus", "Barbara", "Ken", "Edsger", "Frances"};
    int rank = 0;
    for (const char* n : names) (void)verbs::new_record(j, "acorn_browse", {"name", "rank"}, {Value(n), Value(rank++ % 3)});
    auto s = g.introspect();
    const Table* bt = s ? s->table("acorn_browse") : nullptr;
    if (!bt) { check(P + "browse: fixture read back", false, ""); return; }

    auto p1 = read_page(g, *bt, {0, 3});
    auto p3 = read_page(g, *bt, {6, 3});
    check(P + "browse: pages of 3 -- total 7, key order, last page short",
          p1 && p3 && p1->total == 7 && p1->rows.size() == 3 && p3->rows.size() == 1 &&
          p1->rows[0][1] == Value("Grace") && p3->rows[0][1] == Value("Frances") && page_range(*p1) == "1–3 of 7",
          p1 ? page_range(*p1) : p1.error().message);
    check(P + "browse: each record carries its journal id",
          p1 && p1->ids.size() == 3 && p1->ids[1] == Store::row_id("acorn_browse", {Value(2)}), p1 && !p1->ids.empty() ? p1->ids[0] : "");
    auto byname = read_page(g, *bt, {0, 7, "name"});
    auto desc = read_page(g, *bt, {0, 2, "name", true});
    check(P + "browse: sorted by a column, both ways",
          byname && desc && byname->rows[0][1] == Value("Ada") && byname->rows[6][1] == Value("Linus") &&
          desc->rows[0][1] == Value("Linus") && desc->rows[1][1] == Value("Ken"), "");
    // Ties on the sort column fall back to the key: pages never shuffle.
    auto r1 = read_page(g, *bt, {0, 4, "rank"});
    auto r2 = read_page(g, *bt, {4, 4, "rank"});
    bool stable = r1 && r2 && r1->rows.size() == 4 && r2->rows.size() == 3;
    std::vector<std::int64_t> seen;
    if (stable) {
        for (auto& row : r1->rows) seen.push_back(*row[0].as_int());
        for (auto& row : r2->rows) seen.push_back(*row[0].as_int());
        std::vector<std::int64_t> want = {1, 4, 7, 2, 5, 3, 6};
        stable = seen == want;
    }
    check(P + "browse: equal sort values keep key order across pages", stable, "");
    auto past = read_page(g, *bt, {100, 10});
    check(P + "browse: past the end is an empty page, not an error", past && past->rows.empty() && past->total == 7, "");
    check(P + "browse: reading is not a write (no strays)", j.strays() == 1, std::to_string(j.strays()));
    (void)g.inner().query("DROP TABLE " + g.quote_ident("acorn_browse"));
    j.clear();
}

// ── s005b: the door, on a live fixture -- every engine ────────────────────────
std::vector<TableDef> tables_of(Connection& c) {
    std::vector<TableDef> out;
    if (auto s = c.introspect())
        for (auto& t : s->tables) out.push_back(TableDef::from(t));
    return out;
}
// The whole database as data: every table's shape and every row (s006 -- the
// record verbs change data, not shape).
struct Snapshot {
    std::vector<TableDef> tables;
    std::vector<std::vector<Row>> rows;
    bool operator==(const Snapshot&) const = default;
};
Snapshot snapshot(Connection& c) {
    Snapshot s;
    s.tables = tables_of(c);
    for (const auto& t : s.tables) {
        auto r = c.query("SELECT * FROM " + c.quote_ident(t.name) + " ORDER BY 1");
        s.rows.push_back(r ? r->rows : std::vector<Row>{});
    }
    return s;
}
bool has_table(Connection& c, const std::string& n) {
    auto s = c.introspect();
    return s && s->table(n);
}

void test_journal(Engine* e, const ConnectParams& rw, const ConnectParams& ro, const std::string& P0) {
    const std::string P = P0 + ": journal: ";
    auto opened = e->open(rw);
    check(P + "open read-write fixture", opened.has_value(), opened ? "" : opened.error().message);
    if (!opened) return;
    Gate g(std::move(*opened));
    Journal j(g, e->dialect(), e->caps());
    const Dialect d = e->dialect();

    // The gate: a write with the door shut is refused, counted, and not made.
    auto stray = g.query("CREATE TABLE acorn_stray (x INTEGER)");
    check(P + "the gate refuses a write outside the door, and counts it",
          !stray && stray.error().code == Error::Code::ReadOnly && g.strays() == 1 &&
          !has_table(g, "acorn_stray"), stray ? "written!" : stray.error().message);
    check(P + "the gate lets reads through", g.query("SELECT 1").has_value(), "");

    // Walk every verb the UI offers: a step, nothing strays, undo puts it back
    // exactly, redo reproduces it exactly.
    for (const auto& v : verbs::all()) {
        if (v.setup) {
            auto su = v.setup(j);
            check(P + "verb " + v.name + ": its fixture, through the door", su.has_value(), su ? "" : su.error().message);
        }
        const auto before = snapshot(g);
        const auto steps = j.size();
        auto r = v.run(j);
        const auto after = snapshot(g);
        check(P + "verb " + v.name + ": runs, is one step, changes the database",
              r.has_value() && j.size() == steps + 1 && j.can_undo() && after != before,
              r ? "" : r.error().message);
        auto u = j.undo();
        check(P + "verb " + v.name + ": undo puts the schema back exactly",
              u.has_value() && snapshot(g) == before && j.can_redo(), u ? "" : u.error().message);
        auto re = j.redo();
        check(P + "verb " + v.name + ": redo reproduces it exactly",
              re.has_value() && snapshot(g) == after, re ? "" : re.error().message);
        (void)j.undo();
    }
    check(P + "no verb wrote outside the door", j.strays() == 1, std::to_string(j.strays()) + " strays");

    // New Table, by hand: the label names it; the table is real and empty.
    TableDef t;
    t.name = "notes";
    t.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"body", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"done", decl_for(d, FieldType::Boolean), true, std::nullopt, 0, false}};
    auto made = verbs::new_table(j, t);
    const auto shown = g.introspect();
    const Table* nt = shown ? shown->table("notes") : nullptr;
    check(P + "New Table makes the table, key counting, columns in order",
          made && nt && nt->columns.size() == 3 && nt->columns[0].auto_increment &&
          nt->primary_key() == std::vector<std::string>{"id"} && nt->columns[2].name == "done",
          made ? "" : made.error().message);
    check(P + "the step is named", j.undo_label() == "New Table “notes”", j.undo_label());
    auto ins = g.inner().query("INSERT INTO " + g.quote_ident("notes") + " (" + g.quote_ident("body") + ") VALUES (?)",
                               {Value("hello")});
    check(P + "the new table takes a row (another tool writing)", ins.has_value(), ins ? "" : ins.error().message);

    // Undo is refused while the table holds data it did not make.
    const auto steps = j.size();
    auto refused = j.undo();
    check(P + "undo refused: the table changed since (it holds a row) -- nothing touched",
          !refused && refused.error().code == Error::Code::Changed && has_table(g, "notes") &&
          j.can_undo() && j.size() == steps, refused ? "undone!" : refused.error().message);
    (void)g.inner().query("DELETE FROM " + g.quote_ident("notes"));
    auto undone = j.undo();
    check(P + "emptied again, undo drops it", undone && !has_table(g, "notes"),
          undone ? "" : undone.error().message);

    // Redo is refused when something else took the name meanwhile.
    (void)g.inner().query("CREATE TABLE " + g.quote_ident("notes") + " (x INTEGER)");
    auto rr = j.redo();
    check(P + "redo refused: the name was taken since -- the other table untouched",
          !rr && rr.error().code == Error::Code::Changed && j.can_redo(),
          rr ? "redone!" : rr.error().message);
    const auto other = g.introspect();
    check(P + "... and it is still the other table", other && other->table("notes") &&
          other->table("notes")->columns.size() == 1, "");
    (void)g.inner().query("DROP TABLE " + g.quote_ident("notes"));
    auto rr2 = j.redo();
    check(P + "the name free again, redo makes it", rr2 && has_table(g, "notes"), rr2 ? "" : rr2.error().message);

    // A failure mid-step is rolled back: no step, nothing made.
    const auto before_fail = tables_of(g);
    const auto n_before = j.size();
    TableDef broken{"acorn_broken", {{"x", "NOT A TYPE(((", true, std::nullopt, 0, false}}, 0};
    auto f = verbs::new_table(j, broken);
    check(P + "an engine error: no step, nothing made, the reason returned",
          !f && j.size() == n_before && tables_of(g) == before_fail && !f.error().message.empty(),
          f ? "made!" : f.error().message);
    auto dup = verbs::new_table(j, t);   // "notes" exists
    check(P + "a name that exists: refused, no step", !dup && j.size() == n_before && tables_of(g) == before_fail,
          dup ? "made!" : dup.error().message);

    // Two tables in one step: atomic where DDL rolls back; refused up front
    // where it can't.
    const auto ids2 = std::vector<Store::Id>{Store::table_id("acorn_a"), Store::table_id("acorn_b")};
    auto two = j.run("Two", ids2, [&](Store& s) {
        s.put(ids2[0], TableDef{"acorn_a", {{"x", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}}, 0});
        s.put(ids2[1], TableDef{"acorn_b", {{"x", "NOT A TYPE(((", true, std::nullopt, 0, false}}, 0});
    });
    if (e->caps().transactional_ddl)
        check(P + "a step that fails half-way rolls back its first half", !two && !has_table(g, "acorn_a") &&
              j.size() == n_before, two ? "made!" : two.error().message);
    else
        check(P + "no transactional DDL: a two-table step is refused before it runs",
              !two && two.error().code == Error::Code::Unsupported && !has_table(g, "acorn_a") &&
              j.size() == n_before, two ? "made!" : two.error().message);
    auto undeclared = j.run("Sneaky", {Store::table_id("acorn_c")}, [&](Store& s) {
        s.put(Store::table_id("acorn_d"), TableDef{"acorn_d", {{"x", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}}, 0});
    });
    check(P + "a step writes only what it declared", !undeclared && !has_table(g, "acorn_d"), "");

    // The data rule: the store never drops a table holding rows.
    (void)g.inner().query("INSERT INTO " + g.quote_ident("notes") + " (" + g.quote_ident("body") + ") VALUES ('kept')");
    auto drop = j.run("Drop", {Store::table_id("notes")}, [&](Store& s) { s.put(Store::table_id("notes"), std::nullopt); });
    auto cnt = g.query("SELECT COUNT(*) FROM " + g.quote_ident("notes"));
    check(P + "never drops a table with data in it", !drop && cnt && num(cnt->rows[0][0]) == 1.0,
          drop ? "dropped!" : drop.error().message);
    check(P + "still exactly one stray (the deliberate one)", j.strays() == 1, std::to_string(j.strays()));

    // A read-only connection: the journal refuses before anything runs.
    if (auto r2 = e->open(ro)) {
        Gate rg(std::move(*r2));
        Journal rj(rg, d, e->caps());
        TableDef x{"acorn_ro", {{"x", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}}, 0};
        auto w = verbs::new_table(rj, x);
        check(P + "read-only connection: refused, no step", !w && w.error().code == Error::Code::ReadOnly &&
              rj.size() == 0 && !has_table(g, "acorn_ro"), w ? "made!" : w.error().message);
    } else {
        check(P + "reopen read-only", false, r2.error().message);
    }
    (void)g.inner().query("DROP TABLE " + g.quote_ident("notes"));
    test_records(j, g, e, ro, P);
    test_browse(j, g, e, P);
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
    test_journal(e, T.rw, T.ro, e->id());   // s005b: the door, in the same throwaway database

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
    test_journal_pure();
    test_row_ids();
    test_browse_pure();
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
        {   // s005b: the door, on its own fresh fixture file
            Target J = T;
            J.rw.path = (tmp.p / "journal.sqlite").string();
            J.ro.path = J.rw.path;
            test_journal(e, J.rw, J.ro, "sqlite");
        }
        // The file-level proof: a read-only session leaves the bytes alone.
        const std::string before = slurp(T.rw.path);
        { auto ro = e->open(T.ro); if (ro) (void)(*ro)->introspect(); }
        check("sqlite: the file is byte-identical after a read-only session", slurp(T.rw.path) == before, "");

        // s005a -- New Database File: makes a real, empty database; NEVER
        // touches anything already at the path.
        {
            const fs::path made = tmp.p / "new.sqlite";
            auto r = new_file(*e, made);
            check("new file: created", r.has_value() && fs::exists(made), r ? "" : r.error().message);
            const std::string bytes = slurp(made);
            check("new file: a real SQLite file, not zero bytes",
                  bytes.size() >= 100 && bytes.rfind("SQLite format 3", 0) == 0, std::to_string(bytes.size()) + " bytes");
            bool stamped = false, empty = false;
            if (auto ro = e->open(ConnectParams{.path = made.string()})) {
                if (auto q = (*ro)->query("PRAGMA application_id"); q && !q->rows.empty())
                    stamped = q->rows[0][0].kind() == Value::Kind::Integer &&
                              q->rows[0][0].as_int() == std::int64_t{kAcornApplicationId};
                if (auto s = (*ro)->introspect()) empty = s->tables.empty();
            }
            check("new file: stamped with Acorn's application id", stamped, "");
            check("new file: opens read-only and has no tables", empty, "");

            const std::string fixture_before = slurp(T.rw.path);
            auto again = new_file(*e, T.rw.path);
            check("new file: refuses an existing database, bytes untouched",
                  !again && again.error().code == Error::Code::Exists && slurp(T.rw.path) == fixture_before,
                  again ? "created!" : again.error().message);
            const fs::path blank = tmp.p / "blank.sqlite";
            { std::ofstream(blank) << ""; }
            auto b = new_file(*e, blank);
            check("new file: refuses even an empty file", !b && fs::file_size(blank) == 0, "");
            auto d = new_file(*e, tmp.p);
            check("new file: refuses a directory", !d && d.error().code == Error::Code::Exists, "");
            auto nf = new_file(*e, tmp.p / "no" / "such" / "dir.sqlite");
            check("new file: a missing folder is refused, nothing made",
                  !nf && nf.error().code == Error::Code::NotFound && !fs::exists(tmp.p / "no"), "");
        }
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
