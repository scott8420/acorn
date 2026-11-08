// selftest_db.cpp -- the database layer, headless (s002, s003).
//
// RULES "the data is sacred": every database here is a fixture this file
// creates and removes -- a temp SQLite file, or a throwaway database named
// acorn_selftest_<pid> on a server named by ACORN_TEST_MARIADB /
// ACORN_TEST_POSTGRES (see scripts/acorn_servers.sh). One suite runs against
// every engine; an engine that can't run is reported SKIPPED, never a pass.
#include "db/Engine.hpp"
#include "db/Journal.hpp"
#include "db/Starter.hpp"
#include "db/Browse.hpp"
#include "db/Print.hpp"
#include "db/Script.hpp"
#include "db/Export.hpp"
#include "db/Import.hpp"
#include "db/Accounts.hpp"
#include <sstream>
#include "core/Print.hpp"
#include "db/Convert.hpp"
#include "db/Loader.hpp"
#include "db/NewFile.hpp"
#include "db/Types.hpp"

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>
#include <set>
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
// ── s007: the designer's SQL ─────────────────────────────────────────────────
void test_design_pure() {
    TableDef live;
    live.name = "people";
    live.columns = {{"id", "INTEGER", false, std::nullopt, 1, true},
                    {"name", "TEXT", true, std::nullopt, 0, false},
                    {"age", "INTEGER", true, std::nullopt, 0, false}};
    live.indexes = {{"people_age", {"age"}, false, false}};

    TableDef add = live;
    add.columns.push_back({"email", "TEXT", true, std::nullopt, 0, false});
    auto pa = plan_alter(Dialect::SQLite, '"', live, add);
    check("design: a new field is ADD COLUMN, one statement",
          pa && pa->size() == 1 && (*pa)[0] == "ALTER TABLE \"people\" ADD COLUMN \"email\" TEXT",
          pa && !pa->empty() ? (*pa)[0] : (pa ? "" : pa.error()));

    TableDef ren = live;
    ren.columns[2].name = "years";
    ren.indexes[0].columns = {"years"};
    auto pr = plan_alter(Dialect::PostgreSQL, '"', live, ren);
    check("design: a field whose name alone changed is a RENAME; its index follows, no index SQL",
          pr && pr->size() == 1 && (*pr)[0] == "ALTER TABLE \"people\" RENAME COLUMN \"age\" TO \"years\"",
          pr && !pr->empty() ? (*pr)[0] : (pr ? "" : pr.error()));

    TableDef del = live;
    del.columns.erase(del.columns.begin() + 2);
    del.indexes.clear();
    auto pd = plan_alter(Dialect::SQLite, '"', live, del);
    check("design: delete a field -- its index dropped FIRST, then the column",
          pd && pd->size() == 2 && (*pd)[0] == "DROP INDEX \"people_age\"" &&
          (*pd)[1] == "ALTER TABLE \"people\" DROP COLUMN \"age\"", "");
    auto pm = plan_alter(Dialect::MariaDB, '`', live, del);
    check("design: MariaDB gets ONE ALTER TABLE (can't roll DDL back)",
          pm && pm->size() == 1 && (*pm)[0] == "ALTER TABLE `people`\n  DROP INDEX `people_age`,\n  DROP COLUMN `age`",
          pm && !pm->empty() ? (*pm)[0] : "");
    // ... and undo of it is the mirror: column back, then the index.
    auto pu = plan_alter(Dialect::SQLite, '"', del, live);
    check("design: undo of delete -- the column added back, then its index made",
          pu && pu->size() == 2 && (*pu)[0].find("ADD COLUMN \"age\" INTEGER") != std::string::npos &&
          (*pu)[1] == "CREATE INDEX \"people_age\" ON \"people\" (\"age\")", "");

    {   // s007e -- a field coming back BEFORE fields the table keeps goes in its place.
    TableDef mid = live;
    mid.columns.erase(mid.columns.begin() + 1);   // "name", between id and age
    auto ps = plan_alter(Dialect::SQLite, '"', mid, live);
    const std::string ps0 = ps && !ps->empty() ? (*ps)[0] : (ps ? "" : ps.error());
    const auto pi = ps0.find("\"id\""), pn = ps0.find("\"name\""), pg = ps0.find("\"age\"");
    bool ps_seq = false, ps_ix = false;
    if (ps) for (auto& st : *ps) {
        if (st.find("sqlite_sequence") != std::string::npos) ps_seq = true;
        if (st == "CREATE INDEX \"people_age\" ON \"people\" (\"age\")") ps_ix = true;
    }
    check("design (s007e): SQLite -- a field back in the middle makes the table again in the picture's order "
          "(id count carried, indexes made again)",
          ps && ps0.rfind("CREATE TABLE \"acorn_rebuild_people\"", 0) == 0 && pi < pn && pn < pg &&
          pg != std::string::npos && ps_seq && ps_ix, ps0);
    auto pf = plan_alter(Dialect::SQLite, '"', mid, live, false);
    check("design (s007e): SQLite -- when the table can't be made again, ADD COLUMN at the end (one statement)",
          pf && pf->size() == 1 && (*pf)[0] == "ALTER TABLE \"people\" ADD COLUMN \"name\" TEXT",
          pf && !pf->empty() ? (*pf)[0] : "");
    auto pma = plan_alter(Dialect::MariaDB, '`', mid, live);
    check("design (s007e): MariaDB -- ADD COLUMN .. AFTER the field before it",
          pma && pma->size() == 1 && (*pma)[0] == "ALTER TABLE `people`\n  ADD COLUMN `name` TEXT AFTER `id`",
          pma && !pma->empty() ? (*pma)[0] : "");
    TableDef first = live;
    first.columns.insert(first.columns.begin(), {"email", "TEXT", true, std::nullopt, 0, false});
    auto pmf = plan_alter(Dialect::MariaDB, '`', live, first);
    check("design (s007e): MariaDB -- a field first is ADD COLUMN .. FIRST",
          pmf && pmf->size() == 1 && (*pmf)[0] == "ALTER TABLE `people`\n  ADD COLUMN `email` TEXT FIRST",
          pmf && !pmf->empty() ? (*pmf)[0] : "");
    auto ppg = plan_alter(Dialect::PostgreSQL, '"', mid, live);
    check("design (s007e): PostgreSQL -- can only add at the end; no rebuild",
          ppg && ppg->size() == 1 && (*ppg)[0] == "ALTER TABLE \"people\" ADD COLUMN \"name\" TEXT",
          ppg && !ppg->empty() ? (*ppg)[0] : "");
    auto pend = plan_alter(Dialect::MariaDB, '`', live, add);
    check("design (s007e): a field at the end stays a plain ADD COLUMN (no AFTER)",
          pend && pend->size() == 1 && (*pend)[0] == "ALTER TABLE `people`\n  ADD COLUMN `email` TEXT",
          pend && !pend->empty() ? (*pend)[0] : "");
    }

    TableDef keep_ix = del;
    keep_ix.indexes = live.indexes;
    auto pk = plan_alter(Dialect::SQLite, '"', live, keep_ix);
    check("design: refused -- an index kept on a field that is going",
          !pk && pk.error().find("people_age") != std::string::npos, pk ? "planned!" : pk.error());

    TableDef req = live;
    req.columns.push_back({"code", "TEXT", false, std::nullopt, 0, false});
    auto pq = plan_alter(Dialect::SQLite, '"', live, req);
    TableDef reqd = live;
    reqd.columns.push_back({"code", "TEXT", false, std::string("'x'"), 0, false});
    auto pq2 = plan_alter(Dialect::SQLite, '"', live, reqd);
    check("design: a required new field needs a default -- refused without, written with",
          !pq && pq.error().find("needs a default") != std::string::npos && pq2 &&
          (*pq2)[0].ends_with("\"code\" TEXT NOT NULL DEFAULT 'x'"), pq ? "planned!" : pq.error());

    TableDef typ = live;
    typ.columns[1].decl_type = "INTEGER";
    auto pt = plan_alter(Dialect::SQLite, '"', live, typ);
    TableDef key = live;
    key.columns.erase(key.columns.begin());
    auto pky = plan_alter(Dialect::SQLite, '"', live, key);
    TableDef two = live;
    two.columns[1].name = "nm";
    two.columns.push_back({"more", "TEXT", true, std::nullopt, 0, false});
    auto p2 = plan_alter(Dialect::SQLite, '"', live, two);
    check("design: the key is refused in words (s007d); a type change is planned (s007c)",
          pt && !pky && pky.error().find("key") != std::string::npos, pky ? "planned!" : pky.error());

    // s007c -- field changes, per engine.
    TableDef chg = live;   // name: Text -> Integer, required, default 0
    chg.columns[1] = {"name", "INTEGER", false, std::string("0"), 0, false};
    auto sq = plan_alter(Dialect::SQLite, '"', live, chg);
    const std::vector<std::string> sq_want = {
        "CREATE TABLE \"acorn_rebuild_people\" (\n  \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "  \"name\" INTEGER NOT NULL DEFAULT 0,\n  \"age\" INTEGER\n)",
        "INSERT INTO \"acorn_rebuild_people\" (\"id\", \"name\", \"age\")\n  SELECT \"id\", COALESCE(\"name\", 0), \"age\" FROM \"people\"",
        "DELETE FROM sqlite_sequence WHERE name = 'acorn_rebuild_people'",
        "INSERT INTO sqlite_sequence (name, seq)\n  SELECT 'acorn_rebuild_people', seq FROM sqlite_sequence WHERE name = 'people'",
        "DROP TABLE \"people\"",
        "ALTER TABLE \"acorn_rebuild_people\" RENAME TO \"people\"",
        "-- then each record's \"name\" is written back as a whole number (Acorn's rules; what won't fit takes the default)",
        "CREATE INDEX \"people_age\" ON \"people\" (\"age\")"};
    check("change: SQLite makes the table again -- copy (empties filled), count kept, swap, values written back, indexes",
          sq && *sq == sq_want, sq ? (sq->size() > 1 ? (*sq)[1] : "") : sq.error());
    TableDef pgl = live, pgw = live;
    pgl.columns[1] = {"name", "text", true, std::string("'x'::text"), 0, false};
    pgw.columns[1] = {"name", "bigint", false, std::string("0"), 0, false};
    auto pg = plan_alter(Dialect::PostgreSQL, '"', pgl, pgw);
    const std::string A = "ALTER TABLE \"people\" ALTER COLUMN \"name\" ";
    const std::vector<std::string> pg_want = {
        A + "DROP DEFAULT", A + "TYPE bigint USING NULL::bigint",
        "-- then each record's \"name\" is written back as a whole number (Acorn's rules; what won't fit takes the default)",
        A + "SET DEFAULT 0", "UPDATE \"people\" SET \"name\" = 0 WHERE \"name\" IS NULL", A + "SET NOT NULL"};
    check("change: PostgreSQL -- empty it (USING NULL), write back, default, fill, then required",
          pg && *pg == pg_want, pg ? (pg->size() > 1 ? (*pg)[1] : "") : pg.error());
    TableDef pgd = pgl;
    pgd.columns[1].nullable = false;
    pgd.columns[1].default_sql = std::string("'none'");
    auto pg2 = plan_alter(Dialect::PostgreSQL, '"', pgl, pgd);
    check("change: PostgreSQL -- same type: just the default and required, empties filled first",
          pg2 && pg2->size() == 3 && (*pg2)[0] == A + "SET DEFAULT 'none'" &&
          (*pg2)[1] == "UPDATE \"people\" SET \"name\" = 'none' WHERE \"name\" IS NULL" && (*pg2)[2] == A + "SET NOT NULL",
          pg2 && !pg2->empty() ? (*pg2)[0] : "");
    TableDef myl = live, myw = live;
    myl.columns[2].decl_type = "bigint";
    myw.columns[2] = {"age", "varchar(255)", true, std::string("'?'"), 0, false};
    auto my = plan_alter(Dialect::MariaDB, '`', myl, myw);
    check("change: MariaDB -- ONE ALTER with MODIFY COLUMN, then Acorn's reading written where it differs",
          my && my->size() == 2 && (*my)[0] == "ALTER TABLE `people`\n  MODIFY COLUMN `age` varchar(255) DEFAULT '?'" &&
          (*my)[1].starts_with("-- then each record's `age` is written back as text"), my && !my->empty() ? (*my)[0] : "");
    TableDef rk = live;
    rk.columns[1].name = "nm";
    rk.columns[2].decl_type = "TEXT";
    auto prk = plan_alter(Dialect::SQLite, '"', live, rk);
    TableDef auto_ix = live;
    auto_ix.indexes.push_back({"sqlite_autoindex_people_1", {"name"}, true, false});
    TableDef auto_w = auto_ix;
    auto_w.columns[2].decl_type = "TEXT";
    auto pai = plan_alter(Dialect::SQLite, '"', auto_ix, auto_w);
    check("change: refused -- a rename with a change in one step; a SQLite table with a UNIQUE rule inside",
          !prk && prk.error().find("rename") != std::string::npos && !pai &&
          pai.error().find("UNIQUE") != std::string::npos, pai ? "planned!" : pai.error());
    check("change: labels -- one field changed is Change Field; with more, Change Table",
          verbs::design_label(live, chg) == "Change Field “name” in “people”" &&
          verbs::design_label(live, rk) == "Change Table “people”", verbs::design_label(live, chg));
    check("design: a name changed AND a field added is not a rename -- drop + adds",
          p2 && p2->size() == 3 && (*p2)[0].find("DROP COLUMN \"name\"") != std::string::npos, p2 ? "" : p2.error());

    TableDef ix = live;
    ix.indexes.push_back({"people_name", {"name", "age"}, true, false});
    auto pi = plan_alter(Dialect::PostgreSQL, '"', live, ix);
    TableDef part = live;
    part.indexes.push_back({"people_p", {"name"}, false, true});
    auto pp = plan_alter(Dialect::SQLite, '"', live, part);
    TableDef same = live;
    std::reverse(same.columns.begin(), same.columns.end());
    auto ps = plan_alter(Dialect::SQLite, '"', live, same);
    check("design: CREATE UNIQUE INDEX in key order; a partial one refused; same fields reordered = nothing to do",
          pi && pi->size() == 1 && (*pi)[0] == "CREATE UNIQUE INDEX \"people_name\" ON \"people\" (\"name\", \"age\")" &&
          !pp && ps && ps->empty() && same == live, "");
    check("design: drop index per dialect",
          drop_index_sql(Dialect::MariaDB, '`', "t", "i") == "DROP INDEX `i` ON `t`" &&
          drop_index_sql(Dialect::SQLite, '"', "t", "i") == "DROP INDEX \"i\"", "");
    check("design: labels -- one change gets its own name, more is Change Table",
          verbs::design_label(live, add) == "New Field \u201Cemail\u201D in \u201Cpeople\u201D" &&
          verbs::design_label(live, ren) == "Rename Field \u201Cage\u201D to \u201Cyears\u201D" &&
          verbs::design_label(live, del) == "Delete Field \u201Cage\u201D from \u201Cpeople\u201D" &&
          verbs::design_label(live, ix) == "New Index \u201Cpeople_name\u201D on \u201Cpeople\u201D" &&
          verbs::design_label(live, two) == "Change Table \u201Cpeople\u201D", verbs::design_label(live, del));
    std::string t, c;
    check("design: field ids round-trip (odd names, a ':' in both)",
          Store::parse_field_id(Store::field_id("a:b", "c:d"), t, c) && t == "a:b" && c == "c:d" &&
          !Store::parse_field_id("field:9:ab:c", t, c) && !Store::parse_field_id("row:1:a:i1;", t, c), t + "|" + c);
}

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
    auto round = [](const std::string& t, std::vector<std::string> f, std::vector<Value> k) {
        const auto id = Store::row_id(t, f, k);
        std::string t2;
        std::vector<std::string> f2;
        std::vector<Value> k2;
        return !id.empty() && Store::parse_row_id(id, t2, f2, k2) && t2 == t && f2 == f && k2 == k;
    };
    check("row id: integer key round-trips", round("customers", {"id"}, {Value(std::int64_t{7})}),
          Store::row_id("customers", {"id"}, {Value(7)}));
    check("row id: text with : ; = and quotes, a table and a key field with : in their names",
          round("odd:table;x", {"a:b", "c=d;"}, {Value("a:b;c\"d"), Value(std::int64_t{-3})}), "");
    check("row id: composite (text, real, int) round-trips",
          round("t", {"a", "b", "c"}, {Value("k"), Value(2.5), Value(std::int64_t{9})}), "");
    check("row id: empty text key round-trips", round("t", {"code"}, {Value("")}), "");
    check("row id: NULL / blob / no key / unpaired fields -> no id",
          Store::row_id("t", {"id"}, {Value()}).empty() && Store::row_id("t", {"id"}, {Value(Blob{1})}).empty() &&
          Store::row_id("t", {}, {}).empty() && Store::row_id("t", {"a", "b"}, {Value(1)}).empty() &&
          Store::row_id("t", {""}, {Value(1)}).empty(), "");
    std::string t; std::vector<Value> k;
    check("row id: garbage is not a row id",
          !Store::parse_row_id("table:x", t, k) && !Store::parse_row_id("row:99:x:2:id=i1;", t, k) &&
          !Store::parse_row_id("row:1:x:2:id=q1;", t, k) && !Store::parse_row_id("row:1:x:", t, k) &&
          !Store::parse_row_id("row:1:x:2:id=i1", t, k) && !Store::parse_row_id("row:1:x:i1;", t, k) &&
          !Store::parse_row_id("row:1:x:2:id=", t, k), "");
    check("row id: s007d -- the key's fields are part of the id",
          Store::row_id("t", {"id"}, {Value(5)}) != Store::row_id("t", {"n"}, {Value(5)}), "");
    check("row id: describe() says table and key",
          Store::describe(Store::row_id("customers", {"id"}, {Value(7)})) == "“customers” record 7",
          Store::describe(Store::row_id("customers", {"id"}, {Value(7)})));
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
          a && b && *a == Store::row_id("acorn_people", {"id"}, {Value(1)}) && *b == Store::row_id("acorn_people", {"id"}, {Value(2)}),
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
// s007c -- Acorn's own conversion rules (db/Convert): the designer's words and
// what the step does are this one function.
void test_convert_pure() {
    using FT = FieldType;
    auto to = [](const Value& v, FT from, FT t) { return convert_value(v, from, t); };
    check("convert: to Integer -- ints, whole reals, text that reads as one (spaces round it); 12.5 / 4x / blob won't",
          to(Value(7), FT::Integer, FT::Integer) == Value(7) && to(Value(12.0), FT::Number, FT::Integer) == Value(12) &&
          to(Value(" 42 "), FT::Text, FT::Integer) == Value(42) && to(Value("+3"), FT::Text, FT::Integer) == Value(3) &&
          !to(Value(12.5), FT::Number, FT::Integer) && !to(Value("4x"), FT::Text, FT::Integer) &&
          !to(Value(""), FT::Text, FT::Integer) && !to(Value(Blob{1, 2}), FT::Container, FT::Integer), "");
    check("convert: to Text -- numbers as written, Yes-No as Yes / No, text as is, NULL stays NULL",
          to(Value(12), FT::Integer, FT::Text) == Value("12") && to(Value(12.5), FT::Number, FT::Text) == Value("12.5") &&
          to(Value(3.0), FT::Number, FT::Text) == Value("3") && to(Value(1), FT::Boolean, FT::Text) == Value("Yes") &&
          to(Value(0), FT::Boolean, FT::Text) == Value("No") && to(Value(1), FT::Integer, FT::Text) == Value("1") &&
          to(Value(), FT::Integer, FT::Text) == Value() && !to(Value(Blob{1}), FT::Container, FT::Text), "");
    check("convert: to Number / Yes-No -- numbers and their text; yes/no/true/false/1/0, nothing else",
          to(Value(2), FT::Integer, FT::Number) == Value(2.0) && to(Value("0.25"), FT::Text, FT::Number) == Value(0.25) &&
          !to(Value("nan"), FT::Text, FT::Number) && !to(Value("1e999"), FT::Text, FT::Number) &&
          to(Value("Yes"), FT::Text, FT::Boolean) == Value(1) && to(Value(" FALSE "), FT::Text, FT::Boolean) == Value(0) &&
          to(Value(1.0), FT::Number, FT::Boolean) == Value(1) && !to(Value(2), FT::Integer, FT::Boolean) &&
          !to(Value("maybe"), FT::Text, FT::Boolean), "");
    check("convert: dates, times, timestamps by shape; a date gains 00:00:00; never cut",
          to(Value("2026-11-07"), FT::Text, FT::Date) == Value("2026-11-07") && !to(Value("2026-13-01"), FT::Text, FT::Date) &&
          !to(Value("11/07/2026"), FT::Text, FT::Date) && to(Value("09:30"), FT::Text, FT::Time) == Value("09:30") &&
          to(Value("23:59:59.5"), FT::Text, FT::Time) && !to(Value("24:00"), FT::Text, FT::Time) &&
          to(Value("2026-11-07"), FT::Date, FT::Timestamp) == Value("2026-11-07 00:00:00") &&
          to(Value("2026-11-07T09:30:00"), FT::Text, FT::Timestamp) &&
          !to(Value("2026-11-07 09:30"), FT::Timestamp, FT::Date) && !to(Value(20261107), FT::Integer, FT::Date), "");
    check("convert: native shapes are put back untouched",
          native_to(Value(1), FT::Boolean) && native_to(Value("x"), FT::Date) && !native_to(Value("1"), FT::Integer) &&
          native_to(Value(1), FT::Number) && !native_to(Value(1.5), FT::Integer), "");
    auto ds = [](Dialect d, FT t, const std::string& typed) {
        auto r = default_sql_for(d, t, "f", typed);
        return r ? r->value_or("(none)") : "!" + r.error();
    };
    check("defaults: typed -> SQL (quoted text, bare numbers, Yes -> 1 / TRUE on PostgreSQL, '' -> none, refusals)",
          ds(Dialect::SQLite, FT::Text, "it's") == "'it''s'" && ds(Dialect::SQLite, FT::Integer, " 5 ") == "5" &&
          ds(Dialect::SQLite, FT::Number, "2.50") == "2.5" && ds(Dialect::SQLite, FT::Boolean, "yes") == "1" &&
          ds(Dialect::PostgreSQL, FT::Boolean, "no") == "FALSE" && ds(Dialect::SQLite, FT::Text, "") == "(none)" &&
          ds(Dialect::SQLite, FT::Date, "2026-01-02") == "'2026-01-02'" &&
          ds(Dialect::SQLite, FT::Integer, "x").starts_with("!the default for") &&
          ds(Dialect::SQLite, FT::Container, "x").starts_with("!"), ds(Dialect::SQLite, FT::Number, "2.50"));
    check("defaults: SQL -> what the designer shows (quotes, '' and PostgreSQL casts undone)",
          default_text(std::string("'it''s'")) == "it's" && default_text(std::string("'x'::text")) == "x" &&
          default_text(std::string("'a b'::character varying")) == "a b" && default_text(std::string("0")) == "0" &&
          default_text(std::string("CURRENT_TIMESTAMP")) == "CURRENT_TIMESTAMP" && default_text(std::nullopt).empty() &&
          default_text(std::string("(7)::bigint")) == "7", default_text(std::string("'x'::text")));
}

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
          p1 && p1->ids.size() == 3 && p1->ids[1] == Store::row_id("acorn_browse", {"id"}, {Value(2)}), p1 && !p1->ids.empty() ? p1->ids[0] : "");
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

// ── s007: the designer, live -- every engine ──────────────────────────────────
void test_design(Journal& j, Gate& g, Engine* e, const std::string& P) {
    const Dialect d = e->dialect();
    const std::string T = "acorn_design";
    TableDef t;
    t.name = T;
    t.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"name", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"age", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false},
                 {"city", decl_for(d, FieldType::Text), true, std::nullopt, 0, false}};
    t.indexes = {{"acorn_design_age", {"age"}, false, false}};
    if (auto mk = verbs::new_table(j, t); !mk) { check(P + "design: fixture", false, mk.error().message); return; }
    const std::pair<const char*, int> people[] = {{"Ada", 36}, {"Grace", 85}, {"Linus", 0}};
    for (auto [n, a] : people)
        (void)verbs::new_record(j, T, {"name", "age"}, {Value(n), a ? Value(a) : Value()});
    // Postgres and MariaDB list the key's own index; Acorn's indexes are the rest.
    auto info = [&] {
        auto s = g.introspect();
        if (!s || !s->table(T)) return std::optional<Table>();
        Table x = *s->table(T);
        std::erase_if(x.indexes, [](const Index& ix) { return ix.primary; });
        return std::optional<Table>(x);
    };
    auto ages = [&](const std::string& col) {
        std::vector<Value> v;
        if (auto r = g.query("SELECT " + g.quote_ident(col) + " FROM " + g.quote_ident(T) + " ORDER BY " + g.quote_ident("id")))
            for (auto& row : r->rows) v.push_back(row[0]);
        return v;
    };
    const std::vector<Value> want_ages = {Value(36), Value(85), Value()};

    // The preview is what runs.
    TableDef w = TableDef::from(*info());
    w.columns.push_back({"email", decl_for(d, FieldType::Text), true, std::nullopt, 0, false});
    auto pv = verbs::preview(j, w);
    auto nf = verbs::new_field(j, T, w.columns.back());
    auto after = verbs::preview(j, TableDef::from(*info()));
    check(P + "design: New Field -- previewed as one statement, made, NULL everywhere; preview then empty",
          pv && pv->size() == 1 && nf && info() && info()->column("email") && after && after->empty() &&
          ages("email") == std::vector<Value>{Value(), Value(), Value()} &&
          j.undo_label() == "New Field \u201Cemail\u201D in \u201C" + T + "\u201D",
          nf ? j.undo_label() : nf.error().message);

    // Another tool fills the new field: undo of New Field is refused (it would
    // drop data), nothing touched.
    (void)g.inner().query("UPDATE " + g.quote_ident(T) + " SET " + g.quote_ident("email") + " = 'x@y'");
    auto un = j.undo();
    check(P + "design: undo New Field refused once the field holds values -- they stay",
          !un && info() && info()->column("email") && ages("email")[0] == Value("x@y"), un ? "undone!" : un.error().message);
    (void)g.inner().query("UPDATE " + g.quote_ident(T) + " SET " + g.quote_ident("email") + " = NULL");
    auto un2 = j.undo();
    check(P + "design: emptied again, undo removes the field", un2 && info() && !info()->column("email"),
          un2 ? "" : un2.error().message);

    // Rename: values and the index go with it.
    auto rn = verbs::rename_field(j, T, "age", "years");
    auto ri = info();
    bool ix_follows = false;
    if (ri) for (auto& ix : ri->indexes) if (ix.name == "acorn_design_age") ix_follows = ix.columns == std::vector<std::string>{"years"};
    check(P + "design: Rename Field -- values kept, the index follows",
          rn && ri && ri->column("years") && !ri->column("age") && ages("years") == want_ages && ix_follows,
          rn ? "" : rn.error().message);
    auto dup = verbs::rename_field(j, T, "years", "NAME");
    check(P + "design: rename onto another field's name (any case) refused", !dup && dup.error().code == Error::Code::Exists,
          dup ? "renamed!" : dup.error().message);
    (void)j.undo();

    // Delete Field: its index goes, its values are KEPT; undo brings them back.
    auto df = verbs::delete_field(j, T, "age");
    auto di = info();
    check(P + "design: Delete Field -- gone, its index gone too, one step",
          df && di && !di->column("age") && di->indexes.size() == 0 &&
          j.undo_label() == "Delete Field \u201Cage\u201D from \u201C" + T + "\u201D",
          df ? "" : df.error().message);
    auto seq_now = [&] {   // SQLite's id count (s007e: the placement rebuild keeps it)
        auto r = g.query("SELECT seq FROM sqlite_sequence WHERE name = '" + T + "'");
        return r && !r->rows.empty() ? r->rows[0][0] : Value();
    };
    const Value seq0 = d == Dialect::SQLite ? seq_now() : Value();
    auto order_of = [&](const std::optional<Table>& x) {
        std::vector<std::string> n;
        if (x) for (auto& c : x->columns) n.push_back(c.name);
        return n;
    };
    const std::vector<std::string> in_place = {"id", "name", "age", "city"}, at_end = {"id", "name", "city", "age"};
    auto ud = j.undo();
    auto ui = info();
    // s007e: SQLite (made again) and MariaDB (AFTER) put it back in its place;
    // PostgreSQL can only add it at the end.
    const bool placed = d == Dialect::PostgreSQL ? order_of(ui) == at_end : order_of(ui) == in_place;
    check(P + "design: undo Delete Field -- the field back " +
              (d == Dialect::PostgreSQL ? std::string("(last: the engine's limit)") : std::string("IN ITS PLACE")) +
              ", every value back, the index back",
          ud && ui && placed && ages("age") == want_ages &&
          ui->indexes.size() == 1 && ui->indexes[0].columns == std::vector<std::string>{"age"} &&
          (d != Dialect::SQLite || seq_now() == seq0),
          ud ? "" : ud.error().message);
    auto rd = j.redo();
    check(P + "design: redo Delete Field -- gone again", rd && info() && !info()->column("age"), rd ? "" : rd.error().message);
    (void)j.undo();
    if (d == Dialect::SQLite) {
        // s007e: a trigger the rebuild would lose -> no rebuild; the field goes
        // on the end as before, values and trigger intact.
        (void)verbs::delete_field(j, T, "age");
        (void)g.inner().query("CREATE TRIGGER acorn_design_tr AFTER INSERT ON " + g.quote_ident(T) + " BEGIN SELECT 1; END");
        auto pv2 = verbs::preview(j, [&] { TableDef x = TableDef::from(*info()); x.columns.insert(x.columns.begin() + 2,
                                        {"age", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}); return x; }());
        auto ut = j.undo();
        auto tr = g.query("SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' AND name = 'acorn_design_tr'");
        check(P + "design (s007e): SQLite -- a trigger blocks the rebuild: undo puts the field at the end, "
                  "values and trigger kept; the preview says ADD COLUMN",
              ut && order_of(info()) == at_end && ages("age") == want_ages && tr && !tr->rows.empty() &&
              tr->rows[0][0] == Value(1) && pv2 && pv2->size() == 1 && (*pv2)[0].find("ADD COLUMN") != std::string::npos,
              ut ? (pv2 && !pv2->empty() ? (*pv2)[0] : "") : ut.error().message);
        (void)g.inner().query("DROP TRIGGER acorn_design_tr");
        // Put the fixture back in its usual order for what follows.
        j.clear();
        (void)g.inner().query("DROP TABLE " + g.quote_ident(T));
        if (auto mk = verbs::new_table(j, t); !mk) { check(P + "design: fixture again", false, mk.error().message); return; }
        for (auto [n, a] : people)
            (void)verbs::new_record(j, T, {"name", "age"}, {Value(n), a ? Value(a) : Value()});
    }
    // A record changed while the field was gone: undo is refused, nothing touched.
    (void)verbs::delete_field(j, T, "age");
    (void)g.inner().query("DELETE FROM " + g.quote_ident(T) + " WHERE " + g.quote_ident("name") + " = 'Grace'");
    auto gone = j.undo();
    check(P + "design: undo refused when a record that held a value is gone -- no field half-restored",
          !gone && info() && !info()->column("age"), gone ? "undone!" : gone.error().message);
    j.clear();
    (void)g.inner().query("DROP TABLE " + g.quote_ident(T));
    if (auto mk = verbs::new_table(j, t); !mk) { check(P + "design: fixture again", false, mk.error().message); return; }
    for (auto [n, a] : people)
        (void)verbs::new_record(j, T, {"name", "age"}, {Value(n), a ? Value(a) : Value()});

    // The data rule, in the store: a step that drops a field holding values
    // without keeping them is refused.
    auto raw = j.run("Raw drop", {Store::table_id(T)}, [&](Store& s) {
        TableDef x = std::get<TableDef>(*s.get(Store::table_id(T)));
        std::erase_if(x.columns, [](auto& c) { return c.name == "age"; });
        x.indexes.clear();
        s.put(Store::table_id(T), x);
    });
    check(P + "design: the store never drops a field with data unless the step keeps it",
          !raw && raw.error().code == Error::Code::Constraint && info() && info()->column("age") && ages("age") == want_ages,
          raw ? "dropped!" : raw.error().message);
    auto dk = verbs::delete_field(j, T, "id");
    check(P + "design: the key field is never deleted", !dk && info() && info()->column("id"), dk ? "deleted!" : dk.error().message);

    // Indexes.
    const auto n1 = j.size();
    auto bad = verbs::new_index(j, T, {"acorn_design_city", {"city"}, true, false});   // NULLs: fine
    (void)j.undo();
    (void)g.inner().query("UPDATE " + g.quote_ident(T) + " SET " + g.quote_ident("city") + " = 'Paris'");
    auto clash = verbs::new_index(j, T, {"acorn_design_city", {"city"}, true, false});
    check(P + "design: a UNIQUE index over duplicates -- the engine refuses, rolled back, no step",
          bad && !clash && j.size() == n1 + 1 && info() && info()->indexes.size() == 1, clash ? "made!" : clash.error().message);
    auto taken = verbs::new_index(j, T, {"acorn_design_age", {"name"}, false, false});
    check(P + "design: an index name already used is refused", !taken && taken.error().code == Error::Code::Exists, "");
    auto mi = verbs::new_index(j, T, {"acorn_design_nm", {"name", "city"}, false, false});
    auto mii = info();
    bool two_cols = false;
    if (mii) for (auto& ix : mii->indexes) if (ix.name == "acorn_design_nm") two_cols = ix.columns == std::vector<std::string>{"name", "city"};
    auto dx = verbs::delete_index(j, T, "acorn_design_age");
    check(P + "design: New Index (two fields, in order), Delete Index",
          mi && two_cols && dx && info() && info()->indexes.size() == 1, mi ? (dx ? "" : dx.error().message) : mi.error().message);
    auto ux = j.undo();
    check(P + "design: undo Delete Index makes it again", ux && info() && info()->indexes.size() == 2, ux ? "" : ux.error().message);
    // s007b: the designer's whole picture as one step, with its words first.
    TableDef dw = TableDef::from(*info());
    std::erase_if(dw.columns, [](auto& c) { return c.name == "age"; });
    std::erase_if(dw.indexes, [](auto& x) { return std::find(x.columns.begin(), x.columns.end(), "age") != x.columns.end(); });
    dw.columns.push_back({"phone", decl_for(d, FieldType::Text), true, std::nullopt, 0, false});
    auto loss = verbs::design_losses(j, dw);
    auto dt = verbs::design_table(j, dw);
    auto dti = info();
    check(P + "design: Design Table -- the loss said in words first, then delete + add as ONE step",
          loss && loss->size() == 1 && (*loss)[0] == "\u201Cage\u201D holds 2 values -- they go with the field" &&
          dt && dti && !dti->column("age") && dti->column("phone") && j.undo_label() == "Change Table \u201C" + T + "\u201D",
          loss && !loss->empty() ? (*loss)[0] : (dt ? j.undo_label() : dt.error().message));
    auto dtu = j.undo();
    check(P + "design: one Ctrl+Z undoes all of it, values back",
          dtu && info() && info()->column("age") && !info()->column("phone") && ages("age") == want_ages,
          dtu ? "" : dtu.error().message);
    // Delete one field and add one of the same kind: reads as a rename -- refused.
    TableDef amb = TableDef::from(*info());
    std::erase_if(amb.columns, [](auto& c) { return c.name == "city"; });
    std::erase_if(amb.indexes, [](auto& x) { return std::find(x.columns.begin(), x.columns.end(), "city") != x.columns.end(); });
    amb.columns.push_back({"town", "VARCHAR(40)", true, std::nullopt, 0, false});   // Text, spelled otherwise
    const auto na = j.size();
    auto am = verbs::design_table(j, amb);
    check(P + "design: delete + add of the same kind in one step is refused (it would read as a rename)",
          !am && am.error().message.find("rename") != std::string::npos && j.size() == na && info()->column("city"),
          am ? "applied!" : am.error().message);
    auto none = verbs::design_table(j, TableDef::from(*info()));
    check(P + "design: nothing changed -- no step", none && j.can_redo(), none ? "" : none.error().message);

    // ── s007c: change a field -- type, required, default ──────────────────
    const bool maria = d == Dialect::MariaDB;
    auto col = [&](const std::string& n) {
        auto i = info();
        const Column* c = i ? i->column(n) : nullptr;
        return c ? std::optional<Column>(*c) : std::nullopt;
    };
    auto def_of = [&](const std::string& n, FieldType ft, bool nullable, std::optional<std::string> dflt = std::nullopt) {
        TableDef w = TableDef::from(*info());
        for (auto& c : w.columns)
            if (c.name == n) { c.decl_type = ft == map_type(d, c.decl_type) ? c.decl_type : decl_for(d, ft);
                               c.nullable = nullable; c.default_sql = dflt; }
        return w;
    };
    const auto ixs = info()->indexes.size();
    const std::vector<Value> paris(3, Value("Paris"));
    // Integer -> Text: every value fits; undo puts the NUMBERS back, not "36".
    TableDef at = def_of("age", FieldType::Text, true);
    auto atw = verbs::design_losses(j, at);
    auto atd = verbs::design_table(j, at);
    check(P + "change: Integer -> Text -- no words, one step, the values as text, indexes kept",
          atw && atw->empty() && atd && col("age") && col("age")->type == FieldType::Text &&
          ages("age") == std::vector<Value>{Value("36"), Value("85"), Value()} && info()->indexes.size() == ixs &&
          j.undo_label() == "Change Field “age” in “" + T + "”",
          !atw ? atw.error().message : !atd ? atd.error().message : (ages("age").empty() ? "" : ages("age")[0].to_display()));
    auto atu = j.undo();
    check(P + "change: undo -- the field a whole number again, 36 / 85 exactly",
          atu && col("age") && col("age")->type == FieldType::Integer && ages("age") == want_ages,
          atu ? (ages("age").empty() ? "" : ages("age")[0].to_display()) : atu.error().message);
    auto atr = j.redo();
    check(P + "change: redo -- text again", atr && col("age") && col("age")->type == FieldType::Text &&
          ages("age") == std::vector<Value>{Value("36"), Value("85"), Value()}, atr ? "" : atr.error().message);
    (void)j.undo();

    // Text -> Integer: "Paris" won't fit -- said in words; becomes empty; undo brings it back.
    TableDef ci = def_of("city", FieldType::Integer, true);
    auto ciw = verbs::design_losses(j, ci);
    auto cid = verbs::design_table(j, ci);
    if (maria) {
        check(P + "change: MariaDB refuses emptying values that won't fit (it can't roll the ALTER back), untouched",
              !ciw && !cid && ages("city") == paris && col("city")->type == FieldType::Text,
              cid ? "changed!" : cid.error().message);
    } else {
        check(P + "change: Text -> Integer -- the misfits said in words, then emptied",
              ciw && ciw->size() == 1 &&
              (*ciw)[0] == "3 of “city”'s 3 values won't fit a whole number -- they become empty" &&
              cid && col("city") && col("city")->type == FieldType::Integer &&
              ages("city") == std::vector<Value>(3, Value()) && info()->indexes.size() == ixs,
              !ciw ? ciw.error().message : ciw->empty() ? "no words" : (*ciw)[0]);
        auto ciu = j.undo();
        check(P + "change: undo -- “Paris” back in every record, Text again, indexes kept",
              ciu && ages("city") == paris && col("city")->type == FieldType::Text && info()->indexes.size() == ixs,
              ciu ? "" : ciu.error().message);
        // The data rule in the store: a type change on a field holding values
        // that the step doesn't keep is refused.
        auto raw2 = j.run("Raw change", {Store::table_id(T)}, [&](Store& s) { s.put(Store::table_id(T), ci); });
        check(P + "change: the store never converts values the step doesn't keep",
              !raw2 && raw2.error().code == Error::Code::Constraint && ages("city") == paris,
              raw2 ? "changed!" : raw2.error().message);
    }

    // Required: an empty record and no default -> refused in words; with one,
    // the empties take it (said first), and undo empties them again.
    TableDef rq = def_of("age", FieldType::Integer, false);
    auto rqw = verbs::design_losses(j, rq);
    auto rqd = verbs::design_table(j, rq);
    check(P + "change: required with an empty record and no default -- refused in words, untouched",
          !rqw && rqw.error().message.find("needs a default") != std::string::npos && !rqd && col("age")->nullable,
          rqw ? "allowed!" : rqw.error().message);
    TableDef rq2 = def_of("age", FieldType::Integer, false, std::string("18"));
    auto rq2w = verbs::design_losses(j, rq2);
    auto rq2d = verbs::design_table(j, rq2);
    if (maria) {
        check(P + "change: MariaDB refuses filling empty records too", !rq2w && !rq2d && col("age")->nullable,
              rq2d ? "changed!" : rq2d.error().message);
    } else {
        check(P + "change: required + default -- the fill said in words, then made: 18 where it was empty",
              rq2w && rq2w->size() == 1 && (*rq2w)[0] == "1 record has no “age” -- it takes the default “18”" &&
              rq2d && !col("age")->nullable && default_text(col("age")->default_sql) == "18" &&
              ages("age") == std::vector<Value>{Value(36), Value(85), Value(18)},
              !rq2w ? rq2w.error().message : rq2w->empty() ? "no words" : (*rq2w)[0]);
        auto rqu = j.undo();
        check(P + "change: undo -- optional again, no default, the record empty again",
              rqu && col("age")->nullable && !col("age")->default_sql && ages("age") == want_ages,
              rqu ? "" : rqu.error().message);
    }
    // A default alone: nothing to say, nothing changes in the records.
    TableDef dft = def_of("name", FieldType::Text, true, std::string("'nobody'"));
    auto dfw = verbs::design_losses(j, dft);
    auto dfd = verbs::design_table(j, dft);
    check(P + "change: a default alone -- no words, one step, read back as the same words",
          dfw && dfw->empty() && dfd && default_text(col("name")->default_sql) == "nobody",
          dfd ? (col("name")->default_sql ? *col("name")->default_sql : "none") : dfd.error().message);
    (void)j.undo();
    check(P + "change: undo -- no default again", col("name") && !col("name")->default_sql, "");

    if (d == Dialect::SQLite) {
        // The rebuild keeps the count: ids are never reused, even past a
        // deleted last record.
        auto seq = [&] {
            auto r = g.query("SELECT seq FROM sqlite_sequence WHERE name = '" + T + "'");
            return r && !r->rows.empty() ? r->rows[0][0] : Value();
        };
        auto made = verbs::new_record(j, T, {"name"}, {Value("Temp")});
        if (made) (void)verbs::delete_record(j, *made);
        const Value s0 = seq();
        auto rb = verbs::design_table(j, def_of("city", FieldType::Text, true, std::string("'here'")));
        auto next = verbs::new_record(j, T, {"name"}, {Value("After")});
        std::string tb;
        std::vector<Value> key;
        if (next) Store::parse_row_id(*next, tb, key);
        check(P + "change: SQLite rebuild keeps the id count (a deleted last id is not handed out again)",
              rb && seq().as_int() && s0.as_int() && !key.empty() && key[0].as_int() &&
              *key[0].as_int() == *s0.as_int() + 1, rb ? s0.to_display() + " -> " + seq().to_display() : rb.error().message);
        (void)j.undo();
        (void)j.undo();
        (void)g.inner().query("CREATE TRIGGER acorn_design_tr AFTER INSERT ON " + g.quote_ident(T) + " BEGIN SELECT 1; END");
        auto tw = verbs::design_losses(j, def_of("city", FieldType::Text, true, std::string("'here'")));
        check(P + "change: SQLite -- a trigger on the table stops the rebuild, in words",
              !tw && tw.error().message.find("trigger") != std::string::npos, tw ? "allowed!" : tw.error().message);
        (void)g.inner().query("DROP TRIGGER acorn_design_tr");
    }
    check(P + "design: nothing wrote outside the door", j.strays() == 1, std::to_string(j.strays()));
    (void)g.inner().query("DROP TABLE " + g.quote_ident(T));
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
    // Fields by NAME (s007: a field undo puts back comes back last -- the same
    // table), rows in the order of those fields.
    for (const auto& t : s.tables) {
        std::vector<std::string> cols;
        for (const auto& col : t.columns) cols.push_back(col.name);
        std::sort(cols.begin(), cols.end());
        std::string list;
        for (std::size_t i = 0; i < cols.size(); ++i) list += (i ? ", " : "") + c.quote_ident(cols[i]);
        std::string order;
        for (std::size_t i = 1; i <= cols.size(); ++i) order += (i > 1 ? ", " : "") + std::to_string(i);
        auto r = c.query("SELECT " + list + " FROM " + c.quote_ident(t.name) + " ORDER BY " + order);
        s.rows.push_back(r ? r->rows : std::vector<Row>{});
    }
    return s;
}
bool has_table(Connection& c, const std::string& n) {
    auto s = c.introspect();
    return s && s->table(n);
}

// ── s007d: the key ───────────────────────────────────────────────────────────
void test_key_pure() {
    TableDef live;
    live.name = "people";
    live.key_name = "people_pkey";
    live.columns = {{"id", "INTEGER", true, std::nullopt, 1, true},
                    {"email", "TEXT", true, std::nullopt, 0, false},
                    {"age", "INTEGER", true, std::nullopt, 0, false}};
    auto by_email = verbs::keyed(live, {"email"}, false);
    check("key: verbs::keyed() -- email joins (required), id leaves (required, stops counting)",
          by_email && by_email->column("email")->pk_position == 1 && !by_email->column("email")->nullable &&
          by_email->column("id")->pk_position == 0 && !by_email->column("id")->auto_increment &&
          !by_email->column("id")->nullable && !by_email->column("age")->nullable == false, "");
    check("key: verbs::keyed() refuses a field the table doesn't have, or one named twice",
          !verbs::keyed(live, {"nope"}, false) && !verbs::keyed(live, {"email", "email"}, false), "");
    check("key: the label names the new key", verbs::design_label(live, *by_email) == "Change Key of “people” to “email”",
          verbs::design_label(live, *by_email));

    auto sq = plan_alter(Dialect::SQLite, '"', live, *by_email);
    std::string all;
    if (sq) for (auto& s : *sq) all += s + "\n";
    check("key: SQLite makes the table again -- the new key written, the id count carried, the old row cleared",
          sq && all.find("PRIMARY KEY (\"email\")") != std::string::npos &&
          all.find("\"email\" TEXT NOT NULL") != std::string::npos &&
          all.find("INSERT INTO sqlite_sequence") != std::string::npos &&
          all.find("DELETE FROM sqlite_sequence WHERE name = 'people'") != std::string::npos &&
          all.find("AUTOINCREMENT") == std::string::npos, sq ? all : sq.error());

    TableDef pl = live;
    pl.columns[0].decl_type = "bigint";
    pl.columns[0].nullable = false;
    pl.columns[1].decl_type = "text";
    auto pk = verbs::keyed(pl, {"email"}, false);
    auto pg = plan_alter(Dialect::PostgreSQL, '"', pl, *pk);
    check("key: PostgreSQL -- DROP IDENTITY, DROP CONSTRAINT by name, SET NOT NULL, ADD PRIMARY KEY, in that order",
          pg && pg->size() == 4 && (*pg)[0] == "ALTER TABLE \"people\" ALTER COLUMN \"id\" DROP IDENTITY" &&
          (*pg)[1] == "ALTER TABLE \"people\" DROP CONSTRAINT \"people_pkey\"" &&
          (*pg)[2] == "ALTER TABLE \"people\" ALTER COLUMN \"email\" SET NOT NULL" &&
          (*pg)[3] == "ALTER TABLE \"people\" ADD PRIMARY KEY (\"email\")",
          pg ? (pg->empty() ? "" : (*pg)[0] + " / " + std::to_string(pg->size())) : pg.error());
    auto back = plan_alter(Dialect::PostgreSQL, '"', *pk, pl);
    std::string ball;
    if (back) for (auto& s : *back) ball += s + "\n";
    check("key: PostgreSQL -- a counting key back: identity added, the count starts past the highest id",
          back && ball.find("ADD GENERATED BY DEFAULT AS IDENTITY") != std::string::npos &&
          ball.find("setval(pg_get_serial_sequence('\"people\"', 'id'), COALESCE(MAX(\"id\"), 0) + 1, false)") != std::string::npos &&
          ball.find("ALTER COLUMN \"email\" DROP NOT NULL") != std::string::npos,
          back ? ball : back.error());

    TableDef ml = pl;
    ml.columns[0].decl_type = "bigint(20)";
    ml.columns[1].decl_type = "varchar(255)";
    auto mk = verbs::keyed(ml, {"email", "id"}, false);
    auto my = plan_alter(Dialect::MariaDB, '`', ml, *mk);
    check("key: MariaDB -- ONE ALTER: drop the key, the fields, the new key",
          my && my->size() == 1 && (*my)[0].find("DROP PRIMARY KEY") != std::string::npos &&
          (*my)[0].find("MODIFY COLUMN `id` bigint(20) NOT NULL") != std::string::npos &&
          (*my)[0].find("AUTO_INCREMENT") == std::string::npos &&
          (*my)[0].find("ADD PRIMARY KEY (`email`, `id`)") != std::string::npos,
          my ? (my->empty() ? "" : (*my)[0]) : my.error());

    // Refusals, in words.
    auto mixed = *by_email;
    mixed.columns[2].name = "years";
    check("key: a key change mixed with other changes is refused -- a step of its own",
          !plan_alter(Dialect::SQLite, '"', live, mixed), "");
    auto none = verbs::keyed(live, {}, false);
    auto pn = plan_alter(Dialect::PostgreSQL, '"', pl, *verbs::keyed(pl, {}, false));
    check("key: no key at all is refused", none && !pn && pn.error().find("needs a key") != std::string::npos,
          pn ? "planned!" : pn.error());
    auto txt = verbs::keyed(pl, {"email"}, true);
    auto pt = plan_alter(Dialect::PostgreSQL, '"', pl, *txt);
    check("key: a text key can't count up", !pt && pt.error().find("whole number") != std::string::npos,
          pt ? "planned!" : pt.error());
    auto two = verbs::keyed(pl, {"id", "age"}, true);
    check("key: a key of two fields doesn't count up", two && !two->column("id")->auto_increment, "");
    auto stop = verbs::keyed(live, {"id"}, false);
    auto ps = plan_alter(Dialect::SQLite, '"', live, *stop);
    check("key: SQLite -- a lone INTEGER key always counts (the engine's rule), said in words",
          !ps && ps.error().find("always counts up") != std::string::npos, ps ? "planned!" : ps.error());
    auto ok_pg = plan_alter(Dialect::PostgreSQL, '"', pl, *verbs::keyed(pl, {"id"}, false));
    check("key: PostgreSQL -- the key stops counting: DROP IDENTITY alone",
          ok_pg && ok_pg->size() == 1 && (*ok_pg)[0].find("DROP IDENTITY") != std::string::npos,
          ok_pg ? std::to_string(ok_pg->size()) : ok_pg.error());
    check("key: the counting labels", verbs::design_label(pl, *verbs::keyed(pl, {"id"}, false)) == "Stop “id” Counting Up" &&
          verbs::design_label(*verbs::keyed(pl, {"id"}, false), pl) == "Make “id” Count Up", "");
    auto renamed = live;
    renamed.columns[0].name = "pid";
    auto pr = plan_alter(Dialect::PostgreSQL, '"', pl, [&] { auto x = pl; x.columns[0].name = "pid"; return x; }());
    check("key: renaming the key field is a rename, not a key change",
          pr && pr->size() == 1 && (*pr)[0].find("RENAME COLUMN") != std::string::npos, pr ? "" : pr.error());
}

// Every engine: the key moves, comes back, refuses what would break it.
void test_key(Journal& j, Gate& g, Engine* e, const std::string& P) {
    const Dialect d = e->dialect();
    const std::string T = "acorn_key";
    TableDef t;
    t.name = T;
    t.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"email", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"name", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"age", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}};
    if (auto mk = verbs::new_table(j, t); !mk) { check(P + "key: fixture", false, mk.error().message); return; }
    const char* rows[][3] = {{"ada@x", "Ada", "36"}, {"grace@x", "Grace", ""}, {"linus@x", "Ada", "54"}, {"gone@x", "Gone", "1"}};
    for (auto& r : rows)
        (void)verbs::new_record(j, T, {"email", "name", "age"},
                                {Value(r[0]), Value(r[1]), *r[2] ? Value(static_cast<std::int64_t>(std::stoll(r[2]))) : Value()});
    // the last one deleted: the highest id ever (4) is no longer in the table
    (void)verbs::delete_record(j, Store::row_id(T, {"id"}, {Value(4)}));
    auto pk = [&] {
        auto s = g.introspect();
        return s && s->table(T) ? s->table(T)->primary_key() : std::vector<std::string>{};
    };
    auto col = [&](const std::string& c) -> std::optional<Column> {
        auto s = g.introspect();
        if (!s || !s->table(T) || !s->table(T)->column(c)) return std::nullopt;
        return *s->table(T)->column(c);
    };

    // Refused in words, nothing touched.
    const auto steps = j.size();
    auto dup = verbs::change_key(j, T, {"name"}, false);
    check(P + "key: values shared by two records are refused, in words",
          !dup && dup.error().message.find("1 value of “name” is shared by more than one record") != std::string::npos &&
          j.size() == steps && pk() == std::vector<std::string>{"id"}, dup ? "made!" : dup.error().message);
    auto emp = verbs::change_key(j, T, {"email", "age"}, false);
    check(P + "key: a record without the field is refused, in words",
          !emp && emp.error().message.find("1 record has no “age”") != std::string::npos && j.size() == steps,
          emp ? "made!" : emp.error().message);
    TableDef live = TableDef::from(*g.introspect()->table(T));
    auto mixed = verbs::keyed(live, {"email"}, false);
    mixed->columns.push_back({"extra", decl_for(d, FieldType::Text), true, std::nullopt, 0, false});
    auto mx = verbs::design_table(j, *mixed);
    check(P + "key: a key change with another change is refused -- a step of its own",
          !mx && mx.error().message.find("step of its own") != std::string::npos && j.size() == steps,
          mx ? "made!" : mx.error().message);

    // The words before Apply.
    auto words = verbs::design_losses(j, *verbs::keyed(live, {"email"}, false));
    std::string w;
    if (words) for (auto& x : *words) w += x + " | ";
    check(P + "key: the words say what changes -- found by email, email required, id stays",
          words && w.find("Records are found by “email” from now on") != std::string::npos &&
          w.find("“email” becomes required") != std::string::npos &&
          w.find("“id” stays, as a required field") != std::string::npos,
          words ? w : words.error().message);

    // Move it.
    auto mv = verbs::change_key(j, T, {"email"}, false);
    check(P + "key: Change Key moves it to email -- one step, labelled",
          mv && j.size() == steps + 1 && pk() == std::vector<std::string>{"email"} &&
          j.undo_label() == "Change Key of “acorn_key” to “email”", mv ? j.undo_label() : mv.error().message);
    auto idc = col("id");
    check(P + "key: id stays, required, no longer counting, values kept",
          idc && !idc->nullable && !idc->auto_increment && idc->pk_position == 0, "");
    // A record is found by its email now; a name under the old key is refused.
    auto ed = verbs::set_field(j, Store::row_id(T, {"email"}, {Value("grace@x")}), "age", Value(85));
    check(P + "key: a record is edited by its new key", ed.has_value(), ed ? "" : ed.error().message);
    auto old = verbs::set_field(j, Store::row_id(T, {"id"}, {Value(1)}), "age", Value(99));
    check(P + "key: a record named by the old key is refused in words, never matched by its values",
          !old && old.error().message.find("found by “email” now, not “id”") != std::string::npos,
          old ? "written!" : old.error().message);
    (void)j.undo();   // the edit

    // Back: undo puts the counting key back -- and on SQLite the id count too.
    auto un = j.undo();
    auto idb = col("id");
    check(P + "key: undo -- found by id again, counting", un && pk() == std::vector<std::string>{"id"} && idb && idb->auto_increment,
          un ? "" : un.error().message);
    auto nr = verbs::new_record(j, T, {"email"}, {Value("new@x")});
    if (d == Dialect::SQLite)
        check(P + "key: SQLite -- the id count came back with the key: the next id is 5, never the deleted 4",
              nr && *nr == Store::row_id(T, {"id"}, {Value(5)}), nr ? *nr : nr.error().message);
    else
        check(P + "key: the key counts again after undo", nr.has_value(), nr ? "" : nr.error().message);
    if (nr) (void)j.undo();

    // Counting off and on (the servers; SQLite refuses -- its rule).
    auto off = verbs::change_key(j, T, {"id"}, false);
    if (d == Dialect::SQLite) {
        check(P + "key: SQLite refuses to stop a lone INTEGER key counting, in words",
              !off && off.error().message.find("always counts up") != std::string::npos, off ? "made!" : off.error().message);
    } else {
        auto c0 = col("id");
        check(P + "key: Stop Counting Up -- id is still the key, no longer counting",
              off && c0 && !c0->auto_increment && pk() == std::vector<std::string>{"id"} &&
              j.undo_label() == "Stop “id” Counting Up", off ? j.undo_label() : off.error().message);
        auto none = verbs::new_record(j, T, {"email"}, {Value("nokey@x")});
        check(P + "key: ... so a record without its key is refused by the engine", !none, none ? *none : "");
        auto on = verbs::change_key(j, T, {"id"}, true);
        auto c1 = col("id");
        check(P + "key: Make Count Up -- counting again", on && c1 && c1->auto_increment &&
              j.undo_label() == "Make “id” Count Up", on ? j.undo_label() : on.error().message);
        auto nx = verbs::new_record(j, T, {"email"}, {Value("next@x")});
        check(P + "key: the count starts past the highest id there", nx && *nx != Store::row_id(T, {"id"}, {Value(1)}) &&
              *nx != Store::row_id(T, {"id"}, {Value(2)}) && *nx != Store::row_id(T, {"id"}, {Value(3)}),
              nx ? *nx : nx.error().message);
        if (nx) (void)j.undo();
        auto u1 = j.undo(), u2 = j.undo();
        check(P + "key: undo both -- counting as made", u1 && u2 && col("id") && col("id")->auto_increment,
              u1 ? (u2 ? "" : u2.error().message) : u1.error().message);
    }

    // A key of two fields, and back.
    auto two = verbs::change_key(j, T, {"name", "email"}, false);
    check(P + "key: a key of two fields (name, email -- name repeats, the pair doesn't)",
          two && pk() == std::vector<std::string>{"name", "email"}, two ? "" : two.error().message);
    auto u = j.undo();
    check(P + "key: undo -- id again", u && pk() == std::vector<std::string>{"id"}, u ? "" : u.error().message);
    check(P + "key: nothing strayed", j.strays() == 1, std::to_string(j.strays()));
}


// ── s008: links ──────────────────────────────────────────────────────────────
std::string upper_of(std::string v) {
    for (auto& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return v;
}
void test_links_pure() {
    TableDef orders;
    orders.name = "orders";
    orders.columns = {{"id", "INTEGER", false, std::nullopt, 1, true},
                      {"customer_id", "INTEGER", true, std::nullopt, 0, false},
                      {"note", "TEXT", true, std::nullopt, 0, false}};
    TableDef linked = orders;
    linked.links = {{"", {"customer_id"}, "customers", {"id"}, "RESTRICT", "NO ACTION"}};

    check("link: the SQL a link is written as",
          link_sql('"', linked.links[0]) ==
          "FOREIGN KEY (\"customer_id\") REFERENCES \"customers\" (\"id\") ON DELETE RESTRICT", link_sql('"', linked.links[0]));
    const std::string ct = create_table_sql(Dialect::PostgreSQL, '"', linked);
    check("link: CREATE TABLE writes the table's links in", ct.find(",\n  FOREIGN KEY (\"customer_id\")") != std::string::npos, ct);

    auto ps = plan_alter(Dialect::SQLite, '"', orders, linked);
    bool fk_in_create = ps && !ps->empty() && (*ps)[0].rfind("CREATE TABLE \"acorn_rebuild_orders\"", 0) == 0 &&
                        (*ps)[0].find("REFERENCES \"customers\" (\"id\") ON DELETE RESTRICT") != std::string::npos;
    bool swap = false;
    if (ps) for (auto& st : *ps) if (st == "ALTER TABLE \"acorn_rebuild_orders\" RENAME TO \"orders\"") swap = true;
    check("link: SQLite makes the child again with the link written in (copy, swap)", fk_in_create && swap,
          ps && !ps->empty() ? (*ps)[0] : (ps ? "" : ps.error()));
    auto pp = plan_alter(Dialect::PostgreSQL, '"', orders, linked);
    check("link: PostgreSQL -- ALTER TABLE .. ADD FOREIGN KEY, one statement",
          pp && pp->size() == 1 && (*pp)[0] == "ALTER TABLE \"orders\" ADD FOREIGN KEY (\"customer_id\") REFERENCES "
                                                "\"customers\" (\"id\") ON DELETE RESTRICT",
          pp && !pp->empty() ? (*pp)[0] : (pp ? "" : pp.error()));
    auto pm = plan_alter(Dialect::MariaDB, '`', orders, linked);
    check("link: MariaDB -- ONE ALTER TABLE with ADD FOREIGN KEY",
          pm && pm->size() == 1 && (*pm)[0] == "ALTER TABLE `orders`\n  ADD FOREIGN KEY (`customer_id`) REFERENCES "
                                                "`customers` (`id`) ON DELETE RESTRICT",
          pm && !pm->empty() ? (*pm)[0] : (pm ? "" : pm.error()));

    TableDef named = linked;
    named.links[0].name = "orders_customer_id_fkey";
    auto up = plan_alter(Dialect::PostgreSQL, '"', named, orders);
    check("link: unlink on PostgreSQL drops the constraint by its name",
          up && up->size() == 1 && (*up)[0] == "ALTER TABLE \"orders\" DROP CONSTRAINT \"orders_customer_id_fkey\"",
          up && !up->empty() ? (*up)[0] : (up ? "" : up.error()));
    TableDef mnamed = named;
    mnamed.links[0].name = "orders_ibfk_1";
    mnamed.indexes = {{"customer_id", {"customer_id"}, false, false}};   // MariaDB's own index for the link
    TableDef mgone = orders;
    auto um = plan_alter(Dialect::MariaDB, '`', mnamed, mgone);
    check("link: unlink on MariaDB -- the link dropped first, then the index it made, one ALTER",
          um && um->size() == 1 && (*um)[0] == "ALTER TABLE `orders`\n  DROP FOREIGN KEY `orders_ibfk_1`,\n  DROP INDEX `customer_id`",
          um && !um->empty() ? (*um)[0] : (um ? "" : um.error()));

    TableDef cascade = named;
    cascade.links[0].on_delete = "CASCADE";
    cascade.links[0].name.clear();
    auto cp = plan_alter(Dialect::PostgreSQL, '"', named, cascade);
    check("link: changing what a delete does is drop + add, in one step",
          cp && cp->size() == 2 && (*cp)[0].find("DROP CONSTRAINT") != std::string::npos &&
          (*cp)[1].find("ON DELETE CASCADE") != std::string::npos, cp ? "" : cp.error());

    TableDef noaction = named;
    noaction.links[0].on_delete = "NO ACTION";
    noaction.links[0].on_update = "RESTRICT";
    check("link: NO ACTION and RESTRICT read as the same rule; the name isn't compared",
          noaction == linked && !(cascade == linked), "");
    check("link: labels -- Link / Unlink / Change Link",
          verbs::design_label(orders, linked) == "Link “orders” to “customers”" &&
          verbs::design_label(named, orders) == "Unlink “orders” from “customers”" &&
          verbs::design_label(named, cascade) == "Change Link “orders” to “customers”", verbs::design_label(orders, linked));

    TableDef mixed = linked;
    mixed.columns.push_back({"extra", "TEXT", true, std::nullopt, 0, false});
    auto mx = plan_alter(Dialect::PostgreSQL, '"', orders, mixed);
    check("link: a link with another change is refused -- a step of its own",
          !mx && mx.error().find("step of its own") != std::string::npos, mx ? "planned!" : mx.error());
    TableDef gone = named;
    gone.columns.erase(gone.columns.begin() + 1);   // customer_id, its link kept
    auto gd = plan_alter(Dialect::PostgreSQL, '"', named, gone);
    check("link: deleting a field a link uses is refused in words",
          !gd && gd.error().find("the link to “customers” uses “customer_id”") != std::string::npos, gd ? "planned!" : gd.error());
    TableDef ren = named;
    ren.columns[1].name = "cust";
    ren.links[0].columns = {"cust"};
    auto rn = plan_alter(Dialect::PostgreSQL, '"', named, ren);
    check("link: renaming a linked field is a plain rename -- the link goes with it",
          rn && rn->size() == 1 && (*rn)[0] == "ALTER TABLE \"orders\" RENAME COLUMN \"customer_id\" TO \"cust\"",
          rn && !rn->empty() ? (*rn)[0] : (rn ? "" : rn.error()));

    Table cust;
    cust.name = "customers";
    cust.columns = {{"id", "INTEGER", FieldType::Integer, false, std::nullopt, 1, true}};
    auto lk = verbs::linked(orders, cust, {"customer_id"}, OnDelete::Cascade);
    check("link: linked() points the fields at the parent's key, with the rule",
          lk && lk->links.size() == 1 && lk->links[0].ref_columns == std::vector<std::string>{"id"} &&
          lk->links[0].deleting() == OnDelete::Cascade, lk ? "" : lk.error());
    auto two = verbs::linked(orders, cust, {"customer_id", "note"}, OnDelete::Restrict);
    Table keyless = cust;
    keyless.columns[0].pk_position = 0;
    auto nk = verbs::linked(orders, keyless, {"customer_id"}, OnDelete::Restrict);
    check("link: linked() refuses -- fields that don't match the key in number; a parent without a key",
          !two && two.error().find("needs 1 field") != std::string::npos && !nk &&
          nk.error().find("has no key") != std::string::npos, two ? "made!" : two.error());
}

void test_links(Journal& j, Gate& g, Engine* e, const std::string& P) {
    const Dialect d = e->dialect();
    const std::string C = "acorn_customers", O = "acorn_orders";
    TableDef c;
    c.name = C;
    c.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"name", decl_for(d, FieldType::Text), true, std::nullopt, 0, false}};
    TableDef o;
    o.name = O;
    o.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"customer_id", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false},
                 {"code", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"note", decl_for(d, FieldType::Text), false, std::string("'x'"), 0, false}};
    if (auto r = verbs::new_table(j, c); !r) { check(P + "links: fixture", false, r.error().message); return; }
    if (auto r = verbs::new_table(j, o); !r) { check(P + "links: fixture", false, r.error().message); return; }
    for (const char* n : {"Ada", "Grace"}) (void)verbs::new_record(j, C, {"name"}, {Value(n)});
    // orders 1, 2 -> customer 1; 3 -> 2; 4, 5 -> customer 7 and 9 (not there); 6 -> none
    const std::int64_t cust[] = {1, 1, 2, 7, 9, 0};
    for (auto k : cust) (void)verbs::new_record(j, O, {"customer_id"}, {k ? Value(k) : Value()});
    auto links_of = [&](const std::string& t) {
        auto s = g.introspect();
        return s && s->table(t) ? s->table(t)->foreign_keys : std::vector<ForeignKey>{};
    };
    auto count = [&](const std::string& t) {
        auto r = g.query("SELECT COUNT(*) FROM " + g.quote_ident(t));
        return r && !r->rows.empty() ? num(r->rows[0][0]).value_or(-1) : -1.0;
    };

    // Records that don't match: refused in words, named by unmatched(), nothing touched.
    const auto steps = j.size();
    auto orphan = verbs::new_link(j, O, {"customer_id"}, C);
    check(P + "links: records pointing at nothing are refused, in words -- nothing deleted, no step",
          !orphan && orphan.error().message.find("2 records in “acorn_orders” point at “acorn_customers” records that "
                                                 "aren't there -- fix them first") != std::string::npos &&
          j.size() == steps && links_of(O).empty() && count(O) == 6.0, orphan ? "linked!" : orphan.error().message);
    auto um = verbs::unmatched(j, O, {"customer_id"}, C);
    check(P + "links: unmatched() names exactly those records (4 and 5), so they can be marked",
          um && um->size() == 2 && (*um)[0] == Store::row_id(O, {"id"}, {Value(4)}) &&
          (*um)[1] == Store::row_id(O, {"id"}, {Value(5)}), um ? std::to_string(um->size()) : um.error().message);
    auto kind = verbs::new_link(j, O, {"code"}, C);
    check(P + "links: fields of another kind are refused, in words",
          !kind && kind.error().message.find("a link joins fields of the same kind") != std::string::npos && j.size() == steps,
          kind ? "linked!" : kind.error().message);
    auto req = verbs::new_link(j, O, {"note"}, C, OnDelete::SetEmpty);
    check(P + "links: Set Empty on a required field is refused, in words (or the kind first)",
          !req && j.size() == steps, req ? "linked!" : req.error().message);

    // Fix them (another tool) -- then the link is made.
    for (int k : {4, 5}) (void)verbs::set_field(j, Store::row_id(O, {"id"}, {Value(k)}), "customer_id", Value());
    auto made = verbs::new_link(j, O, {"customer_id"}, C);
    auto fks = links_of(O);
    check(P + "links: New Link -- one step, the link is in the database, pointing at the key",
          made && fks.size() == 1 && fks[0].ref_table == C && fks[0].columns == std::vector<std::string>{"customer_id"} &&
          j.undo_label() == "Link “acorn_orders” to “acorn_customers”" && count(O) == 6.0,
          made ? j.undo_label() : made.error().message);
    auto bad = g.inner().query("INSERT INTO " + g.quote_ident(O) + " (" + g.quote_ident("customer_id") + ") VALUES (42)");
    check(P + "links: the engine now refuses a record pointing at nothing", !bad, bad ? "inserted!" : bad.error().message);
    auto ed = verbs::set_field(j, Store::row_id(O, {"id"}, {Value(6)}), "customer_id", Value(std::int64_t{2}));
    check(P + "links: a record still edits through the link", ed.has_value(), ed ? "" : ed.error().message);
    (void)j.undo();

    // Restrict: deleting a customer with orders is refused in words.
    const auto s2 = j.size();
    auto rd = verbs::delete_record(j, Store::row_id(C, {"id"}, {Value(1)}));
    check(P + "links: Restrict -- deleting a record others point at is refused, in words",
          !rd && rd.error().message.find("2 records in “acorn_orders” point at this record") != std::string::npos &&
          j.size() == s2 && count(C) == 2.0, rd ? "deleted!" : rd.error().message);

    // The parent stays as it is while linked.
    auto pdrop = verbs::rename_field(j, C, "id", "cid");
    check(P + "links: renaming a field others point at is refused, in words",
          !pdrop && pdrop.error().message.find("remove the link before renaming it") != std::string::npos && j.size() == s2,
          pdrop ? "renamed!" : pdrop.error().message);
    auto pkey = verbs::change_key(j, C, {"name"}, false);
    check(P + "links: changing the key others point at is refused, in words",
          !pkey && pkey.error().message.find("remove the link before changing") != std::string::npos && j.size() == s2,
          pkey ? "changed!" : pkey.error().message);
    auto cdel = verbs::delete_field(j, O, "customer_id");
    check(P + "links: deleting a linked field is refused, in words",
          !cdel && cdel.error().message.find("remove the link first") != std::string::npos && j.size() == s2,
          cdel ? "deleted!" : cdel.error().message);
    auto crn = verbs::rename_field(j, O, "customer_id", "cust");
    auto fr = links_of(O);
    check(P + "links: renaming the child's field takes its link along",
          crn && fr.size() == 1 && fr[0].columns == std::vector<std::string>{"cust"}, crn ? "" : crn.error().message);
    auto crb = j.undo();
    check(P + "links: ... and undo renames it back, link and all",
          crb && links_of(O).size() == 1 && links_of(O)[0].columns == std::vector<std::string>{"customer_id"},
          crb ? "" : crb.error().message);

    // Cascade: the orders go IN the step; undo brings every one back.
    auto ch = verbs::change_link(j, O, {"customer_id"}, OnDelete::Cascade);
    auto lc = links_of(O);
    check(P + "links: Change Link to Cascade", ch && lc.size() == 1 && upper_of(lc[0].on_delete) == "CASCADE" &&
          j.undo_label() == "Change Link “acorn_orders” to “acorn_customers”", ch ? j.undo_label() : ch.error().message);
    const auto before = snapshot(g);
    auto cd = verbs::delete_record(j, Store::row_id(C, {"id"}, {Value(1)}));
    check(P + "links: Cascade -- the customer and its 2 orders go, one step",
          cd && count(C) == 1.0 && count(O) == 4.0, cd ? "" : cd.error().message);
    auto cu = j.undo();
    check(P + "links: ... Ctrl+Z puts the customer AND its orders back exactly", cu && snapshot(g) == before,
          cu ? "" : cu.error().message);
    auto cr = j.redo();
    check(P + "links: ... redo deletes them again", cr && count(O) == 4.0, cr ? "" : cr.error().message);
    (void)j.undo();

    // Set Empty: the orders stay, their customer emptied, in the step.
    (void)verbs::change_link(j, O, {"customer_id"}, OnDelete::SetEmpty);
    const auto before_se = snapshot(g);
    auto sd = verbs::delete_record(j, Store::row_id(C, {"id"}, {Value(1)}));
    auto nul = g.query("SELECT COUNT(*) FROM " + g.quote_ident(O) + " WHERE " + g.quote_ident("customer_id") + " IS NULL");
    check(P + "links: Set Empty -- the customer goes, its orders stay with no customer",
          sd && count(C) == 1.0 && count(O) == 6.0 && nul && num(nul->rows[0][0]) == 5.0, sd ? "" : sd.error().message);
    auto su = j.undo();
    check(P + "links: ... Ctrl+Z puts the customer back and the orders point at it again",
          su && snapshot(g) == before_se, su ? "" : su.error().message);

    // Unlink, undo, redo.
    auto ul = verbs::delete_link(j, O, {"customer_id"});
    check(P + "links: Unlink -- the link is gone, the records untouched",
          ul && links_of(O).empty() && count(O) == 6.0 && j.undo_label() == "Unlink “acorn_orders” from “acorn_customers”",
          ul ? j.undo_label() : ul.error().message);
    auto uu = j.undo();
    check(P + "links: ... undo puts the link back", uu && links_of(O).size() == 1, uu ? "" : uu.error().message);

    // A table others point at is not dropped (undo of its New Table).
    {
        TableDef p;
        p.name = "acorn_parent2";
        p.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true}};
        TableDef k;
        k.name = "acorn_child2";
        k.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                     {"p", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false}};
        (void)verbs::new_table(j, p);
        (void)verbs::new_table(j, k);
        (void)verbs::new_link(j, k.name, {"p"}, p.name);
        auto s = j.store().connection().introspect();
        // the drop is refused by the store, whatever asks for it
        auto dr = j.run("Drop", {Store::table_id(p.name)}, [&](Store& st) { st.put(Store::table_id(p.name), std::nullopt); });
        check(P + "links: a table others link to is never dropped -- refused in words",
              !dr && dr.error().message.find("“acorn_child2” links to “acorn_parent2” -- remove the link first") != std::string::npos &&
              has_table(g, p.name), dr ? "dropped!" : dr.error().message);
    }
    if (d == Dialect::SQLite) {
        auto self = verbs::new_link(j, C, {"id"}, C);
        check(P + "links: SQLite -- a table linking to itself is refused, in words",
              !self && self.error().message.find("link to itself") != std::string::npos,
              self ? "linked!" : self.error().message);
        auto rb = verbs::change_field(j, C, {"name", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false});
        check(P + "links: SQLite -- the parent can't be made again (others point at it), in words",
              !rb && rb.error().message.find("links to “acorn_customers”") != std::string::npos,
              rb ? "changed!" : rb.error().message);
    }
}


// ── s009: the solution document ──────────────────────────────────────────────
void test_solution_pure(const fs::path& dir) {
    core::SavedConnection f;
    f.engine = "sqlite";
    f.path = (dir / "shop.sqlite").string();
    check("solution: a file's sits beside it (shop.sqlite -> shop.acorn)",
          core::solution_path(f, "/x/acorn", true) == (dir / "shop.acorn").string(), core::solution_path(f, "/x/acorn", true));
    const auto away = core::solution_path(f, "/x/acorn", false);
    check("solution: ... in Acorn's own folder when its folder can't be written",
          away.rfind("/x/acorn/solutions/shop-", 0) == 0 && away.size() > 6 && away.substr(away.size() - 6) == ".acorn", away);
    core::SavedConnection srv;
    srv.engine = "postgres"; srv.host = "db.lan"; srv.port = 5433; srv.database = "shop"; srv.user = "scott";
    const auto sp = core::solution_path(srv, "/x/acorn", true);
    check("solution: a server's in Acorn's own folder, a safe name from its key",
          sp.rfind("/x/acorn/solutions/", 0) == 0 && sp.find(' ') == std::string::npos && sp.find(':', 1) == std::string::npos, sp);

    core::Solution s;
    s.points_at = srv;
    s.boxes["orders"] = {400.5, 24};
    s.boxes["“odd” name"] = {24, 300};
    const auto file = (dir / "round.acorn").string();
    const bool saved = core::save_solution(file, s);
    auto back = core::load_solution(file);
    check("solution: round-trips (points at a server, boxes by table, odd names)", saved && back && *back == s, "");
    const std::string text = slurp(file);
    check("solution: holds no password field", text.find("password") == std::string::npos && text.find("\"acorn\": 1") != std::string::npos, text);
    { std::ofstream g(dir / "garbage.acorn"); g << "{ not json"; }
    { std::ofstream g(dir / "newer.acorn"); g << "{\"acorn\": 99, \"boxes\": {}}"; }
    check("solution: garbage / a newer version / no file -> none (never half-read)",
          !core::load_solution((dir / "garbage.acorn").string()) && !core::load_solution((dir / "newer.acorn").string()) &&
          !core::load_solution((dir / "missing.acorn").string()), "");
    check("solution: a missing folder is made on save (Acorn's solutions/)",
          core::save_solution((dir / "deep" / "solutions" / "a.acorn").string(), s) && fs::exists(dir / "deep" / "solutions" / "a.acorn"), "");
    check("solution: folder_writable", core::folder_writable(dir.string()) && !core::folder_writable((dir / "nope").string()), "");
}

// s009: Move “t” -- the solution changes, saved; the database is never touched;
// a read-only connection can still place boxes.
void test_boxes(Engine* e, const ConnectParams& ro, const std::string& P) {
    auto c = e->open(ro);
    if (!c) { check(P + "boxes: open read-only", false, c.error().message); return; }
    Gate g(std::move(*c));
    Journal j(g, e->dialect(), e->caps());
    core::Solution sol;
    int saves = 0;
    bool fail_save = false;
    j.store().set_solution(&sol, [&] { ++saves; return !fail_save; });
    auto m = verbs::move_box(j, "customers", 400, 24);
    check(P + "boxes: Move on a READ-ONLY connection -- a step, saved, nothing written to the database",
          m && sol.boxes.count("customers") && sol.boxes["customers"] == core::BoxPlace{400, 24} && saves == 1 &&
          j.undo_label() == "Move “customers”" && g.strays() == 0, m ? j.undo_label() : m.error().message);
    auto m2 = verbs::move_box(j, "customers", 500, 60);
    auto u = j.undo();
    check(P + "boxes: Ctrl+Z moves it back to where it was", u && sol.boxes["customers"] == core::BoxPlace{400, 24},
          u ? "" : u.error().message);
    auto u2 = j.undo();
    check(P + "boxes: ... and again: not placed (the automatic layout has it)", u2 && !sol.boxes.count("customers"),
          u2 ? "" : u2.error().message);
    auto r = j.redo();
    check(P + "boxes: redo places it again", r && sol.boxes.count("customers"), r ? "" : r.error().message);
    const auto n = j.size();
    auto same = verbs::move_box(j, "customers", 400, 24);
    check(P + "boxes: moved where it already is -- no step", same && j.size() == n, "");
    fail_save = true;
    auto bad = verbs::move_box(j, "customers", 1, 1);
    check(P + "boxes: a save that fails -- no step, the place kept as it was",
          !bad && j.size() == n && sol.boxes["customers"] == core::BoxPlace{400, 24}, bad ? "moved!" : bad.error().message);
    Journal none(g, e->dialect(), e->caps());
    auto ns = verbs::move_box(none, "customers", 1, 1);
    check(P + "boxes: no solution -- refused in words", !ns && ns.error().code == Error::Code::Unsupported, ns ? "moved!" : "");
    (void)m2;
}


// ── s010: layouts ────────────────────────────────────────────────────────────
void test_layouts_pure(const fs::path& dir) {
    using core::ObjectKind;
    const auto d = core::default_layout("contacts", {"id", "first_name", "birthday"});
    const bool shape = d.name == "contacts Form" && d.table == "contacts" && d.objects.size() == 6 &&
        d.objects[0] == core::LayoutObject{ObjectKind::Text, "", "id", 16, 16, 144, 24} &&
        d.objects[1] == core::LayoutObject{ObjectKind::Field, "id", "", 168, 16, 320, 24} &&
        d.objects[4].text == "birthday" && d.objects[5].field == "birthday" && d.objects[5].y == 80 &&
        d.width == 504 && d.height == 80 + 24 + 16;
    check("layouts: the default -- \"contacts Form\", a label left and a box right per field, in order", shape,
          d.name + " " + std::to_string(d.width) + "x" + std::to_string(d.height));
    bool on_grid = true;
    for (const auto& o : d.objects)
        on_grid = on_grid && o.x % core::kLayoutGrid == 0 && o.y % core::kLayoutGrid == 0 &&
                  o.w % core::kLayoutGrid == 0 && o.h % core::kLayoutGrid == 0;
    check("layouts: the default sits on the 8 px grid", on_grid && d.width % 8 == 0 && d.height % 8 == 0, "");
    check("layouts: a field the table no longer has is missing (labels never are)",
          core::missing_field(d.objects[5], {"id", "first_name"}) && !core::missing_field(d.objects[5], {"birthday"}) &&
          !core::missing_field(d.objects[4], {}), "");
    check("layouts: the default is a valid layout; a table of no fields still has one",
          core::check_layout(d).empty() && core::check_layout(core::default_layout("t", {})).empty(),
          core::check_layout(d));
    auto bad = d;
    bad.name = " x";
    auto bad2 = d;
    bad2.objects[1].w = 0;
    auto bad3 = d;
    bad3.objects[1].field.clear();
    check("layouts: refused in words -- a name with a space at the end, an object with no size, a field object with no field",
          !core::check_layout(bad).empty() && !core::check_layout(bad2).empty() && !core::check_layout(bad3).empty(), "");

    // Ids: a '/' in the table or the name reads back.
    std::string t, n;
    check("layouts: ids round-trip, a '/' in the table or the name kept",
          Store::parse_layout_id(Store::layout_id("a/b", "c/d Form"), t, n) && t == "a/b" && n == "c/d Form" &&
          !Store::parse_layout_id("layout:9:ab/c", t, n) && !Store::parse_layout_id("box:x", t, n) &&
          Store::is_local(Store::layout_id("x", "y")) && Store::is_local(Store::box_id("x")) &&
          !Store::is_local(Store::table_id("x")), t + "|" + n);

    // In the .acorn: round-trip; a broken layout skipped, the rest still read.
    core::Solution s;
    s.points_at.engine = "sqlite";
    s.points_at.path = (dir / "lay.sqlite").string();
    s.boxes["contacts"] = {8, 8};
    auto l2 = d;
    l2.name = "“odd” / name";
    s.layouts["contacts"][d.name] = d;
    s.layouts["contacts"][l2.name] = l2;
    const auto file = (dir / "lay.acorn").string();
    auto back = core::save_solution(file, s) ? core::load_solution(file) : std::nullopt;
    check("layouts: round-trip in the .acorn (by table, by name, objects in order)", back && *back == s, "");
    {
        std::ofstream g(dir / "half.acorn");
        g << R"({"acorn": 1, "boxes": {"a": {"x": 1, "y": 2}}, "layouts": {"a": {)"
             R"("good": {"width": 64, "height": 32, "objects": [{"kind": "field", "field": "f", "x": 8, "y": 8, "w": 48, "h": 16},)"
             R"( {"kind": "sparkle", "x": 1, "y": 1, "w": 1, "h": 1}]},)"
             R"("bad": {"width": "wide", "height": 32, "objects": []}}}})";
    }
    auto half = core::load_solution((dir / "half.acorn").string());
    check("layouts: a broken layout is skipped, an unknown object kind not drawn -- the rest of the file reads",
          half && half->boxes.count("a") && half->layout("a", "good") && !half->layout("a", "bad") &&
          half->layout("a", "good")->objects.size() == 1, "");
    { std::ofstream g(dir / "s009.acorn"); g << R"({"acorn": 1, "boxes": {"a": {"x": 1, "y": 2}}})"; }
    auto old = core::load_solution((dir / "s009.acorn").string());
    // s010b: the editor's geometry.
    using core::Grab;
    const core::LayoutObject o{ObjectKind::Field, "f", "", 160, 40, 80, 24};
    check("layouts: snap -- to the nearest 8, never below 0; grid 1 leaves it",
          core::snap(13) == 16 && core::snap(11) == 8 && core::snap(-5) == 0 && core::snap(13, 1) == 13, "");
    const auto mv = core::dragged(o, Grab::Move, 13, -3);
    const auto mv1 = core::dragged(o, Grab::Move, 3, 1, 1);
    const auto far = core::dragged(o, Grab::Move, -999, -999);
    check("layouts: a move snaps to the grid (Shift: by the pixel) and stops at the top-left, its size kept",
          mv.x == 176 && mv.y == 40 && mv.w == 80 && mv1.x == 163 && mv1.y == 41 && far.x == 0 && far.y == 0 && far.w == 80,
          std::to_string(mv.x) + "," + std::to_string(mv.y));
    const auto e1 = core::dragged(o, Grab::E, 21);
    const auto w1 = core::dragged(o, Grab::W, -19);
    const auto se = core::dragged(o, Grab::SE, 5, 13);
    const auto nw = core::dragged(o, Grab::NW, 8, 8);
    check("layouts: a side or corner moves only its own edges, snapped -- the other edges stay",
          e1.x == 160 && e1.w == 104 && w1.x == 144 && w1.w == 96 && se.w == 88 && se.h == 40 && se.x == 160 &&
          nw.x == 168 && nw.y == 48 && nw.w == 72 && nw.h == 16,
          std::to_string(e1.w) + " " + std::to_string(w1.x) + "/" + std::to_string(w1.w));
    const auto squash = core::dragged(o, Grab::E, -500);
    const auto squashw = core::dragged(o, Grab::W, 500);
    const auto squash1 = core::dragged(o, Grab::S, -500, -500, 1);
    const auto off = core::dragged(o, Grab::W, -500);
    check("layouts: never smaller than one grid step (1 px with Shift); a left side stops at 0",
          squash.w == 8 && squash.x == 160 && squashw.w == 8 && squashw.x == 232 && squash1.h == 1 &&
          off.x == 0 && off.w == 240, std::to_string(squashw.x) + "/" + std::to_string(squashw.w));
    auto fl = d;
    core::fit(fl);
    auto fl2 = d;
    fl2.objects[1].w = 401;
    core::fit(fl2);
    core::Layout empty{"t", "e", 999, 999, {}};
    core::fit(empty);
    check("layouts: the body fits its objects plus the margin, up to the grid (the default already does)",
          fl.width == d.width && fl.height == d.height && fl2.width == 168 + 401 + 16 + 7 && empty.width == 32,
          std::to_string(fl2.width));
    // s010c: Add, and the drawing kinds.
    auto ad = d;
    const int added = core::add_object(ad, ObjectKind::Field, "email");
    const auto& albl = ad.objects[ad.objects.size() - 2];
    const auto& abox = ad.objects.back();
    check("layouts: Add Field -- its words left and its box right, a row under everything, the body fitted",
          added == 2 && albl.kind == ObjectKind::Text && albl.text == "email" && albl.x == 16 && albl.y == 112 &&
          abox.kind == ObjectKind::Field && abox.field == "email" && abox.x == 168 && abox.y == 112 &&
          ad.height == 112 + 24 + 16 && core::check_layout(ad).empty(),
          std::to_string(albl.y) + " h " + std::to_string(ad.height));
    auto aw = d;
    core::add_object(aw, ObjectKind::Text, "Who");
    core::add_object(aw, ObjectKind::Line);
    core::add_object(aw, ObjectKind::Box);
    const auto& ln = aw.objects[aw.objects.size() - 2];
    const auto& bx = aw.objects.back();
    check("layouts: Add Words / Line / Box -- each under the last, on the grid, a line across the body",
          aw.objects[aw.objects.size() - 3].text == "Who" && ln.kind == ObjectKind::Line && ln.y == 144 &&
          ln.w == d.width - 32 && ln.h == 8 && bx.kind == ObjectKind::Box && bx.y == 160 && bx.w == 160 &&
          core::check_layout(aw).empty(), std::to_string(ln.y) + "/" + std::to_string(bx.y));
    core::Layout none{"t", "n", 32, 32, {}};
    core::add_object(none, ObjectKind::Text, "x");
    check("layouts: the first object on an empty layout sits at the margin", none.objects[0].y == 16, "");
    check("layouts: the fields a layout doesn't show yet, in the table's order",
          core::fields_not_on(d, {"id", "email", "first_name", "fax"}) == std::vector<std::string>{"email", "fax"}, "");
    {
        core::Solution ls;
        ls.layouts["contacts"][aw.name] = aw;
        const auto lp = dir / "drawing.acorn";
        core::save_solution(lp.string(), ls);
        auto lb = core::load_solution(lp.string());
        check("layouts: a line and a box round-trip in the .acorn", lb && *lb == ls, "");
    }
    // s010d: parts.
    {
        auto pl = d;   // 504 x 112: rows at 16, 48, 80
        const bool h1 = core::move_part_line(pl, core::Part::Header, 45);
        check("layouts: the top edge dragged down makes a header -- snapped, everything below moves with it",
              h1 && pl.header == 48 && pl.height == d.height + 48 && pl.objects[0].y == 64 && pl.objects[5].y == 128 &&
              core::part_of(pl, pl.objects[0]) == core::Part::Body && core::check_layout(pl).empty(),
              std::to_string(pl.header) + " " + std::to_string(pl.height));
        pl.objects.push_back({ObjectKind::Text, "", "Title", 16, 8, 144, 24});   // in the header
        check("layouts: an object whose top is in the header belongs to it",
              core::part_of(pl, pl.objects.back()) == core::Part::Header, "");
        core::move_part_line(pl, core::Part::Header, 0);
        check("layouts: a header line can't cut its own objects off (stops under them)",
              pl.header == 32 && pl.objects.back().y == 8 && pl.objects[0].y == 48, std::to_string(pl.header));
        const int h0 = pl.height;
        const bool f1 = core::move_part_line(pl, core::Part::Footer, h0 + 40);
        check("layouts: the bottom edge dragged down makes a footer below", f1 && pl.footer == 40 && pl.height == h0 + 40,
              std::to_string(pl.footer));
        pl.objects.push_back({ObjectKind::Text, "", "Page", 16, core::footer_top(pl) + 8, 144, 24});
        auto big = core::default_layout("t", {"a"});
        const int ft = core::footer_top(pl);
        core::add_object(pl, ObjectKind::Field, "email");
        const auto& added = pl.objects[pl.objects.size() - 2];
        check("layouts: Add lands in the body; the footer moves down with its objects to make room",
              core::part_of(pl, added) == core::Part::Body && core::footer_top(pl) > ft &&
              pl.objects[pl.objects.size() - 3].y == core::footer_top(pl) + 8 && pl.footer == 40 &&
              core::check_layout(pl).empty(), std::to_string(added.y) + " ft " + std::to_string(core::footer_top(pl)));
        const int fb = core::footer_top(pl);
        core::move_part_line(pl, core::Part::Footer, 0);
        check("layouts: the footer line stops under the body's objects, with the room fit() keeps",
              core::footer_top(pl) <= fb && core::footer_top(pl) >= added.y + added.h + core::kFormGap,
              std::to_string(core::footer_top(pl)));
        core::move_layout_bottom(pl, 0);
        check("layouts: the bottom edge stops under the footer's objects",
              pl.height - core::footer_top(pl) == 8 + 24 + 0 || pl.objects[pl.objects.size() - 3].y + 24 <= pl.height, "");
        core::Solution ps;
        ps.layouts[pl.table][pl.name] = pl;
        const auto pp = dir / "parts.acorn";
        core::save_solution(pp.string(), ps);
        auto pb = core::load_solution(pp.string());
        check("layouts: parts round-trip in the .acorn", pb && *pb == ps, core::check_layout(pl) + " h" + std::to_string(pl.height) + " f" + std::to_string(pl.footer) + " hd" + std::to_string(pl.header));
        auto bad = d;
        bad.header = 60; bad.footer = 60;
        check("layouts: header + footer leaving no body -- refused in words", !core::check_layout(bad).empty(), "");
        (void)big;
    }
    // s010f: the sample's Contact Card.
    {
        const auto card = sample_layout();
        const std::vector<std::string> cf{"id", "first_name", "last_name", "company", "email", "birthday", "notes"};
        bool fields_ok = true;
        for (const auto& o : card.objects) fields_ok = fields_ok && !core::missing_field(o, cf);
        check("sample: the Contact Card passes check_layout, every field on it is a contacts field, on the grid",
              core::check_layout(card).empty() && fields_ok && card.header == 48 && card.footer == 40 &&
              core::body_objects(card).size() == card.objects.size() - 2, core::check_layout(card));
    }
    // s011a: portals.
    {
        const auto card = sample_layout();
        const core::LayoutObject* port = nullptr;
        for (const auto& o : card.objects) if (o.kind == core::ObjectKind::Portal) port = &o;
        check("portals: the Contact Card lists the contact's phones (by contact_id: kind, number), in the body",
              port && port->from == "phones" && port->field == "contact_id" &&
              port->columns == std::vector<std::string>{"kind", "number"} && core::part_of(card, *port) == core::Part::Body &&
              port->y + port->h <= core::footer_top(card), "");
        const std::vector<core::PortalLink> links{{"phones", "contact_id"}, {"addresses", "contact_id"}};
        auto gone = *port;
        gone.field = "owner_id";
        check("portals: a portal whose link is there isn't missing; one whose link went is (fields and words never are)",
              !core::missing_link(*port, links) && core::missing_link(gone, links) && !core::missing_link(gone, {{"phones", "owner_id"}}) &&
              !core::missing_link(card.objects[0], {}), "");
        check("portals: a new one's columns -- every field but the link and the counting key",
              core::portal_columns({"id", "contact_id", "kind", "number"}, "contact_id", "id") == std::vector<std::string>{"kind", "number"} &&
              core::portal_columns({"contact_id", "number"}, "contact_id") == std::vector<std::string>{"number"}, "");
        auto l = core::default_layout("contacts", {"id", "first_name"});
        const int below = core::next_row_y(l);
        core::add_portal(l, {"phones", "contact_id"}, {"kind", "number"});
        const auto& p = l.objects.back();
        check("portals: Add Portal -- under everything, a default row wide, the names plus four records tall, on the grid, fitted",
              p.kind == core::ObjectKind::Portal && p.y == below && p.x == 16 && p.w == 472 && p.h == 120 &&
              p.y % 8 == 0 && p.h % 8 == 0 && l.height >= p.y + p.h + 16 && core::check_layout(l).empty(),
              std::to_string(p.y) + " " + std::to_string(l.height));
        check("portals: its name in the notes; Add Field still offers every field (the link field is another table's)",
              core::object_label(p) == "the portal to “phones”" && core::link_label({"phones", "contact_id"}) == "phones · contact_id" &&
              core::fields_not_on(core::default_layout("phones", {"id", "contact_id"}), {"id", "contact_id"}).empty() &&
              core::add_object(l, core::ObjectKind::Portal) == 0, core::object_label(p));
        // The drive's catch: Add on a body full to the footer line -- the footer moves down, the new object stays in the body.
        auto full = sample_layout();
        const int ft0 = core::footer_top(full), foot0 = full.footer;
        core::add_portal(full, {"addresses", "contact_id"}, {"kind", "city"});
        auto fw = sample_layout();
        core::add_object(fw, core::ObjectKind::Text, "More");
        check("portals: Add with a footer in the way -- the footer (and its words) moves down, the new object is in the body",
              core::part_of(full, full.objects.back()) == core::Part::Body && full.footer == foot0 &&
              core::footer_top(full) > ft0 && full.objects[full.objects.size() - 2].y == 392 + (core::footer_top(full) - ft0) &&
              core::part_of(fw, fw.objects.back()) == core::Part::Body && fw.footer == foot0 && core::check_layout(full).empty(),
              std::to_string(full.objects.back().y) + " ft " + std::to_string(core::footer_top(full)) + " footer " + std::to_string(full.footer));
        // s011c: columns.
        auto pc = *port;   // kind, number
        check("portals: columns not on it -- the table's order, never the link field",
              core::columns_not_on(pc, {"id", "contact_id", "kind", "number"}) == std::vector<std::string>{"id"} &&
              core::columns_not_on(gone, {"contact_id", "owner_id", "kind"}) == std::vector<std::string>{"contact_id"}, "");
        auto mv = pc;
        const bool m1 = core::move_column(mv, 1, -1);
        const bool m2 = core::move_column(mv, 0, -1), m3 = core::move_column(mv, 1, 1), m4 = core::move_column(mv, 5, 1);
        check("portals: a column moved left; not past either end, nor one that isn't there",
              m1 && mv.columns == std::vector<std::string>{"number", "kind"} && !m2 && !m3 && !m4, "");
        auto nofrom = l;
        nofrom.objects.back().from.clear();
        check("portals: one with no table or no link field -- refused in words", !core::check_layout(nofrom).empty(), "");
        core::Solution ps;
        ps.points_at.engine = "sqlite";
        ps.points_at.path = (dir / "portal.sqlite").string();
        ps.layouts["contacts"][card.name] = card;
        const auto pf = (dir / "portal.acorn").string();
        auto pb = core::save_solution(pf, ps) ? core::load_solution(pf) : std::nullopt;
        std::ifstream in(pf);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check("portals: round-trip in the .acorn -- {\"kind\": \"portal\", \"from\", \"field\", \"columns\"}",
              pb && *pb == ps && text.find("\"portal\"") != std::string::npos && text.find("\"columns\"") != std::string::npos, "");
    }
    // s011d: related fields -- a parent's field on the child's layout.
    {
        const std::vector<core::RelatedLink> outs{{"contact_id", "contacts"}};
        auto l = core::default_layout("addresses", {"id", "contact_id", "kind", "street"});
        const int below = core::next_row_y(l);
        const int n = core::add_related(l, outs[0], "last_name");
        const auto& r = l.objects.back();
        const auto& w = l.objects[l.objects.size() - 2];
        check("related: Add Related Field -- a row under everything: its name as words left, the value right, fitted",
              n == 2 && r.kind == core::ObjectKind::Related && r.field == "contact_id" && r.from == "contacts" && r.shows == "last_name" &&
              r.y == below && r.x == 168 && r.w == 320 && r.h == 24 && w.kind == core::ObjectKind::Text && w.text == "last_name" &&
              w.y == below && l.height >= r.y + r.h + 16 && core::check_layout(l).empty(), std::to_string(r.y));
        auto gone = r;
        gone.field = "owner_id";
        check("related: its link there -> not missing; the link gone -> missing; fields, words and portals never are",
              !core::missing_related_link(r, outs) && core::missing_related_link(gone, outs) &&
              !core::missing_related_link(l.objects[0], {}) && !core::missing_related_link(r, {{"contact_id", "contacts"}, {"x", "y"}}), "");
        const std::vector<std::string> cf{"id", "first_name", "last_name", "email"};
        check("related: what Add offers -- the parent's fields not shown yet through that link, never its key",
              core::related_not_on(l, outs[0], cf, "id") == std::vector<std::string>{"first_name", "email"} &&
              core::related_not_on(l, {"other_id", "contacts"}, cf, "id") == std::vector<std::string>{"first_name", "last_name", "email"} &&
              core::related_label("contact_id", "last_name") == "contact_id → last_name", "");
        check("related: its name in the notes; Add Field still offers the table's own fields; a bare Add makes none",
              core::object_label(r) == "“contacts.last_name”" &&
              core::fields_not_on(l, {"id", "contact_id", "kind", "street", "last_name"}) == std::vector<std::string>{"last_name"} &&
              core::add_object(l, core::ObjectKind::Related) == 0, core::object_label(r));
        auto noshow = l;
        noshow.objects.back().shows.clear();
        auto noto = l;
        noto.objects.back().from.clear();
        check("related: one with no link field, table or shown field -- refused in words",
              !core::check_layout(noshow).empty() && !core::check_layout(noto).empty(), "");
        auto full = sample_layout();
        const int ft0 = core::footer_top(full);
        core::add_related(full, {"contact_id", "contacts"}, "email");
        check("related: Add with a footer in the way -- the footer moves down, the row stays in the body",
              core::part_of(full, full.objects.back()) == core::Part::Body && core::footer_top(full) > ft0 && core::check_layout(full).empty(), "");
        core::Solution rs;
        rs.points_at.engine = "sqlite";
        rs.points_at.path = (dir / "related.sqlite").string();
        rs.layouts["addresses"][l.name] = l;
        const auto rf = (dir / "related.acorn").string();
        auto rb = core::save_solution(rf, rs) ? core::load_solution(rf) : std::nullopt;
        std::ifstream in(rf);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check("related: round-trip in the .acorn -- {\"kind\": \"related\", \"field\", \"from\", \"shows\"}",
              rb && *rb == rs && text.find("\"related\"") != std::string::npos && text.find("\"shows\"") != std::string::npos, "");
    }
    // s012a: the list view -- a layout's parts, each drawn on its own.
    {
        const auto card = sample_layout();   // header 48, footer 40, 424 tall
        const auto hs = core::part_objects(card, core::Part::Header);
        const auto bs = core::part_objects(card, core::Part::Body);
        const auto fs = core::part_objects(card, core::Part::Footer);
        bool parts_right = hs.size() + bs.size() + fs.size() == card.objects.size() && !hs.empty() && !fs.empty();
        for (int i : hs) parts_right = parts_right && core::part_of(card, card.objects[i]) == core::Part::Header;
        for (int i : bs) parts_right = parts_right && core::part_of(card, card.objects[i]) == core::Part::Body;
        for (int i : fs) parts_right = parts_right && core::part_of(card, card.objects[i]) == core::Part::Footer;
        check("list: every object in exactly one part -- the Contact Card's header, body and footer",
              parts_right, std::to_string(hs.size()) + "/" + std::to_string(bs.size()) + "/" + std::to_string(fs.size()));
        bool behind_first = true, seen_front = false;
        for (int i : bs) {
            const auto k = card.objects[i].kind;
            const bool behind = k == core::ObjectKind::Line || k == core::ObjectKind::Box;
            if (!behind) seen_front = true;
            else if (seen_front) behind_first = false;
        }
        check("list: a part draws its lines and boxes first (behind the fields)", behind_first, "");
        check("list: the parts' places -- header 0 / 48, body 48 / 336, footer 384 / 40",
              core::part_top(card, core::Part::Header) == 0 && core::part_height(card, core::Part::Header) == 48 &&
              core::part_top(card, core::Part::Body) == 48 && core::part_height(card, core::Part::Body) == 336 &&
              core::part_top(card, core::Part::Footer) == 384 && core::part_height(card, core::Part::Footer) == 40,
              std::to_string(card.height));
        const auto plain = core::default_layout("contacts", {"id", "first_name"});
        check("list: a layout with no parts is all body -- no header, no footer to draw",
              core::part_height(plain, core::Part::Header) == 0 && core::part_height(plain, core::Part::Footer) == 0 &&
              core::part_height(plain, core::Part::Body) == plain.height &&
              core::part_objects(plain, core::Part::Body).size() == plain.objects.size(), "");
    }
    // s012b: words alignment.
    {
        check("align: names round-trip; anything else reads as right (the default)",
              core::parse_align(core::align_name(core::Align::Left)) == core::Align::Left &&
              core::parse_align(core::align_name(core::Align::Center)) == core::Align::Center &&
              core::parse_align("right") == core::Align::Right && core::parse_align("") == core::Align::Right &&
              core::parse_align("justify") == core::Align::Right && core::LayoutObject{}.align == core::Align::Right, "");
        core::Solution as;
        as.points_at.engine = "sqlite";
        as.points_at.path = (dir / "align.sqlite").string();
        auto al = sample_list_layout();
        al.objects.push_back({core::ObjectKind::Text, "", "Centre", 24, 88, 96, 24});
        al.objects.back().align = core::Align::Center;
        al.objects.push_back({core::ObjectKind::Text, "", "Plain", 128, 88, 96, 24});   // right: the default
        as.layouts["contacts"][al.name] = al;
        const auto af = (dir / "align.acorn").string();
        auto ab = core::save_solution(af, as) ? core::load_solution(af) : std::nullopt;
        std::ifstream ain(af);
        const std::string atext((std::istreambuf_iterator<char>(ain)), std::istreambuf_iterator<char>());
        std::size_t rights = 0;
        for (std::size_t at = atext.find("\"right\""); at != std::string::npos; at = atext.find("\"right\"", at + 1)) ++rights;
        check("align: round-trip in the .acorn -- \"align\" written for left / center, never for right",
              ab && *ab == as && atext.find("\"left\"") != std::string::npos && atext.find("\"center\"") != std::string::npos &&
              rights == 0, std::to_string(rights));
        const auto sl = sample_list_layout();
        std::size_t lefts = 0;
        for (const auto& o : sl.objects) if (o.kind == core::ObjectKind::Text && o.align == core::Align::Left) ++lefts;
        check("align: the sample's Contact List -- a valid layout, titles in the header, one row of fields, left-aligned words",
              core::check_layout(sl).empty() && core::part_objects(sl, core::Part::Header).size() == 4 &&
              core::part_objects(sl, core::Part::Body).size() == 4 && core::part_objects(sl, core::Part::Footer).size() == 1 &&
              lefts == 4 && core::missing_field(sl.objects[4], {"id", "first_name"}) == false,
              core::check_layout(sl));
    }
    // s010e: several picked.
    {
        std::vector<int> pk;
        core::toggle_pick(pk, 5);
        core::toggle_pick(pk, 1);
        core::toggle_pick(pk, 3);
        core::toggle_pick(pk, 5);
        core::toggle_pick(pk, -1);
        check("layouts: Shift-click adds to the pick or drops -- kept sorted, no repeats",
              pk == std::vector<int>{1, 3} && core::is_picked(pk, 3) && !core::is_picked(pk, 5), "");
        auto ml = d;   // 504 x 112: labels at x 16, boxes at x 168; rows at 16, 48, 80
        const auto m1 = core::moved_objects(ml, {0, 1}, 1, 13, 21);
        check("layouts: a drag moves every picked object together -- the grabbed one snapped, the rest by as much",
              m1.objects[1].x == 184 && m1.objects[1].y == 40 && m1.objects[0].x == 32 && m1.objects[0].y == 40 &&
              m1.objects[2] == ml.objects[2] && m1.objects[3] == ml.objects[3],
              std::to_string(m1.objects[0].x) + "," + std::to_string(m1.objects[0].y));
        const auto m2 = core::moved_objects(ml, {0, 1, 3}, 1, -100, -100);
        check("layouts: the group stops whole at the top / left -- it keeps its shape",
              m2.objects[0].x == 0 && m2.objects[1].x == 152 && m2.objects[0].y == 0 && m2.objects[3].y == 32 &&
              m2.objects[3].x == 152, std::to_string(m2.objects[1].x) + "," + std::to_string(m2.objects[3].y));
        const auto m3 = core::moved_objects(ml, {2, 3}, 2, 1, 0, 1);
        check("layouts: Shift (grid 1) moves the group by the pixel",
              m3.objects[2].x == 17 && m3.objects[3].x == 169 && m3.objects[0] == ml.objects[0], "");
        check("layouts: no pick, nothing moves", core::moved_objects(ml, {}, -1, 40, 40) == ml, "");
        auto rl = d;
        const auto keep = rl.objects[2];
        const int gone = core::remove_objects(rl, {5, 0, 1, 1, 99});
        check("layouts: Delete takes every picked object off -- the rest stay in order",
              gone == 3 && rl.objects.size() == d.objects.size() - 3 && rl.objects[0] == keep, std::to_string(gone));
        check("layouts: what a step calls the pick -- one by its name, several by count",
              core::picks_label(d, {1}) == "“" + d.objects[1].field + "”" && core::picks_label(d, {0, 1, 2}) == "3 objects", "");
        auto pa = d;
        core::move_part_line(pa, core::Part::Header, 40);
        pa.objects.push_back({ObjectKind::Text, "", "Title", 16, 8, 144, 24});   // in the header
        core::move_part_line(pa, core::Part::Footer, pa.height + 40);
        pa.objects.push_back({ObjectKind::Text, "", "Page", 16, core::footer_top(pa) + 8, 144, 24});
        const auto all = core::body_objects(pa);
        check("layouts: Ctrl+A picks the body's objects -- not the header's, not the footer's",
              all.size() == d.objects.size() && !core::is_picked(all, static_cast<int>(pa.objects.size()) - 1) &&
              !core::is_picked(all, static_cast<int>(pa.objects.size()) - 2), std::to_string(all.size()));
    }
    check("layouts: a free name -- contacts Form 2, 3 ...",
          core::unique_layout_name("contacts Form", {"x"}) == "contacts Form" &&
          core::unique_layout_name("contacts Form", {"contacts Form", "contacts Form 2"}) == "contacts Form 3", "");
    check("layouts: an s009 file (no layouts) reads -- no layouts", old && old->layouts.empty() && old->boxes.size() == 1, "");
}

// s010: Save / Delete Layout -- steps on the same list, kept in the solution,
// on a read-only connection too; the database never touched.
void test_layouts(Engine* e, const ConnectParams& ro, const std::string& P) {
    auto c = e->open(ro);
    if (!c) { check(P + "layouts: open read-only", false, c.error().message); return; }
    Gate g(std::move(*c));
    Journal j(g, e->dialect(), e->caps());
    core::Solution sol;
    int saves = 0;
    bool fail_save = false;
    j.store().set_solution(&sol, [&] { ++saves; return !fail_save; });
    auto l = core::default_layout("customers", {"id", "name"});
    auto m = verbs::save_layout(j, l);
    check(P + "layouts: Save on a READ-ONLY connection -- \"New Layout\", a step, saved, nothing written to the database",
          m && sol.layout("customers", "customers Form") && *sol.layout("customers", "customers Form") == l && saves == 1 &&
          j.undo_label() == "New Layout “customers Form”" && g.strays() == 0, m ? j.undo_label() : m.error().message);
    auto moved = l;
    moved.objects[1].x += 16;
    auto m2 = verbs::save_layout(j, moved);
    check(P + "layouts: an edit is \"Change Layout\"", m2 && j.undo_label() == "Change Layout “customers Form”" &&
          *sol.layout("customers", "customers Form") == moved, m2 ? j.undo_label() : m2.error().message);
    const auto n = j.size();
    auto same = verbs::save_layout(j, moved);
    check(P + "layouts: saving what is kept -- no step", same && j.size() == n, "");
    auto u = j.undo();
    check(P + "layouts: Ctrl+Z puts the layout back as it was", u && *sol.layout("customers", "customers Form") == l,
          u ? "" : u.error().message);
    auto u2 = j.undo();
    check(P + "layouts: ... and again: none kept (the default is computed again)", u2 && sol.layouts.empty(),
          u2 ? "" : u2.error().message);
    auto r = j.redo();
    check(P + "layouts: redo keeps it again", r && sol.layout("customers", "customers Form"), r ? "" : r.error().message);
    auto del = verbs::delete_layout(j, "customers", "customers Form");
    check(P + "layouts: Delete Layout -- gone from the solution, one step",
          del && !sol.layout("customers", "customers Form") && j.undo_label() == "Delete Layout “customers Form”",
          del ? j.undo_label() : del.error().message);
    auto ud = j.undo();
    check(P + "layouts: ... Ctrl+Z brings it back exactly", ud && sol.layout("customers", "customers Form") &&
          *sol.layout("customers", "customers Form") == l, ud ? "" : ud.error().message);
    auto none = verbs::delete_layout(j, "customers", "nope");
    check(P + "layouts: deleting a layout that isn't there -- refused in words", !none && none.error().code == Error::Code::NotFound &&
          none.error().message.find("nope") != std::string::npos, none ? "deleted!" : none.error().message);
    auto badl = l;
    badl.name = "";
    const auto n2 = j.size();
    auto b = verbs::save_layout(j, badl);
    check(P + "layouts: a layout with no name -- refused in words, no step", !b && j.size() == n2, b ? "saved!" : b.error().message);
    fail_save = true;
    auto lost = verbs::save_layout(j, moved);
    check(P + "layouts: a save that fails -- no step, the layout kept as it was",
          !lost && j.size() == n2 && *sol.layout("customers", "customers Form") == l, lost ? "saved!" : lost.error().message);
    fail_save = false;
    sol.boxes["customers"] = {1, 2};
    auto both = verbs::move_box(j, "customers", 3, 4);
    auto ub = j.undo();
    check(P + "layouts: a box's undo leaves the layouts alone", both && ub && sol.boxes["customers"] == core::BoxPlace{1, 2} &&
          *sol.layout("customers", "customers Form") == l, "");
    // s010b: Rename Layout -- one step; Ctrl+Z puts the old name back.
    auto rn = verbs::rename_layout(j, "customers", "customers Form", "Front Desk");
    check(P + "layouts: Rename Layout -- one step, the old name gone, the new one kept as it was",
          rn && !sol.layout("customers", "customers Form") && sol.layout("customers", "Front Desk") &&
          sol.layout("customers", "Front Desk")->objects == l.objects &&
          j.undo_label() == "Rename Layout “customers Form” to “Front Desk”", rn ? j.undo_label() : rn.error().message);
    auto urn = j.undo();
    check(P + "layouts: ... Ctrl+Z puts the old name back", urn && sol.layout("customers", "customers Form") &&
          !sol.layout("customers", "Front Desk") && *sol.layout("customers", "customers Form") == l, "");
    auto dup = l;
    dup.name = "Front Desk";
    (void)verbs::save_layout(j, dup);
    const auto n3 = j.size();
    auto clash = verbs::rename_layout(j, "customers", "customers Form", "Front Desk");
    auto gone = verbs::rename_layout(j, "customers", "nope", "x");
    auto blank = verbs::rename_layout(j, "customers", "customers Form", " ");
    auto same2 = verbs::rename_layout(j, "customers", "customers Form", "customers Form");
    check(P + "layouts: Rename refused in words -- the name taken, no such layout, a blank name; the same name is no step",
          !clash && clash.error().code == Error::Code::Exists && !gone && gone.error().code == Error::Code::NotFound &&
          !blank && same2 && j.size() == n3, clash ? "renamed!" : clash.error().message);
    Journal nosol(g, e->dialect(), e->caps());
    auto ns = verbs::save_layout(nosol, l);
    check(P + "layouts: no solution -- refused in words", !ns && ns.error().code == Error::Code::Unsupported, ns ? "saved!" : "");
    check(P + "layouts: the database untouched throughout", g.strays() == 0, std::to_string(g.strays()));
}


// ── s014a: calculation fields ────────────────────────────────────────────────
void test_calc_pure(const fs::path& dir) {
    using core::CalcValue;
    // A record of `people`: the language over it, each answer as shown.
    const std::map<std::string, CalcValue> rec = {
        {"first", CalcValue{std::string("Ada")}}, {"last", CalcValue{std::string("Lovelace")}},
        {"qty", CalcValue{3.0}}, {"price", CalcValue{2.5}}, {"none", CalcValue{}},
        {"born", CalcValue{std::string("1815-12-10")}}, {"word", CalcValue{std::string("abc")}},
        {"numtext", CalcValue{std::string(" 12.5 ")}}, {"accent", CalcValue{std::string("Ünïcode")}},
    };
    const core::CalcLookup look = [&](const std::string& n) -> std::optional<CalcValue> {
        auto it = rec.find(n);
        return it == rec.end() ? std::nullopt : std::optional<CalcValue>(it->second);
    };
    const auto run = [&](const std::string& src) -> std::string {
        auto p = core::parse_calc(src);
        if (auto* why = std::get_if<std::string>(&p)) return "PARSE: " + *why;
        auto o = core::evaluate(std::get<core::CalcExpr>(p), look, "2026-11-07");
        return o.ok() ? core::calc_text(o) : "ERR: " + o.error;
    };
    struct Case { const char* src; const char* want; };
    const Case cases[] = {
        {"1 + 2 * 3", "7"}, {"(1 + 2) * 3", "9"}, {"10 / 4", "2.5"}, {"-qty + 1", "-2"}, {"qty * price", "7.5"},
        {"first & \" \" & last", "Ada Lovelace"}, {"\"say \\\"hi\\\"\"", "say \"hi\""}, {"qty & \"x\"", "3x"},
        {"none + 1", ""}, {"none & \"!\"", "!"}, {"numtext * 2", "25"}, {"1 / 3", "0.3333333333"},
        {"Length(last)", "8"}, {"length(accent)", "7"}, {"Upper(first)", "ADA"}, {"LOWER(last)", "lovelace"},
        {"Left(last, 4)", "Love"}, {"Right(last, 3)", "ace"}, {"Left(accent, 2)", "Ün"}, {"Right(first, 99)", "Ada"},
        {"Left(first, -1)", ""}, {"Trim(\"  x y \")", "x y"}, {"Round(2.567, 2)", "2.57"}, {"Round(2.5)", "3"},
        {"If(qty > 2, \"many\", \"few\")", "many"}, {"If(qty > 5, \"many\")", ""}, {"If(none, 1, 2)", "2"},
        {"qty = 3", "1"}, {"qty <> 3", "0"}, {"qty >= 3 and price < 3", "1"}, {"not (qty = 3) or none", "0"},
        {"born < Today()", "1"}, {"Today()", "2026-11-07"}, {"\"b\" > \"a\"", "1"}, {"first = \"Ada\"", "1"},
        {"If(1, \"y\", 1 / 0)", "y"}, {"qty ≠ 4", "1"},
    };
    std::string bad;
    for (const auto& c : cases)
        if (const auto got = run(c.src); got != c.want) bad += std::string(c.src) + " -> " + got + " (want " + c.want + "); ";
    check("calc: the language -- arithmetic, &, compare, and / or / not, every function, empty", bad.empty(), bad);
    const auto e1 = run("word + 1"), e2 = run("qty / 0"), e3 = run("nope & 1"), e4 = run("Left(first, word)");
    check("calc: a record that can't be worked out says why (a word as a number, / 0, no such field)",
          e1 == "ERR: “abc” is not a number" && e2 == "ERR: divided by zero" &&
          e3 == "ERR: there's no field “nope”" && e4.rfind("ERR: ", 0) == 0, e1 + " | " + e2 + " | " + e3);
    std::string pbad;
    for (const char* src : {"", "1 +", "(1 + 2", "Upper(first", "Upper()", "Left(first)", "Nope(1)", "1 2", "\"open",
                            "Upper", "qty $ 2", "2abc", "and"})
        if (run(src).rfind("PARSE: ", 0) != 0) pbad += std::string(src) + " -> " + run(src) + "; ";
    check("calc: what doesn't parse is refused in words", pbad.empty(), pbad);
    check("calc: the words name the trouble",
          run("(1 + 2") == "PARSE: a closing ) is missing after “(1 + 2”" &&
          run("Left(first)") == "PARSE: “Left” takes 2 values, not 1" &&
          run("Nope(1)") == "PARSE: there's no function “Nope”", run("(1 + 2") + " | " + run("Left(first)"));
    auto pe = core::parse_calc("first & last & first & qty");
    check("calc: the names an expression reads -- each once, in order",
          std::holds_alternative<core::CalcExpr>(pe) &&
          std::get<core::CalcExpr>(pe).names == std::vector<std::string>{"first", "last", "qty"}, "");

    // The check before keeping one.
    const std::vector<std::string> fields = {"id", "first", "last", "qty"};
    std::map<std::string, core::Calc> calcs = {{"full", {"people", "full", "first & \" \" & last"}},
                                              {"shout", {"people", "shout", "Upper(full)"}}};
    check("calc: a good one passes; one reading another passes",
          core::check_calc({"people", "twice", "qty * 2"}, fields, calcs).empty() &&
          core::check_calc({"people", "loud", "shout & \"!\""}, fields, calcs).empty(), "");
    const auto c1 = core::check_calc({"people", "x", "qty + missing"}, fields, calcs);
    const auto c2 = core::check_calc({"people", "full", "shout"}, fields, calcs);
    const auto c3 = core::check_calc({"people", "first", "1"}, fields, calcs);
    const auto c4 = core::check_calc({"people", "two words", "1"}, fields, calcs);
    const auto c5 = core::check_calc({"people", "if", "1"}, fields, calcs);
    const auto c6 = core::check_calc({"people", "self", "self + 1"}, fields, calcs);
    check("calc: refused in words -- a name it reads isn't there, a loop, a field's name, a bad name, a word of the language",
          c1 == "there's no field “missing” in “people”" && c2.find("reading itself") != std::string::npos &&
          c3.find("already has a field") != std::string::npos && !c4.empty() && !c5.empty() &&
          c6.find("reading itself") != std::string::npos, c1 + " | " + c2 + " | " + c3);

    // A record's every calculation, through the bridge from the database's values.
    Table t;
    t.name = "people";
    t.columns = {{"id", "INTEGER", FieldType::Integer, false, std::nullopt, 1, true},
                 {"first", "TEXT", FieldType::Text, true, std::nullopt, 0, false},
                 {"last", "TEXT", FieldType::Text, true, std::nullopt, 0, false},
                 {"qty", "DECIMAL", FieldType::Number, true, std::nullopt, 0, false},
                 {"ok", "BOOLEAN", FieldType::Boolean, true, std::nullopt, 0, false}};
    calcs["twice"] = {"people", "twice", "qty * 2"};
    calcs["okay"] = {"people", "okay", "If(ok, \"yes\", \"no\")"};
    calcs["broken"] = {"people", "broken", "first +"};
    calcs["loop_a"] = {"people", "loop_a", "loop_b"};
    calcs["loop_b"] = {"people", "loop_b", "loop_a & 1"};
    calcs["leans"] = {"people", "leans", "broken & 1"};
    auto out = row_calcs(t, {Value(1), Value("Ada"), Value("Lovelace"), Value("12.50"), Value("t")}, calcs, "2026-11-07");
    check("calc: a record's calculations -- one reading another, a number given as text, a Yes-No as \"t\"",
          core::calc_text(out["full"]) == "Ada Lovelace" && core::calc_text(out["shout"]) == "ADA LOVELACE" &&
          core::calc_text(out["twice"]) == "25" && core::calc_text(out["okay"]) == "yes",
          core::calc_text(out["full"]) + "|" + core::calc_text(out["twice"]) + "|" + core::calc_text(out["okay"]));
    check("calc: ... one that doesn't parse, a loop, one reading a broken one -- \"?\" and the words, the rest unharmed",
          core::calc_text(out["broken"]) == "?" && !out["broken"].error.empty() &&
          !out["loop_a"].ok() && out["loop_a"].error.find("itself") != std::string::npos && !out["loop_b"].ok() &&
          !out["leans"].ok() && out["leans"].error.find("broken") != std::string::npos,
          out["loop_a"].error + " | " + out["leans"].error);
    auto empty = row_calcs(t, {Value(2), Value(), Value(), Value(), Value(0)}, calcs, "2026-11-07");
    check("calc: an empty record -- empty sums, empty joins as nothing",
          core::calc_text(empty["twice"]) == "" && core::calc_text(empty["full"]) == " " && core::calc_text(empty["okay"]) == "no",
          "[" + core::calc_text(empty["full"]) + "]");
    check("calc: values from the database -- NULL, whole, real, text, decimal text, Yes-No, a date, a blob",
          std::holds_alternative<std::monostate>(calc_value(Value(), FieldType::Text)) &&
          calc_value(Value(4), FieldType::Integer) == CalcValue{4.0} && calc_value(Value(1.5), FieldType::Number) == CalcValue{1.5} &&
          calc_value(Value("x"), FieldType::Text) == CalcValue{std::string("x")} &&
          calc_value(Value("3.10"), FieldType::Number) == CalcValue{3.1} && calc_value(Value("f"), FieldType::Boolean) == CalcValue{0.0} &&
          calc_value(Value("2026-01-02"), FieldType::Date) == CalcValue{std::string("2026-01-02")} &&
          std::holds_alternative<std::monostate>(calc_value(Value(Blob{1}), FieldType::Container)), "");
    check("calc: numbers shown -- whole without .0, others to 10 digits",
          core::calc_text(CalcValue{3.0}) == "3" && core::calc_text(CalcValue{-0.5}) == "-0.5" &&
          core::calc_text(CalcValue{1e20}) == "1e+20" && core::today_iso().size() == 10, core::today_iso());

    // Ids and the .acorn.
    std::string ti, na;
    check("calc: ids round-trip, a '/' kept; a calculation is the solution's",
          Store::parse_calc_id(Store::calc_id("a/b", "c"), ti, na) && ti == "a/b" && na == "c" &&
          !Store::parse_calc_id(Store::layout_id("a", "c"), ti, na) && Store::is_local(Store::calc_id("x", "y")),
          ti + "|" + na);
    core::Solution sol;
    sol.points_at.engine = "sqlite";
    sol.points_at.path = (dir / "calc.sqlite").string();
    sol.calcs["people"]["full"] = {"people", "full", "first & \" \" & last"};
    sol.calcs["people"]["twice"] = {"people", "twice", "qty * 2"};
    const auto file = (dir / "calc.acorn").string();
    auto back = core::save_solution(file, sol) ? core::load_solution(file) : std::nullopt;
    check("calc: round-trip in the .acorn", back && *back == sol && back->calc("people", "full"), "");
    {
        std::ofstream g(dir / "calc_half.acorn");
        g << R"({"acorn": 1, "calcs": {"people": {"good": {"expr": "1"}, "bad name": {"expr": "1"}, "noexpr": {}}}})";
    }
    auto half = core::load_solution((dir / "calc_half.acorn").string());
    check("calc: a broken calculation is skipped, the rest of the file reads; none -> no \"calcs\" key",
          half && half->calc("people", "good") && !half->calc("people", "bad name") && !half->calc("people", "noexpr") &&
          slurp(dir / "lay.acorn").find("calcs") == std::string::npos, "");
}

void test_calcs(Engine* e, const ConnectParams& ro, const std::string& P) {
    auto c = e->open(ro);
    if (!c) { check(P + "calcs: open read-only", false, c.error().message); return; }
    Gate g(std::move(*c));
    Journal j(g, e->dialect(), e->caps());
    core::Solution sol;
    int saves = 0;
    j.store().set_solution(&sol, [&] { ++saves; return true; });
    const core::Calc label{"customers", "label", "Upper(name) & \" <\" & email & \">\""};
    auto m = verbs::save_calc(j, label);
    check(P + "calcs: New Calculation on a READ-ONLY connection -- a step, saved, nothing written to the database",
          m && sol.calc("customers", "label") && *sol.calc("customers", "label") == label && saves == 1 &&
          j.undo_label() == "New Calculation “label”" && g.strays() == 0, m ? j.undo_label() : m.error().message);
    auto changed = label;
    changed.expr = "name & \"!\"";
    auto m2 = verbs::save_calc(j, changed);
    const auto n = j.size();
    auto same = verbs::save_calc(j, changed);
    check(P + "calcs: an edit is \"Change Calculation\"; saving what is kept -- no step",
          m2 && j.undo_label() == "Change Calculation “label”" && same && j.size() == n, m2 ? j.undo_label() : m2.error().message);
    auto renamed = changed;
    renamed.name = "tag";
    auto rn = verbs::save_calc(j, renamed, "label");
    check(P + "calcs: a new name -- one step, the old name gone",
          rn && !sol.calc("customers", "label") && sol.calc("customers", "tag") && j.undo_label() == "Change Calculation “label”",
          rn ? j.undo_label() : rn.error().message);
    auto u = j.undo();
    check(P + "calcs: ... Ctrl+Z puts the old name back", u && sol.calc("customers", "label") && !sol.calc("customers", "tag") &&
          *sol.calc("customers", "label") == changed, u ? "" : u.error().message);
    auto del = verbs::delete_calc(j, "customers", "label");
    auto ud = del ? j.undo() : std::expected<std::string, Error>(std::unexpected(del.error()));
    check(P + "calcs: Delete Calculation, Ctrl+Z brings it back exactly",
          del && ud && sol.calc("customers", "label") && *sol.calc("customers", "label") == changed, ud ? "" : ud.error().message);
    const auto lbl0 = j.undo_label();
    auto r1 = verbs::save_calc(j, {"customers", "x", "nosuch + 1"});
    auto r2 = verbs::save_calc(j, {"customers", "name", "1"});
    auto r3 = verbs::save_calc(j, {"nosuch", "x", "1"});
    auto r4 = verbs::save_calc(j, {"customers", "y", "(1"});
    auto r6 = verbs::save_calc(j, {"customers", "label", "1"}, "nosuch");
    auto r7 = verbs::delete_calc(j, "customers", "nosuch");
    const bool none_kept = j.undo_label() == lbl0;
    (void)verbs::save_calc(j, {"customers", "other", "1"});
    const auto n3 = j.size();
    auto r8 = verbs::save_calc(j, {"customers", "other", "1"}, "label");
    check(P + "calcs: refused in words, no step -- a field it reads isn't there, a field's name, no table, no parse, "
              "renaming one that isn't there / onto one that is, deleting one that isn't",
          !r1 && r1.error().message.find("nosuch") != std::string::npos && !r2 && !r3 && r3.error().code == Error::Code::NotFound &&
          !r4 && !r6 && !r7 && none_kept && !r8 && r8.error().code == Error::Code::Exists && j.size() == n3,
          std::string(r1 ? "1" : "") + (r2 ? "2" : "") + (r3 ? "3" : "") + (r4 ? "4" : "") + (r6 ? "6" : "") + (r7 ? "7" : "") +
              (r8 ? "8" : "") + (none_kept ? "" : " a step kept"));
    auto loop1 = verbs::save_calc(j, {"customers", "a", "other + 1"});
    auto loop2 = verbs::save_calc(j, {"customers", "other", "a + 1"});
    check(P + "calcs: a change that would make a loop -- refused", loop1 && !loop2 &&
          loop2.error().message.find("itself") != std::string::npos, loop2 ? "kept!" : loop2.error().message);
    Journal nosol(g, e->dialect(), e->caps());
    auto ns = verbs::save_calc(nosol, label);
    check(P + "calcs: no solution -- refused in words; the database untouched throughout",
          !ns && ns.error().code == Error::Code::Unsupported && g.strays() == 0, std::to_string(g.strays()));
    // Worked out over the fixture's own records, each against the same rule in C++.
    const Table* t = nullptr;
    auto sch = g.introspect();
    if (sch) t = sch->table("customers");
    auto page = t ? read_page(g, *t, PageRequest{}) : std::expected<Page, Error>(std::unexpected(Error{}));
    bool same_all = page.has_value() && !page->rows.empty();
    std::string why;
    const std::map<std::string, core::Calc> cs = {{"label", {"customers", "label", "Upper(name) & \"/\" & Length(name)"}}};
    for (std::size_t i = 0; same_all && i < page->rows.size(); ++i) {
        const auto& row = page->rows[i];
        std::size_t ni = 0;
        while (ni < t->columns.size() && t->columns[ni].name != "name") ++ni;
        const std::string nm = row[ni].as_text() ? *row[ni].as_text() : "";
        std::size_t cps = 0;
        for (unsigned char ch : nm) if ((ch & 0xC0) != 0x80) ++cps;   // Length counts letters, not bytes
        const std::string want = upper_of(nm) + "/" + std::to_string(cps);
        const auto got = core::calc_text(row_calcs(*t, row, cs, "2026-11-07")["label"]);
        if (got != want) { same_all = false; why = got + " vs " + want; }
    }
    check(P + "calcs: worked out over the table's own records (Upper, &, Length)", same_all,
          page ? why : page.error().message);
}

// ── s014b: summaries ─────────────────────────────────────────────────────────
void test_summary_pure(const fs::path& dir) {
    using core::SummaryOp;
    bool ops = true;
    for (auto op : {SummaryOp::Count, SummaryOp::Total, SummaryOp::Average, SummaryOp::Min, SummaryOp::Max})
        ops = ops && core::parse_summary_op(core::summary_op_name(op)) == op;
    check("summary: each kind's word reads back; an unknown one doesn't; \"Total of balance\"",
          ops && !core::parse_summary_op("sum") && core::summary_says({"t", "x", SummaryOp::Total, "balance"}) == "Total of balance", "");
    const std::vector<std::string> fields = {"id", "name", "balance"};
    const auto a = core::check_summary({"t", "total_balance", SummaryOp::Total, "balance"}, fields, {"label"});
    const auto b = core::check_summary({"t", "label", SummaryOp::Count, "name"}, fields, {"label"});
    const auto c = core::check_summary({"t", "name", SummaryOp::Count, "name"}, fields, {});
    const auto d = core::check_summary({"t", "x", SummaryOp::Count, "nope"}, fields, {});
    const auto e = core::check_summary({"t", "", SummaryOp::Count, "name"}, fields, {});
    const auto f = core::check_summary({"t", "x", SummaryOp::Count, ""}, fields, {});
    check("summary: a good one passes; refused in words -- a calculation's name, a field's, no such field, no name, no field",
          a.empty() && b.find("already has a calculation or summary") != std::string::npos &&
          c.find("already has a field") != std::string::npos && d.find("no field") != std::string::npos &&
          e == "A summary needs a name" && !f.empty(), b + " | " + e);
    std::string ti, na;
    check("summary: ids round-trip; a summary is the solution's",
          Store::parse_summary_id(Store::summary_id("a/b", "s"), ti, na) && ti == "a/b" && na == "s" &&
          !Store::parse_summary_id(Store::calc_id("a", "s"), ti, na) && Store::is_local(Store::summary_id("x", "y")), "");
    core::Solution sol;
    sol.points_at.engine = "sqlite";
    sol.points_at.path = (dir / "sum.sqlite").string();
    sol.summaries["t"]["total_balance"] = {"t", "total_balance", SummaryOp::Total, "balance"};
    sol.summaries["t"]["newest"] = {"t", "newest", SummaryOp::Max, "joined"};
    sol.calcs["t"]["share"] = {"t", "share", "balance / total_balance"};
    const auto file = (dir / "sum.acorn").string();
    auto back = core::save_solution(file, sol) ? core::load_solution(file) : std::nullopt;
    check("summary: round-trip in the .acorn beside the calculations", back && *back == sol, "");
    {
        std::ofstream g(dir / "sum_half.acorn");
        g << R"({"acorn": 1, "summaries": {"t": {"good": {"op": "average", "of": "b"}, "bad": {"op": "sum", "of": "b"},)"
             R"( "nofield": {"op": "count"}}}})";
    }
    auto half = core::load_solution((dir / "sum_half.acorn").string());
    check("summary: a broken one is skipped (an unknown kind, no field), the rest reads",
          half && half->summary("t", "good") && half->summary("t", "good")->op == SummaryOp::Average &&
          !half->summary("t", "bad") && !half->summary("t", "nofield"), "");
    // A calculation reading a summary: through `extra`, like a field.
    Table t;
    t.name = "t";
    t.columns = {{"id", "INTEGER", FieldType::Integer, false, std::nullopt, 1, true},
                 {"balance", "REAL", FieldType::Number, true, std::nullopt, 0, false}};
    const std::map<std::string, core::CalcValue> extra = {{"total_balance", core::CalcValue{200.0}}};
    auto out = row_calcs(t, {Value(1), Value(50.0)}, sol.calcs_of("t"), "2026-11-07", &extra);
    auto none = row_calcs(t, {Value(1), Value(50.0)}, sol.calcs_of("t"), "2026-11-07");
    check("summary: a calculation reads one like a field (50 / 200 -> 0.25); without it, \"?\"",
          core::calc_text(out["share"]) == "0.25" && !none["share"].ok(), core::calc_text(out["share"]));
}

void test_summaries(Engine* e, const ConnectParams& ro, const std::string& P) {
    using core::SummaryOp;
    auto c = e->open(ro);
    if (!c) { check(P + "summaries: open read-only", false, c.error().message); return; }
    Gate g(std::move(*c));
    Journal j(g, e->dialect(), e->caps());
    core::Solution sol;
    j.store().set_solution(&sol, [] { return true; });
    const core::Summary tb{"customers", "total_balance", SummaryOp::Total, "balance"};
    auto m = verbs::save_summary(j, tb);
    check(P + "summaries: New Summary on a READ-ONLY connection -- a step, kept, nothing written to the database",
          m && sol.summary("customers", "total_balance") && j.undo_label() == "New Summary “total_balance”" &&
          g.strays() == 0, m ? j.undo_label() : m.error().message);
    auto avg = tb;
    avg.op = SummaryOp::Average;
    auto m2 = verbs::save_summary(j, avg);
    auto rn = avg;
    rn.name = "avg_balance";
    auto m3 = verbs::save_summary(j, rn, "total_balance");
    check(P + "summaries: a change is \"Change Summary\"; a new name in the same step",
          m2 && m3 && !sol.summary("customers", "total_balance") && sol.summary("customers", "avg_balance") &&
          j.undo_label() == "Change Summary “total_balance”", m3 ? j.undo_label() : m3.error().message);
    auto u = j.undo();
    auto del = verbs::delete_summary(j, "customers", "total_balance");
    auto ud = del ? j.undo() : std::expected<std::string, Error>(std::unexpected(del.error()));
    check(P + "summaries: Ctrl+Z puts the old name back; Delete Summary, Ctrl+Z brings it back exactly",
          u && del && ud && sol.summary("customers", "total_balance") && *sol.summary("customers", "total_balance") == avg,
          ud ? "" : ud.error().message);
    (void)verbs::save_calc(j, {"customers", "label", "Upper(name)"});
    const auto lbl = j.undo_label();
    auto r1 = verbs::save_summary(j, {"customers", "x", SummaryOp::Total, "name"});
    auto r2 = verbs::save_summary(j, {"customers", "x", SummaryOp::Count, "photo"});
    auto r3 = verbs::save_summary(j, {"customers", "label", SummaryOp::Count, "name"});
    auto r4 = verbs::save_summary(j, {"customers", "email", SummaryOp::Count, "name"});
    auto r5 = verbs::save_summary(j, {"nosuch", "x", SummaryOp::Count, "a"});
    auto r6 = verbs::save_summary(j, {"customers", "x", SummaryOp::Count, "nosuch"});
    auto r7 = verbs::delete_summary(j, "customers", "nosuch");
    auto r8 = verbs::save_calc(j, {"customers", "total_balance", "1"});
    check(P + "summaries: refused in words, no step -- Total of text, a binary field, a calculation's name, a field's, "
              "no table, no field, deleting one that isn't there; a calculation can't take a summary's name",
          !r1 && r1.error().message.find("needs a number") != std::string::npos && !r2 && !r3 && !r4 && !r5 && !r6 && !r7 &&
          !r8 && j.undo_label() == lbl,
          (r1 ? std::string("kept!") : r1.error().message) + " | " + (r8 ? std::string("kept!") : r8.error().message));

    // Worked out by the database -- each against the same sum done in C++ over every record.
    auto sch = g.introspect();
    const Table* t = sch ? sch->table("customers") : nullptr;
    if (!t) { check(P + "summaries: the fixture's table", false, ""); return; }
    std::map<std::string, core::Summary> sums = {
        {"n", {"customers", "n", SummaryOp::Count, "email"}},
        {"tot", {"customers", "tot", SummaryOp::Total, "balance"}},
        {"avg", {"customers", "avg", SummaryOp::Average, "balance"}},
        {"low", {"customers", "low", SummaryOp::Min, "name"}},
        {"high", {"customers", "high", SummaryOp::Max, "joined"}},
        {"gone", {"customers", "gone", SummaryOp::Count, "nosuch"}},
    };
    auto all = read_page(g, *t, PageRequest{0, 10000});
    auto got = read_summaries(g, *t, sums);
    std::size_t ie = 0, ib = 0, in = 0, ij = 0;
    for (std::size_t i = 0; i < t->columns.size(); ++i) {
        if (t->columns[i].name == "email") ie = i;
        if (t->columns[i].name == "balance") ib = i;
        if (t->columns[i].name == "name") in = i;
        if (t->columns[i].name == "joined") ij = i;
    }
    double n = 0, tot = 0, nb = 0;
    std::string low, high;
    bool first = true;
    for (const auto& row : all ? all->rows : std::vector<Row>{}) {
        if (!row[ie].is_null()) ++n;
        if (auto v = num(row[ib])) { tot += *v; ++nb; }
        const std::string nm = row[in].as_text() ? *row[in].as_text() : "";
        const std::string jd = row[ij].as_text() ? row[ij].as_text()->substr(0, 10) : "";
        if (first || nm < low) low = nm;
        if (!jd.empty() && jd > high) high = jd;
        first = false;
    }
    const auto near = [](const core::CalcValue& v, double want) {
        const auto* d = std::get_if<double>(&v);
        return d && std::fabs(*d - want) < 1e-6;
    };
    std::string why;
    if (got) why = core::calc_text((*got)["n"]) + " " + core::calc_text((*got)["tot"]) + " " + core::calc_text((*got)["avg"]) +
                   " " + core::calc_text((*got)["low"]) + " " + core::calc_text((*got)["high"]);
    check(P + "summaries: Count / Total / Average / Minimum / Maximum over every record -- as C++ adds them up; "
              "a field gone is left out",
          got && near((*got)["n"], n) && near((*got)["tot"], tot) && nb > 0 && near((*got)["avg"], tot / nb) &&
          core::calc_text((*got)["low"]) == low && core::calc_text((*got)["high"]).substr(0, 10) == high &&
          !got->contains("gone"), got ? why + " want " + std::to_string(n) + " " + std::to_string(tot) + " " + low + " " + high
                                      : got.error().message);
    // Over a found set: the same WHERE as Find.
    const std::vector<Criterion> crit = {{"name", low}};
    auto found = read_found(g, *t, crit, PageRequest{0, 10000});
    auto fs = read_summaries(g, *t, sums, crit);
    double fn = 0;
    for (const auto& row : found ? found->rows : std::vector<Row>{}) if (!row[ie].is_null()) ++fn;
    const std::vector<Criterion> nothing = {{"name", "=no such name at all"}};
    auto empty = read_summaries(g, *t, sums, nothing);
    check(P + "summaries: over a found set (the Find's WHERE); over none -- Count 0, Total / Minimum empty",
          found && fs && near((*fs)["n"], fn) && empty && near((*empty)["n"], 0) &&
          std::holds_alternative<std::monostate>((*empty)["tot"]) && std::holds_alternative<std::monostate>((*empty)["low"]),
          fs ? core::calc_text((*fs)["n"]) + " vs " + std::to_string(fn) : fs.error().message);
    check(P + "summaries: the database untouched throughout", g.strays() == 0, std::to_string(g.strays()));
}

// ── s014c: value lists ───────────────────────────────────────────────────────
void test_value_list_pure(const fs::path& dir) {
    using core::ValueList;
    const auto vals = core::parse_list_values("  home \n\nwork\r\nmobile\nhome\n  \nother");
    check("value list: typed lines -- trimmed, blanks and repeats left out, in order; back as lines",
          vals == std::vector<std::string>{"home", "work", "mobile", "other"} &&
          core::list_values_text(vals) == "home\nwork\nmobile\nother" && core::parse_list_values("").empty(),
          core::list_values_text(vals));
    check("value list: names -- any words; refused: empty, a space at an end, two lines, over 64 letters",
          core::check_value_list_name("Phone kinds").empty() && core::check_value_list_name("état").empty() &&
          !core::check_value_list_name("").empty() && !core::check_value_list_name(" x").empty() &&
          !core::check_value_list_name("a\nb").empty() && !core::check_value_list_name(std::string(65, 'x')).empty() &&
          core::check_value_list_name(std::string(64, 'x')).empty(), "");
    const std::vector<std::string> cf = {"id", "last_name", "city"};
    const ValueList typed{"phones", "kinds", {"home", "work"}};
    const ValueList cities{"contacts", "cities", {}, "contacts", "city"};
    const ValueList who{"phones", "who", {}, "contacts", "id", "last_name"};
    const auto a = core::check_value_list(typed, {"other"}, nullptr);
    const auto b = core::check_value_list(who, {}, &cf);
    const auto c = core::check_value_list(typed, {"kinds"}, nullptr);
    const auto d = core::check_value_list(ValueList{"phones", "none", {}}, {}, nullptr);
    const auto e = core::check_value_list(who, {}, nullptr);
    const auto f = core::check_value_list(ValueList{"phones", "x", {}, "contacts", "nope"}, {}, &cf);
    const auto g = core::check_value_list(ValueList{"phones", "x", {}, "contacts", "id", "id"}, {}, &cf);
    const auto h = core::check_value_list(ValueList{"phones", "x", {}, "contacts", "id", "nope"}, {}, &cf);
    check("value list: good ones pass; refused in words -- a name taken, no values, no table, no field, showing itself, "
          "no field to show",
          a.empty() && b.empty() && core::check_value_list(cities, {}, &cf).empty() &&
          c.find("already has a value list") != std::string::npos && d == "Type at least one value" &&
          e.find("no table") != std::string::npos && f.find("no field") != std::string::npos &&
          g.find("already") != std::string::npos && h.find("no field") != std::string::npos, c + " | " + g);
    check("value list: how each says what it offers",
          core::value_list_says(typed) == "home · work" &&
          core::value_list_says(ValueList{"t", "n", {"a", "b", "c", "d", "e", "f"}}) == "a · b · c · d  + 2 more" &&
          core::value_list_says(cities) == "every “city” of “contacts”" &&
          core::value_list_says(who) == "“id” of “contacts”, showing “last_name”" &&
          core::typed_items(typed) == std::vector<core::ListItem>{{"home", "home"}, {"work", "work"}},
          core::value_list_says(who));
    std::string ti, na;
    check("value list: ids round-trip; a value list is the solution's",
          Store::parse_list_id(Store::list_id("a/b", "my list"), ti, na) && ti == "a/b" && na == "my list" &&
          !Store::parse_list_id(Store::summary_id("a", "s"), ti, na) && Store::is_local(Store::list_id("x", "y")), "");
    core::Solution sol;
    sol.points_at.engine = "sqlite";
    sol.points_at.path = (dir / "vl.sqlite").string();
    sol.lists["phones"]["kinds"] = typed;
    sol.lists["phones"]["who"] = who;
    sol.lists["contacts"]["cities"] = cities;
    auto l = core::default_layout("phones", {"id", "kind", "contact_id"});
    for (auto& o : l.objects) if (o.kind == core::ObjectKind::Field && o.field == "kind") o.list = "kinds";
    sol.layouts["phones"][l.name] = l;
    const auto file = (dir / "vl.acorn").string();
    auto back = core::save_solution(file, sol) ? core::load_solution(file) : std::nullopt;
    std::ifstream in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check("value list: round-trip in the .acorn -- each kind, and a layout field's list",
          back && *back == sol && text.find("\"value_lists\"") != std::string::npos && text.find("\"list\": \"kinds\"") != std::string::npos,
          "");
    {
        std::ofstream o(dir / "vl_half.acorn");
        o << R"({"acorn": 1, "value_lists": {"t": {"good": {"values": ["a", "b"]}, "empty": {"values": []},)"
             R"( "both": {"values": ["a"], "from": "x", "field": "y"}, "nofrom": {"field": "y"},)"
             R"( "src": {"from": "x", "field": "y"}}}})";
    }
    auto half = core::load_solution((dir / "vl_half.acorn").string());
    check("value list: a broken one is skipped (no values, two kinds at once, no table), the rest reads",
          half && half->list("t", "good") && half->list("t", "good")->values.size() == 2 && half->list("t", "src") &&
          !half->list("t", "empty") && !half->list("t", "both") && !half->list("t", "nofrom"), "");
    core::Solution plain;
    plain.points_at = sol.points_at;
    const auto pf = (dir / "vl_plain.acorn").string();
    std::string ptext;
    if (core::save_solution(pf, plain)) {
        std::ifstream pi(pf);
        ptext.assign((std::istreambuf_iterator<char>(pi)), std::istreambuf_iterator<char>());
    }
    check("value list: a solution without any never writes the key", !ptext.empty() && ptext.find("value_lists") == std::string::npos, "");
}

void test_value_lists(Engine* e, const ConnectParams& ro, const std::string& P) {
    using core::ValueList;
    auto c = e->open(ro);
    if (!c) { check(P + "value lists: open read-only", false, c.error().message); return; }
    Gate g(std::move(*c));
    Journal j(g, e->dialect(), e->caps());
    core::Solution sol;
    j.store().set_solution(&sol, [] { return true; });
    const ValueList kinds{"orders", "kinds", {"home", "work"}};
    auto m = verbs::save_value_list(j, kinds);
    check(P + "value lists: New Value List on a READ-ONLY connection -- a step, kept, nothing written to the database",
          m && sol.list("orders", "kinds") && j.undo_label() == "New Value List “kinds”" && g.strays() == 0,
          m ? j.undo_label() : m.error().message);
    auto more = kinds;
    more.values.push_back("mobile");
    auto m2 = verbs::save_value_list(j, more);
    auto same = verbs::save_value_list(j, more);
    const auto after_same = j.undo_label();
    auto rn = more;
    rn.name = "phone kinds";
    auto m3 = verbs::save_value_list(j, rn, "kinds");
    check(P + "value lists: a change is \"Change Value List\"; the same again no step; a new name in the same step",
          m2 && same && after_same == "Change Value List “kinds”" && m3 && !sol.list("orders", "kinds") &&
          sol.list("orders", "phone kinds") && j.undo_label() == "Change Value List “kinds”",
          m3 ? j.undo_label() : m3.error().message);
    auto u = j.undo();
    auto del = verbs::delete_value_list(j, "orders", "kinds");
    auto ud = del ? j.undo() : std::expected<std::string, Error>(std::unexpected(del.error()));
    check(P + "value lists: Ctrl+Z puts the old name back; Delete Value List, Ctrl+Z brings it back exactly",
          u && del && ud && sol.list("orders", "kinds") && *sol.list("orders", "kinds") == more,
          ud ? "" : ud.error().message);
    const auto lbl = j.undo_label();
    auto r1 = verbs::save_value_list(j, ValueList{"orders", "x", {}});
    auto r2 = verbs::save_value_list(j, ValueList{"orders", "kinds", {"a"}}, "nosuch");
    auto r3 = verbs::save_value_list(j, ValueList{"nosuch", "x", {"a"}});
    auto r4 = verbs::save_value_list(j, ValueList{"orders", "x", {}, "nosuch", "id"});
    auto r5 = verbs::save_value_list(j, ValueList{"orders", "x", {}, "customers", "nosuch"});
    auto r6 = verbs::save_value_list(j, ValueList{"orders", "x", {}, "customers", "photo"});
    auto r7 = verbs::save_value_list(j, ValueList{"orders", "x", {}, "customers", "id", "photo"});
    auto r8 = verbs::delete_value_list(j, "orders", "nosuch");
    auto two = verbs::save_value_list(j, ValueList{"orders", "two", {"z"}});
    auto r9 = two ? verbs::save_value_list(j, ValueList{"orders", "kinds", {"z"}}, "two")
                  : std::expected<void, Error>(std::unexpected(two.error()));
    check(P + "value lists: refused in words, no step -- no values, renaming one that isn't there, no table, no table to read, "
              "no field, a binary field read or shown, deleting one that isn't there, a name another has",
          !r1 && !r2 && !r3 && !r4 && r4.error().message.find("no table") != std::string::npos && !r5 && !r6 &&
          r6.error().message.find("binary") != std::string::npos && !r7 && !r8 && two && !r9 &&
          r9.error().message.find("already has a value list") != std::string::npos,
          (r6 ? std::string("kept!") : r6.error().message) + " | " + (r9 ? std::string("kept!") : r9.error().message));
    (void)lbl;
    // s014d: a layout field offers a list by name -- one Change Layout step; the
    // list deleted leaves the layout as it was (its box plain until it's back).
    auto lay = core::default_layout("orders", {"id", "customer_id"});
    for (auto& o : lay.objects) if (o.kind == core::ObjectKind::Field && o.field == "customer_id") o.list = "kinds";
    auto ls = verbs::save_layout(j, lay);
    auto ld = ls ? verbs::delete_value_list(j, "orders", "kinds") : std::expected<void, Error>(std::unexpected(ls.error()));
    const core::Layout* kept = sol.layout("orders", lay.name);
    bool still = false;
    if (kept) for (const auto& o : kept->objects) if (o.field == "customer_id" && o.kind == core::ObjectKind::Field) still = o.list == "kinds";
    auto lu = ld ? j.undo() : std::expected<std::string, Error>(std::unexpected(ld.error()));
    check(P + "value lists: a layout field offers one by name (a layout step); deleting the list leaves the layout as it was",
          ls && ld && still && lu && sol.list("orders", "kinds") && g.strays() == 0, ls ? (ld ? "" : ld.error().message) : ls.error().message);

    // What each offers -- against the same set worked out in C++ over every record.
    auto sch = g.introspect();
    const Table* t = sch ? sch->table("customers") : nullptr;
    if (!t) { check(P + "value lists: the fixture's table", false, ""); return; }
    auto all = read_page(g, *t, PageRequest{0, 10000});
    std::size_t ii = 0, in = 0;
    for (std::size_t i = 0; i < t->columns.size(); ++i) {
        if (t->columns[i].name == "id") ii = i;
        if (t->columns[i].name == "name") in = i;
    }
    std::set<std::string> names;
    std::map<std::string, std::string> name_of;   // id -> name
    for (const auto& row : all ? all->rows : std::vector<Row>{}) {
        const std::string nm = cell_text(row[in], t->columns[in].type);
        if (!nm.empty()) names.insert(nm);
        name_of[cell_text(row[ii], t->columns[ii].type)] = nm;
    }
    auto ty = read_value_list(g, *sch, more);
    check(P + "value lists: a typed list offers its values, in order",
          ty && ty->items == std::vector<core::ListItem>{{"home", "home"}, {"work", "work"}, {"mobile", "mobile"}} && !ty->more, "");
    auto byname = read_value_list(g, *sch, ValueList{"orders", "n", {}, "customers", "name"});
    std::set<std::string> got;
    bool each_once = byname.has_value(), self_shown = true;
    for (const auto& it : byname ? byname->items : std::vector<core::ListItem>{}) {
        each_once = each_once && got.insert(it.value).second;
        self_shown = self_shown && it.shown == it.value;
    }
    check(P + "value lists: from a field -- every distinct value once, as Browse shows it (one SELECT DISTINCT)",
          byname && !names.empty() && got == names && each_once && self_shown && !byname->more,
          byname ? std::to_string(got.size()) + " vs " + std::to_string(names.size()) : byname.error().message);
    auto who = read_value_list(g, *sch, ValueList{"orders", "who", {}, "customers", "id", "name"});
    bool pairs = who.has_value() && who->items.size() == name_of.size();
    bool ordered = true;
    for (std::size_t i = 0; who && i < who->items.size(); ++i) {
        const auto& it = who->items[i];
        pairs = pairs && name_of.contains(it.value) && name_of[it.value] == it.shown;
        if (i && who->items[i - 1].shown > it.shown && e->dialect() != Dialect::MariaDB) ordered = false;   // MariaDB's order ignores case
    }
    check(P + "value lists: a key showing another field -- each id with its own name, in the order of the names",
          pairs && ordered, who ? std::to_string(who->items.size()) + " vs " + std::to_string(name_of.size()) : who.error().message);
    auto gone = read_value_list(g, *sch, ValueList{"orders", "g", {}, "customers", "nosuch"});
    check(P + "value lists: a field that's gone -- refused in words, no SELECT run",
          !gone && gone.error().message.find("no field") != std::string::npos, "");
    check(P + "value lists: the database untouched throughout", g.strays() == 0, std::to_string(g.strays()));
}

// ── s015a: preview / print ───────────────────────────────────────────────────
void test_print_pure() {
    using core::PrintPage;
    const auto a = core::paginate(48, 336, 40, 12, 750);   // the Contact Card on A4: one a page
    const auto b = core::paginate(56, 32, 40, 12, 640);    // a list: all on one page
    const auto c = core::paginate(56, 32, 40, 40, 640);    // 17 a page -> 17, 17, 6
    const auto d = core::paginate(0, 0, 0, 0, 640);        // no records: one page, header / footer only
    const auto e = core::paginate(0, 900, 0, 2, 640);      // a body taller than the page: still one each
    const auto f = core::paginate(10, 0, 10, 5, 640);      // no body: one page
    check("print: pages -- one record a page, all on one, 17 + 17 + 6, none (one page), too tall (one each), no body",
          a.size() == 12 && a[11] == PrintPage{11, 1} && b == std::vector<PrintPage>{{0, 12}} &&
          c == std::vector<PrintPage>{{0, 17}, {17, 17}, {34, 6}} && d == std::vector<PrintPage>{{0, 0}} &&
          e == std::vector<PrintPage>{{0, 1}, {1, 1}} && f == std::vector<PrintPage>{{0, 5}}, std::to_string(c.size()));
    const auto L = core::paper_letter(), A = core::paper_a4();
    check("print: paper -- Letter 612 x 792, A4 595 x 842; a layout fits whole (1) or is scaled to the printable width",
          L.width == 612 && L.height == 792 && std::lround(A.height) == 842 && core::print_scale(504, L) == 1.0 &&
          std::fabs(core::print_scale(1080, L) - 540.0 / 1080) < 1e-9 && core::page_words(2, 5) == "Page 2 of 5", "");
}

void test_print(Engine* e, const ConnectParams& ro, const std::string& P) {
    auto c = e->open(ro);
    if (!c) { check(P + "print: open read-only", false, c.error().message); return; }
    Gate g(std::move(*c));
    auto sch = g.introspect();
    const Table* t = sch ? sch->table("customers") : nullptr;
    if (!t) { check(P + "print: the fixture's table", false, ""); return; }
    std::vector<std::string> fields;
    for (const auto& col : t->columns) fields.push_back(col.name);
    core::Layout l = core::default_layout("customers", fields);
    core::LayoutObject calc;
    calc.kind = core::ObjectKind::Field;
    calc.field = "loud";
    l.objects.push_back(calc);
    core::LayoutObject sum = calc;
    sum.field = "how_many";
    l.objects.push_back(sum);
    core::LayoutObject gone = calc;
    gone.field = "nosuch";
    l.objects.push_back(gone);
    const std::map<std::string, core::Calc> calcs = {{"loud", {"customers", "loud", "Upper(name)"}}};
    const std::map<std::string, core::CalcValue> sums = {{"how_many", core::CalcValue{7.0}}};
    auto all = read_page(g, *t, PageRequest{0, 10000});
    bool same = all.has_value() && !all->rows.empty();
    std::string why;
    for (const auto& row : all ? all->rows : std::vector<Row>{}) {
        const auto pr = print_record(g, *sch, *t, l, row, calcs, sums, "2026-11-07");
        for (std::size_t i = 0; i < l.objects.size() && same; ++i) {
            const auto& ob = l.objects[i];
            if (ob.kind == core::ObjectKind::Text && pr[i].text != ob.text) { same = false; why = "words " + ob.text; }
            if (ob.kind == core::ObjectKind::Field) {
                const Column* col = t->column(ob.field);
                std::size_t k = 0;
                for (std::size_t q = 0; q < t->columns.size(); ++q) if (t->columns[q].name == ob.field) k = q;
                const std::string want = col ? (col->type == FieldType::Container ? (row[k].is_null() ? "" : "(binary)")
                                                                                   : cell_text(row[k], col->type))
                                       : ob.field == "loud" ? core::calc_text(row_calcs(*t, row, calcs, "2026-11-07")["loud"])
                                       : ob.field == "how_many" ? "7" : "missing field “nosuch”";
                if (pr[i].text != want || pr[i].missing != (ob.field == "nosuch")) { same = false; why = ob.field + ": " + pr[i].text + " vs " + want; }
            }
        }
    }
    check(P + "print: every customer's layout resolved -- fields as Browse shows them, words, a calculation, a summary, "
              "a field that's gone (dashed)", same, why);
    check(P + "print: the database untouched", g.strays() == 0, std::to_string(g.strays()));
}

// s015a: portals and related fields print through their links -- on the
// Address Book (contacts <- phones), which has records that point.
void test_print_book(Gate& g, const std::string& P) {
    auto sch = g.introspect();
    const Table* ct = sch ? sch->table("contacts") : nullptr;
    const Table* pt = sch ? sch->table("phones") : nullptr;
    if (!ct || !pt) { check(P + "print: the Address Book's tables", false, ""); return; }
    auto col = [](const Table& t, const std::string& n) {
        for (std::size_t i = 0; i < t.columns.size(); ++i) if (t.columns[i].name == n) return i;
        return std::size_t{0};
    };
    core::Layout cl = core::default_layout("contacts", {"last_name"});
    core::PortalLink link{"phones", "contact_id"};
    core::add_portal(cl, link, {"kind", "number"});
    cl.objects.back().h = core::kPortalRowH * 2;   // the names and ONE record
    auto contacts = read_page(g, *ct, PageRequest{0, 10000});
    auto phones = read_page(g, *pt, PageRequest{0, 10000});
    std::map<std::string, std::vector<std::string>> numbers;   // contact id -> its numbers, in key order
    for (const auto& r : phones ? phones->rows : std::vector<Row>{})
        numbers[cell_text(r[col(*pt, "contact_id")], FieldType::Integer)].push_back(cell_text(r[col(*pt, "number")], FieldType::Text));
    bool portals = contacts.has_value() && !contacts->rows.empty() && !numbers.empty();
    std::string why;
    for (const auto& r : contacts ? contacts->rows : std::vector<Row>{}) {
        const auto pr = print_record(g, *sch, *ct, cl, r, {}, {}, "2026-11-07");
        const auto& rows = pr.back().rows;
        const auto& want = numbers[cell_text(r[col(*ct, "id")], FieldType::Integer)];
        if (rows.size() != std::min<std::size_t>(1, want.size()) || (!rows.empty() && rows[0][1] != want[0])) {
            portals = false;
            why = std::to_string(rows.size()) + " rows";
        }
    }
    check(P + "print: a portal prints the records that point at the record, as many as its box holds", portals, why);
    core::Layout pl = core::default_layout("phones", {"number"});
    core::add_related(pl, core::RelatedLink{"contact_id", "contacts"}, "last_name");
    std::map<std::string, std::string> name_of;
    for (const auto& r : contacts ? contacts->rows : std::vector<Row>{})
        name_of[cell_text(r[col(*ct, "id")], FieldType::Integer)] = cell_text(r[col(*ct, "last_name")], FieldType::Text);
    bool related = phones.has_value() && !phones->rows.empty();
    for (const auto& r : phones ? phones->rows : std::vector<Row>{}) {
        const auto pr = print_record(g, *sch, *pt, pl, r, {}, {}, "2026-11-07");
        if (pr.back().text != name_of[cell_text(r[col(*pt, "contact_id")], FieldType::Integer)]) { related = false; why = pr.back().text; }
    }
    check(P + "print: a related field prints the last name of the contact its link points at", related, why);
}

// ── s015b: sub-summary parts ─────────────────────────────────────────────────
void test_report_pure(const fs::path& dir) {
    using core::Part;
    // A list with a header and a footer, then a sub by city each way.
    core::Layout l = core::default_layout("addresses", {"city"});
    l.header = 40;
    for (auto& o : l.objects) o.y += 40;
    l.height += 40;
    core::move_part_line(l, Part::Footer, l.height + 40);   // a footer under the body
    const int h0 = l.height, body0 = core::part_height(l, Part::Body);
    const int lead = core::add_sub(l, "city", true);
    const int trail = core::add_sub(l, "city", false);
    const auto bs = core::bands(l);
    check("report: add a leading and a trailing sub -- header, leading, body, trailing, footer, each 40 tall, the body as it was",
          lead == 0 && trail == 1 && bs.size() == 5 && bs[1].part == Part::Sub && bs[1].sub == 0 && bs[2].part == Part::Body &&
          bs[3].part == Part::Sub && bs[3].sub == 1 && bs[1].height == core::kSubH && bs[3].height == core::kSubH &&
          l.height == h0 + 2 * core::kSubH && core::part_height(l, Part::Body) == body0 && core::check_layout(l).empty(),
          std::to_string(bs.size()) + " bands, " + core::check_layout(l));
    // Each got its field's box, in its band.
    const auto& lb = l.objects[l.objects.size() - 2];
    const auto& tb = l.objects.back();
    check("report: each sub holds its field's box (part_of Sub, sub_of its index); the body's objects stay the body's",
          core::part_of(l, lb) == Part::Sub && core::sub_of(l, lb) == 0 && core::sub_of(l, tb) == 1 &&
          core::part_of(l, l.objects[0]) == Part::Body && core::part_objects(l, Part::Body).size() == 2 &&
          core::band_objects(l, bs[1]) == std::vector<int>{static_cast<int>(l.objects.size()) - 2} &&
          core::body_top(l) == 80 && core::sub_top(l, 1) == core::body_bottom(l), "");
    // Add Field goes in the body, not into a sub; the trailing sub and the footer make room.
    auto added = l;
    core::add_object(added, core::ObjectKind::Field, "street");
    check("report: Add Field lands in the body -- the trailing sub and the footer move down for it",
          core::part_of(added, added.objects.back()) == Part::Body && core::sub_of(added, added.objects[added.objects.size() - 3]) == 1 &&
          added.subs == l.subs && core::check_layout(added).empty(), "");
    // Size a sub by its line: everything under it moves; it never cuts its box.
    auto sized = l;
    const bool grew = core::move_band_line(sized, 1, bs[1].top + bs[1].height + 24);
    auto shrunk = l;
    const bool cut = core::move_band_line(shrunk, 1, bs[1].top + 8);
    check("report: a sub's line -- dragged down it grows (what's under moves); up, it stops at its box",
          grew && sized.subs[0].height == 64 && sized.height == l.height + 24 && sized.objects[0].y == l.objects[0].y + 24 &&
          cut && shrunk.subs[0].height == 32 && shrunk.height == l.height - 8, std::to_string(shrunk.subs[0].height));
    auto foot = l;   // the footer line over a trailing sub sizes that sub
    core::move_part_line(foot, Part::Footer, core::footer_top(l) + 16);
    check("report: the footer line over a trailing sub sizes the sub, the footer moving with it",
          foot.subs[1].height == 56 && foot.footer == l.footer && foot.height == l.height + 16, std::to_string(foot.subs[1].height));
    // Remove: its objects go, what's under moves up.
    auto gone = l;
    const bool rm = core::remove_sub(gone, 0);
    check("report: remove a sub -- its box goes, everything under it moves up 40",
          rm && gone.subs.size() == 1 && !gone.subs[0].leading && gone.objects.size() == l.objects.size() - 1 &&
          gone.objects[0].y == l.objects[0].y - 40 && gone.height == l.height - 40 && !core::remove_sub(gone, 5), "");
    // Groups: leading top-down, then trailing bottom-up, each once.
    core::Layout g2;
    g2.subs = {{"region", true, 40}, {"city", true, 40}, {"city", false, 40}, {"region", false, 40}};
    check("report: groups -- outermost first: region, city; each sub's level",
          core::group_fields(g2) == std::vector<std::string>{"region", "city"} && core::group_level(g2, 0) == 0 &&
          core::group_level(g2, 1) == 1 && core::group_level(g2, 2) == 1 && core::group_level(g2, 3) == 0 &&
          core::group_level(g2, 9) == -1 && core::sub_label(g2.subs[2]) == "trailing sub-summary by “city”", "");
    // Blocks: TN/Harrowgate x2, TN/Millbrook, OR/Port Ellis.
    g2.table = "addresses"; g2.name = "r"; g2.width = 100; g2.height = 400;
    const std::vector<std::vector<std::string>> keys = {{"TN", "Harrowgate"}, {"TN", "Harrowgate"}, {"TN", "Millbrook"}, {"OR", "Port Ellis"}};
    using B = core::ReportBlock;
    const std::vector<B> want = {
        {Part::Sub, 0, 0}, {Part::Sub, 1, 0}, {Part::Body, -1, 0}, {Part::Body, -1, 1}, {Part::Sub, 2, 1},
        {Part::Sub, 1, 2}, {Part::Body, -1, 2}, {Part::Sub, 2, 2}, {Part::Sub, 3, 2},
        {Part::Sub, 0, 3}, {Part::Sub, 1, 3}, {Part::Body, -1, 3}, {Part::Sub, 2, 3}, {Part::Sub, 3, 3}};
    const auto got = core::report_blocks(g2, keys);
    check("report: blocks -- a state's heading, a town's, the town's records, the town's foot, the state's foot (two levels)",
          got == want, std::to_string(got.size()) + " blocks");
    core::Layout plain = core::default_layout("t", {"a"});
    check("report: no subs -- one body per record; no records -- nothing",
          core::report_blocks(plain, {{}, {}, {}}) == std::vector<B>{{Part::Body, -1, 0}, {Part::Body, -1, 1}, {Part::Body, -1, 2}} &&
          core::report_blocks(g2, {}).empty(), "");
    // Pages over blocks: a heading at a page's foot goes over with its records.
    using core::PrintPage;
    const auto p1 = core::paginate_blocks(40, 40, {40, 32, 32, 40, 32}, {true, false, false, true, false}, 230);
    const auto p2 = core::paginate_blocks(0, 0, {}, {}, 600);
    const auto p3 = core::paginate_blocks(0, 0, {40, 900}, {true, false}, 600);   // a heading over a too-tall body: kept together
    check("report: pages over blocks -- a heading never ends a page; no blocks, one page; too tall, still placed",
          p1 == std::vector<PrintPage>{{0, 3}, {3, 2}} && p2 == std::vector<PrintPage>{{0, 0}} &&
          p3 == std::vector<PrintPage>{{0, 1}, {1, 1}}, std::to_string(p1.size()));
    // Kept in the .acorn and read back; a trailing one says so; an older file reads none.
    core::Solution sol;
    sol.layouts["addresses"]["by town"] = l;
    sol.layouts["addresses"]["by town"].name = "by town";
    const auto file = (dir / "report.acorn").string();
    const bool saved = core::save_solution(file, sol);
    auto back = core::load_solution(file);
    const std::string text = slurp(file);
    check("report: subs kept in the .acorn and read back the same (\"when\": \"trailing\")",
          saved && back && *back == sol && text.find("\"subs\"") != std::string::npos && text.find("\"trailing\"") != std::string::npos, "");
    core::Layout bad = l;
    bad.subs[0].by.clear();
    core::Layout tall = l;
    tall.subs[0].height = 2000;
    check("report: check_layout -- a sub needs its field, and the parts leave the body room",
          !core::check_layout(bad).empty() && !core::check_layout(tall).empty(), "");
}

// ── s016: labels ─────────────────────────────────────────────────────────────
// ── s017a: scripts ─────────────────────────────────────────────────────────
void test_script_pure(const fs::path& dir) {
    using core::Script; using core::ScriptStep; using K = core::StepKind;
    bool names = core::step_kinds().size() == 10;
    for (auto k : core::step_kinds()) names = names && core::parse_step_kind(core::step_name(k)) == k;
    check("scripts: ten kinds of step, each name read back; an unknown one isn't one; only three write",
          names && !core::parse_step_kind("loop") && core::step_writes(K::SetField) && core::step_writes(K::NewRecord) &&
          core::step_writes(K::DeleteRecord) && !core::step_writes(K::Find) && !core::step_writes(K::Print), "");
    auto a = core::parse_assignments("first_name = \"Ada\"\n\n  last_name=Upper(\"x\")  \n");
    auto b = core::parse_assignments("first_name \"Ada\"");
    auto c = core::parse_assignments("x = 1\n= 2");
    auto d = core::parse_assignments("x =  ");
    const auto* av = std::get_if<0>(&a);
    check("scripts: New Record's lines -- “field = what”, blank lines skipped; refused in words with the line",
          av && av->size() == 2 && (*av)[0] == std::pair<std::string, std::string>{"first_name", "\"Ada\""} &&
          (*av)[1].second == "Upper(\"x\")" && std::get_if<1>(&b) && std::get<1>(b).find("line 1") != std::string::npos &&
          std::get_if<1>(&c) && std::get<1>(c).find("line 2") != std::string::npos && std::get_if<1>(&d) &&
          core::parse_assignments("").index() == 0, std::get_if<1>(&c) ? std::get<1>(c) : "");
    check("scripts: how each step says itself",
          core::step_says({K::Find, "city", "Mill"}) == "Find “city” has “Mill”" &&
          core::step_says({K::Find, "company", "="}) == "Find “company” empty" &&
          core::step_says({K::Sort, "birthday", "", true}) == "Sort by “birthday”, descending" &&
          core::step_says({K::SetField, "last_name", "Upper(last_name)"}) == "Set Field “last_name” to Upper(last_name)" &&
          core::step_says({K::NewRecord, "", "a = 1\nb = 2"}) == "New Record with “a”, “b”" &&
          core::step_says({K::GoToLayout, "Contact Card"}) == "Go to Layout “Contact Card”" &&
          core::script_says(Script{"t", "n", {{K::Find, "a", "b"}, {K::Preview}}}) == "2 steps: Find · Preview",
          core::step_says({K::NewRecord, "", "a = 1\nb = 2"}));
    const std::vector<std::string> f = {"id", "first_name", "last_name"}, L = {"Contact Card"};
    auto chk = [&](std::vector<ScriptStep> st, std::vector<std::string> taken = {}, std::string name = "s") {
        return core::check_script(Script{"contacts", name, std::move(st)}, taken, f, L);
    };
    const auto ok = chk({{K::GoToLayout, "Contact Card"}, {K::Find, "last_name", "a"}, {K::Sort, "id", "", true},
                         {K::NewRecord, "", "first_name = \"Ada\""}, {K::SetField, "last_name", "Upper(first_name)"},
                         {K::DeleteRecord}, {K::Preview}, {K::Print}, {K::Message, "", "hi"}, {K::ShowAll}});
    const auto r1 = chk({});
    const auto r2 = chk({{K::Find, "nope", "a"}});
    const auto r3 = chk({{K::GoToLayout, "Nope"}});
    const auto r4 = chk({{K::Find, "id", "  "}});
    const auto r5 = chk({{K::ShowAll}, {K::SetField, "last_name", "Upper("}});
    const auto r6 = chk({{K::NewRecord, "", "nope = 1"}});
    const auto r7 = chk({{K::Message, "", ""}});
    const auto r8 = chk({{K::ShowAll}}, {"s"});
    const auto r9 = chk({{K::ShowAll}}, {}, " s");
    check("scripts: a good script passes; refused in words, naming the step -- none, no field, no layout, nothing to "
          "find, an expression that doesn't parse, New Record's field, no words, a name taken / bad",
          ok.empty() && r1 == "A script needs a step" && r2.find("Step 1 (Find)") == 0 && r2.find("no field") != std::string::npos &&
          r3.find("no layout") != std::string::npos && r4.find("something to find") != std::string::npos &&
          r5.find("Step 2 (Set Field)") == 0 && r6.find("no field “nope”") != std::string::npos &&
          r7.find("words") != std::string::npos && r8.find("already has a script") != std::string::npos && !r9.empty(),
          ok + " | " + r5);
    std::string ti, na;
    check("scripts: ids round-trip; a script is the solution's",
          Store::parse_script_id(Store::script_id("a/b", "tidy up"), ti, na) && ti == "a/b" && na == "tidy up" &&
          !Store::parse_script_id(Store::list_id("a", "s"), ti, na) && Store::is_local(Store::script_id("x", "y")) &&
          script_label(Script{"t", "tidy", {}}) == "Script “tidy”", "");
    core::Solution sol;
    sol.points_at.engine = "sqlite";
    sol.points_at.path = (dir / "sc.sqlite").string();
    sol.scripts["contacts"]["tidy"] = Script{"contacts", "tidy", {{K::Find, "last_name", "=x"}, {K::Sort, "id", "", true},
                                                               {K::NewRecord, "", "a = 1\nb = \"x\""}, {K::Message, "", "two\nlines"}}};
    sol.scripts["phones"]["p"] = Script{"phones", "p", {{K::ShowAll}}};
    const auto file = (dir / "sc.acorn").string();
    auto back = core::save_solution(file, sol) ? core::load_solution(file) : std::nullopt;
    std::ifstream in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check("scripts: round-trip in the .acorn, every key only when it says something",
          back && *back == sol && text.find("\"scripts\"") != std::string::npos && text.find("\"descending\": true") != std::string::npos &&
          text.find("\"descending\": false") == std::string::npos, "");
    {
        std::ofstream o(dir / "sc_half.acorn");
        o << R"({"acorn": 1, "scripts": {"t": {"good": [{"step": "show_all"}, {"step": "sort", "field": "x"}],)"
             R"( "none": [], "odd": [{"step": "show_all"}, {"step": "loop"}], "notalist": {"step": "show_all"}}}})";
    }
    auto half = core::load_solution((dir / "sc_half.acorn").string());
    check("scripts: a broken one is skipped (no steps, a step this Acorn doesn't know), the rest reads",
          half && half->script("t", "good") && half->script("t", "good")->steps.size() == 2 && !half->script("t", "none") &&
          !half->script("t", "odd") && !half->script("t", "notalist"), "");
    core::Solution plain;
    plain.points_at = sol.points_at;
    const auto pf = (dir / "sc_plain.acorn").string();
    std::string ptext;
    if (core::save_solution(pf, plain)) {
        std::ifstream pi(pf);
        ptext.assign((std::istreambuf_iterator<char>(pi)), std::istreambuf_iterator<char>());
    }
    check("scripts: a solution without any never writes the key", !ptext.empty() && ptext.find("scripts") == std::string::npos, "");
}

// ── s017b: buttons ─────────────────────────────────────────────────────────
void test_button_pure(const fs::path& dir) {
    using core::ObjectKind;
    core::Layout l = core::default_layout("contacts", {"id", "last_name"});
    const int y = core::next_row_y(l);
    core::add_button(l, "Tidy", "Tidy");
    const auto& b = l.objects.back();
    check("buttons: Add Button lands under everything, button-sized, on the grid, with its words and script",
          b.kind == ObjectKind::Button && b.text == "Tidy" && b.script == "Tidy" && b.y == y && b.x == core::kFormMargin &&
          b.w == core::kButtonW && b.h == core::kButtonH && b.y % core::kLayoutGrid == 0 && l.height >= b.y + b.h &&
          core::check_layout(l).empty(), core::check_layout(l));
    core::LayoutObject none{ObjectKind::Button, "", "Go"};
    check("buttons: how one is named and says itself; a missing script (not “none”)",
          core::object_label(b) == "the button “Tidy”" && core::button_says(b) == "runs “Tidy”" &&
          core::button_says(none) == "runs no script" && core::missing_script(b, {"Other"}) && !core::missing_script(b, {"Tidy"}) &&
          !core::missing_script(none, {}) && !core::missing_script(l.objects.front(), {}), "");
    core::Layout r = l;
    core::add_button(r, "Again", "Tidy");
    core::add_button(r, "Else", "Other");
    const int n = core::rename_script_on(r, "Tidy", "Tidy Up");
    int up = 0, other = 0;
    for (const auto& o : r.objects) { up += o.script == "Tidy Up"; other += o.script == "Other"; }
    check("buttons: a script renamed -- every button that ran it follows, the rest left be; the same name or none: nothing",
          n == 2 && up == 2 && other == 1 && core::rename_script_on(r, "Tidy Up", "Tidy Up") == 0 &&
          core::rename_script_on(r, "", "x") == 0, std::to_string(n));
    core::Solution sol;
    sol.points_at.engine = "sqlite";
    sol.points_at.path = (dir / "bt.sqlite").string();
    core::add_button(l, "Nothing yet", "");
    sol.layouts["contacts"][l.name] = l;
    const auto file = (dir / "bt.acorn").string();
    auto back = core::save_solution(file, sol) ? core::load_solution(file) : std::nullopt;
    std::ifstream in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t scripts = 0;
    for (std::size_t at = text.find("\"script\""); at != std::string::npos; at = text.find("\"script\"", at + 1)) ++scripts;
    check("buttons: round-trip in the .acorn (\"kind\": \"button\"), the script's key only when it runs one",
          back && *back == sol && text.find("\"button\"") != std::string::npos && scripts == 1, text.substr(0, 0));
}

// ── s018a: export ──────────────────────────────────────────────────────────
void test_export_pure() {
    check("export: a value quoted only when it must be -- the separator, a quote (doubled), a line break, a space at an end",
          export_cell("plain", ',') == "plain" && export_cell("a,b", ',') == "\"a,b\"" && export_cell("a,b", '\t') == "a,b" &&
          export_cell("say \"hi\"", ',') == "\"say \"\"hi\"\"\"" && export_cell("two\nlines", ',') == "\"two\nlines\"" &&
          export_cell(" pad", ',') == "\" pad\"" && export_cell("a\tb", '\t') == "\"a\tb\"" && export_cell("", ',').empty() &&
          export_line({"a", "", "c,d"}, ',') == "a,,\"c,d\"", export_cell("say \"hi\"", ','));
    Table t;
    t.name = "contacts";
    auto col = [](const char* n, FieldType ty) { Column c; c.name = n; c.type = ty; return c; };
    t.columns = {col("id", FieldType::Integer), col("name", FieldType::Text), col("photo", FieldType::Container), col("city", FieldType::Text)};
    std::map<std::string, core::Calc> calcs{{"shout", core::Calc{"contacts", "shout", "Upper(name)"}}};
    core::Layout l;
    l.table = "contacts"; l.name = "Card"; l.width = 600; l.height = 200;
    using K = core::ObjectKind;
    l.objects = {{K::Text, "", "City", 16, 48, 100, 24}, {K::Field, "city", "", 120, 48, 100, 24},
                 {K::Field, "name", "", 300, 16, 100, 24}, {K::Field, "id", "", 120, 16, 100, 24},
                 {K::Field, "photo", "", 120, 80, 100, 24}, {K::Field, "gone", "", 120, 112, 100, 24},
                 {K::Field, "shout", "", 300, 48, 100, 24}, {K::Field, "name", "", 120, 140, 100, 24},
                 {K::Button, "", "Go", 16, 160, 100, 24}};
    const auto lf = layout_export_fields(l, t, calcs);
    const auto tf = table_export_fields(t, calcs);
    check("export: a layout's fields in reading order (top, then left), each once -- no binary, no missing, calculations kept",
          lf == std::vector<std::string>{"id", "name", "city", "shout"}, "");
    check("export: every field of the table in order without binary ones, then its calculations",
          tf == std::vector<std::string>{"id", "name", "city", "shout"} && export_ending(ExportFormat::Tab) == std::string(".tsv") &&
          export_says({12, 5}, "c.csv") == "Exported 12 records (5 fields) to c.csv" &&
          export_says({1, 1}, "c.csv") == "Exported 1 record (1 field) to c.csv", export_says({12, 5}, "c.csv"));
}

// On the sample Address Book: every record, a found set in its sort, a
// calculation, a file put in place whole -- and nothing written to the database.
void test_export_book(Gate& g, const std::string& P) {
    auto sch = g.introspect();
    const Table* ct = sch ? sch->table("contacts") : nullptr;
    if (!ct) { check(P + "export: the sample's contacts", false, ""); return; }
    const int strays0 = g.strays();
    auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        std::size_t at = 0;
        for (std::size_t k; (k = text.find("\r\n", at)) != std::string::npos; at = k + 2) out.push_back(text.substr(at, k - at));
        return out;
    };
    ExportRequest all;
    all.fields = {"first_name", "last_name", "birthday"};
    all.chunk = 5;   // three reads for twelve
    std::ostringstream a;
    auto ra = export_records(g, *sch, *ct, all, a);
    const auto la = lines(a.str());
    check(P + "export: every record, a header first, read a few at a time -- 12 records, CRLF lines",
          ra && ra->records == 12 && ra->fields == 3 && la.size() == 13 && la[0] == "first_name,last_name,birthday" &&
          a.str().size() >= 2 && a.str().substr(a.str().size() - 2) == "\r\n", ra ? a.str().substr(0, 80) : ra.error().message);
    ExportRequest f;
    f.fields = {"last_name", "shout"};
    f.calcs["shout"] = core::Calc{"contacts", "shout", "Upper(last_name)"};
    f.found = {{"last_name", "e"}};
    f.sort = "last_name";
    f.descending = true;
    f.format = ExportFormat::Tab;
    f.today = core::today_iso();
    std::ostringstream b;
    auto rb = export_records(g, *sch, *ct, f, b);
    const auto lb = lines(b.str());
    bool sorted = lb.size() >= 3, upper = lb.size() >= 2;
    for (std::size_t i = 2; i < lb.size(); ++i) sorted = sorted && lb[i - 1].substr(0, lb[i - 1].find('\t')) >= lb[i].substr(0, lb[i].find('\t'));
    for (std::size_t i = 1; i < lb.size(); ++i) {
        const auto tab = lb[i].find('\t');
        std::string up = lb[i].substr(0, tab);
        for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        upper = upper && tab != std::string::npos && lb[i].substr(tab + 1) == up;
    }
    check(P + "export: the found set in its sort (descending), tab-separated, a calculation worked out per record",
          rb && rb->records > 0 && rb->records < 12 && static_cast<std::size_t>(rb->records) + 1 == lb.size() &&
          lb[0] == "last_name\tshout" && sorted && upper, rb ? b.str().substr(0, 120) : rb.error().message);
    const auto dir = fs::temp_directory_path() / ("acorn-export-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto path = (dir / "contacts.csv").string();
    { std::ofstream old(path); old << "old"; }
    ExportRequest bad = all;
    bad.fields = {"last_name", "nope"};
    auto rbad = export_file(g, *sch, *ct, bad, path);
    std::string kept;
    { std::ifstream in(path); kept.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
    check(P + "export: a field the table hasn't is refused in words; the file there is left as it was, no .part",
          !rbad && rbad.error().message.find("no field “nope”") != std::string::npos && kept == "old" && !fs::exists(path + ".part"),
          rbad ? "" : rbad.error().message);
    auto rf = export_file(g, *sch, *ct, all, path);
    std::string text;
    { std::ifstream in(path, std::ios::binary); text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
    check(P + "export: the file put in place whole (the same text as the stream), no .part left",
          rf && text == a.str() && !fs::exists(path + ".part"), rf ? "" : rf.error().message);
    // s020b: Stop -- asked after each read; false leaves the file that was there, no .part.
    ExportRequest stop = all;
    stop.chunk = 5;
    std::vector<std::int64_t> asked;
    stop.keep_going = [&asked](std::int64_t n) { asked.push_back(n); return false; };
    auto rs = export_file(g, *sch, *ct, stop, path);
    std::string after;
    { std::ifstream in(path, std::ios::binary); after.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
    check(P + "export: Stop after the first read -- \"stopped\", asked once with 5, the file there untouched, no .part",
          !rs && rs.error().message == "stopped" && asked == std::vector<std::int64_t>{5} && after == text && !fs::exists(path + ".part"),
          rs ? "not stopped" : rs.error().message);
    ExportRequest go = all;
    go.chunk = 5;
    std::vector<std::int64_t> told;
    go.keep_going = [&told](std::int64_t n) { told.push_back(n); return true; };
    std::ostringstream sink;
    auto rg = export_records(g, *sch, *ct, go, sink);
    check(P + "export: keep_going told the running count after each read (5, 10, 12)",
          rg && told == std::vector<std::int64_t>{5, 10, 12}, std::to_string(told.size()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    check(P + "export: reads only -- nothing went near the door", g.strays() == strays0, std::to_string(g.strays()));
}

// ── s018b: import ──────────────────────────────────────────────────────────
void test_import_pure() {
    // What Export writes reads back exactly: the separator, quotes, line breaks, end spaces, empties.
    const std::vector<std::string> tricky{"plain", "a,b", "say \"hi\"", "two\r\nlines", " pad ", "", "a\tb", "\"", "x\ny"};
    bool same = true;
    for (const char sep : {',', '\t'}) {
        const std::string text = export_line({"h1", "h2"}, sep) + "\r\n" + export_line(tricky, sep) + "\r\n" +
                                 export_line({"last", ""}, sep) + "\r\n";
        auto d = read_delimited(text, sep);
        same = same && d && d->rows.size() == 3 && d->rows[1] == tricky && d->rows[2] == std::vector<std::string>{"last", ""} &&
               d->lines == std::vector<std::size_t>{1, 2, 5};
    }
    check("import: what Export writes reads back exactly -- comma and tab; quotes, line breaks, end spaces, empties; "
          "each record's first line counted", same, "");
    auto bom = read_delimited("\xEF\xBB\xBF" "a,b\nc,d\re,f", ',');
    auto open = read_delimited("a,b\n\"never,closed\n", ',');
    auto loose = read_delimited("5\" pipe,x\n\nlast", ',');
    check("import: a byte-order mark skipped; LF, CR and CRLF end a record; a quote never closed refused with its line; "
          "a quote inside a value kept; an empty line is a record of nothing",
          bom && bom->rows.size() == 3 && bom->rows[0][0] == "a" && bom->rows[2] == std::vector<std::string>{"e", "f"} && !open &&
              open.error() == "line 2: a quote is never closed" && loose && loose->rows.size() == 3 &&
              loose->rows[0][0] == "5\" pipe" && loose->rows[1].empty() && loose->lines[2] == 3 && loose->width() == 2,
          open ? "" : open.error());
    check("import: comma or tab -- by the ending, else the first line (outside quotes)",
          guess_separator("a\tb", "x.csv") == ',' && guess_separator("a,b", "x.TSV") == '\t' &&
              guess_separator("a\tb\tc,d", "x.txt") == '\t' && guess_separator("\"a\tb\",c", "x") == ',', "");
    Table t;
    t.name = "contacts";
    auto col = [](const char* n, FieldType ty) { Column c; c.name = n; c.type = ty; return c; };
    t.columns = {col("id", FieldType::Integer), col("last_name", FieldType::Text), col("photo", FieldType::Container),
                 col("birthday", FieldType::Date), col("seen", FieldType::Timestamp), col("at", FieldType::Time),
                 col("vip", FieldType::Boolean), col("age", FieldType::Integer)};
    t.columns[0].pk_position = 1;
    t.columns[0].auto_increment = true;
    t.columns[0].nullable = false;
    const auto m = match_fields({"ID", " Last_Name ", "photo", "nope", "last_name", "AGE"}, t);
    check("import: the first line matched to fields case-blind, each once -- the counting key and binary left out",
          m == std::vector<std::string>{"", "last_name", "", "", "", "age"} && looks_like_header({"x", "Birthday"}, t) &&
              !looks_like_header({"Ada", "1990-07-14"}, t) &&
              import_fields(t) == std::vector<std::string>{"id", "last_name", "birthday", "seen", "at", "vip", "age"}, "");
    auto v = [&](const char* s, std::size_t k) { return import_value(s, t.columns[k]).has_value(); };
    auto why = [&](const char* s, std::size_t k) { auto r = import_value(s, t.columns[k]); return r ? std::string{} : r.error(); };
    check("import: dates, times and timestamps must look like them (ISO, as Export writes); the rest by parse_field",
          v("1990-07-14", 3) && v(" 2024-02-29 ", 3) && !v("2023-02-29", 3) && !v("14/07/1990", 3) && v("", 3) &&
              why("1990-13-01", 3) == "“birthday” needs a date like 1990-07-14" && v("2026-11-07 09:30:00", 4) &&
              v("2026-11-07T09:30:00.123+00:00", 4) && v("2026-11-07", 4) && !v("2026-11-07 25:00", 4) && v("09:30", 5) &&
              v("23:59:59.5", 5) && !v("9:30", 5) && v("Yes", 6) && why("maybe", 6) == "“vip” is Yes or No" &&
              why("4x", 7) == "“age” needs a whole number",
          why("1990-13-01", 3));
    check("import: a plan names each field once, at least one, never binary or missing",
          check_import(t, {{"", "last_name", "age"}, true}).empty() &&
              check_import(t, {{"age", "age"}, true}) == "“age” is picked for two columns" &&
              check_import(t, {{"", ""}, true}) == "Pick a field for at least one column" &&
              check_import(t, {{"photo"}, true}) == "“photo” is binary -- it can't come from a text file" &&
              check_import(t, {{"x"}, true}) == "“contacts” has no field “x”", check_import(t, {{"age", "age"}, true}));
    auto d = read_delimited("last_name,birthday,age\r\nAda,1815-12-10,36\r\n\"Two\nLines\",1990-13-01,1\r\n\r\n"
                            "Bo,,x\r\nCy,,2,extra\r\nDi,,3,\r\n", ',');
    ImportPlan p{{"last_name", "birthday", "age"}, true};
    const auto pv = d ? import_preview(t, *d, p) : ImportResult{};
    check("import: the preview -- good rows counted, refusals by the line each record starts on, an empty line passed by, "
          "an empty extra value let be",
          pv.rows == 5 && pv.records == 2 && pv.refused.size() == 3 && pv.refused[0].line == 3 &&
              refusal_says(pv.refused[0]) == "Line 3: “birthday” needs a date like 1990-07-14" && pv.refused[1].line == 6 &&
              pv.refused[1].why == "“age” needs a whole number" && pv.refused[2].line == 7 &&
              pv.refused[2].why == "it has 4 values -- the file's columns are 3",
          pv.refused.empty() ? "" : refusal_says(pv.refused[0]));
    check("import: the words -- the step's label and the note",
          import_label(40, "contacts") == "Import 40 records into “contacts”" && import_label(1, "c") == "Import 1 record into “c”" &&
              import_says({38, 40, {{7, "x"}, {9, "y"}}}, "contacts") == "Imported 38 records into “contacts” -- 2 refused" &&
              import_says({1, 1, {}}, "c") == "Imported 1 record into “c”", import_says({38, 40, {{7, "x"}, {9, "y"}}}, "contacts"));
}

// On the sample Address Book, writable: Export's file read back through Import --
// every record again, the same values, ONE step; a file with bad rows takes the
// good ones and lists the rest; Ctrl+Z puts it all back.
void test_import_book(Journal& j, Gate& g, const std::string& P) {
    auto count = [&](const std::string& t) {
        auto r = g.query("SELECT COUNT(*) FROM " + g.quote_ident(t));
        return r && !r->rows.empty() ? num(r->rows[0][0]).value_or(-1) : -1.0;
    };
    auto sch = g.introspect();
    const Table* ct = sch ? sch->table("contacts") : nullptr;
    if (!ct) { check(P + "import: the sample's contacts", false, ""); return; }
    const double n0 = count("contacts");
    const int strays0 = j.strays();
    // A record whose notes need every kind of quoting.
    const std::string notes = "a, \"quoted\"\r\nsecond line\tand a tab ";
    auto odd = verbs::new_record(j, "contacts", {"first_name", "last_name", "notes"}, {Value("Odd"), Value("O'Brien, Jr."), Value(notes)});
    // Every field (the key too) out, as CSV.
    ExportRequest all;
    all.fields = table_export_fields(*ct, {});
    all.sort = "last_name";
    std::ostringstream out;
    auto ex = export_records(g, *sch, *ct, all, out);
    auto d = ex ? read_delimited(out.str(), ',') : std::unexpected(std::string("export failed"));
    const auto first = d && !d->rows.empty() ? d->rows[0] : std::vector<std::string>{};
    ImportPlan plan{match_fields(first, *ct), looks_like_header(first, *ct)};
    const bool key_left = !plan.fields.empty() && plan.fields[0].empty();
    auto im = d ? import_records(j, *ct, *d, plan) : std::unexpected(Error{Error::Code::Internal, d.error()});
    check(P + "import: Export's file read back -- the key left to count up, every record in, ONE step named for it",
          odd && im && plan.header && key_left && im->records == n0 + 1 && im->refused.empty() && count("contacts") == 2 * (n0 + 1) &&
              j.undo_label() == import_label(static_cast<std::int64_t>(n0 + 1), "contacts"),
          im ? (im->refused.empty() ? j.undo_label() : refusal_says(im->refused[0])) : im.error().message);
    // The same values: every non-key line of a second export appears exactly twice as often as before.
    ExportRequest nk;
    for (const auto& f : all.fields) if (f != "id") nk.fields.push_back(f);
    std::ostringstream o1, o2;
    (void)j.undo();
    auto e1 = export_records(g, *sch, *ct, nk, o1);
    (void)j.redo();
    auto e2 = export_records(g, *sch, *ct, nk, o2);
    std::multiset<std::string> m1, m2;
    auto rows_of = [](const std::string& s, std::multiset<std::string>& m) {
        auto r = read_delimited(s, ',');
        if (r) for (std::size_t i = 1; i < r->rows.size(); ++i) m.insert(export_line(r->rows[i], ','));
    };
    rows_of(o1.str(), m1);
    rows_of(o2.str(), m2);
    bool doubled = e1 && e2 && !m1.empty() && m2.size() == 2 * m1.size();
    for (const auto& line : m1) doubled = doubled && m2.count(line) == 2 * m1.count(line);
    bool odd_back = false;
    if (auto r = read_delimited(o2.str(), ','); r)
        for (const auto& row : r->rows) odd_back = odd_back || std::find(row.begin(), row.end(), notes) != row.end();
    check(P + "import: the same values came back -- dates, empties, a note with a comma, quotes, a line break, a tab, an end space; "
              "Ctrl+Z took them away, redo put them back",
          doubled && odd_back, std::to_string(m1.size()) + " / " + std::to_string(m2.size()));
    auto u = j.undo();
    check(P + "import: Ctrl+Z puts back the whole import -- one step", u && *u == import_label(static_cast<std::int64_t>(n0 + 1), "contacts") &&
          count("contacts") == n0 + 1, u ? *u : u.error().message);
    (void)j.undo();   // the odd record
    // A file with refusals: no names line, its columns picked by hand.
    const std::string text = "Zed,Zimmer,1990-07-14\nBad,Date,1990-02-30\n\nTwo,\"Line\nName\",\nMore,Values,,here\nLast,One,2001-01-01\n";
    auto d2 = read_delimited(text, ',');
    ImportPlan p2{{"first_name", "last_name", "birthday"}, false};
    auto im2 = d2 ? import_records(j, *ct, *d2, p2) : std::unexpected(Error{Error::Code::Internal, d2.error()});
    check(P + "import: bad rows listed by line, never half a row -- the good three in, one step",
          im2 && im2->rows == 5 && im2->records == 3 && im2->refused.size() == 2 && im2->refused[0].line == 2 &&
              im2->refused[1].line == 6 && count("contacts") == n0 + 3 &&
              j.undo_label() == "Import 3 records into “contacts”",
          im2 ? (im2->refused.empty() ? j.undo_label() : refusal_says(im2->refused[0])) : im2.error().message);
    auto u2 = j.undo();
    const std::string top = j.undo_label();
    auto none = read_delimited("x,y\n", ',');
    auto im3 = none ? import_records(j, *ct, *none, {{"", ""}, false}) : std::unexpected(Error{Error::Code::Internal, ""});
    auto im4 = read_delimited("Only,Bad,nope\n", ',');
    auto r4 = im4 ? import_records(j, *ct, *im4, p2) : std::unexpected(Error{Error::Code::Internal, ""});
    check(P + "import: Ctrl+Z puts it back; a plan with no field refused before anything; nothing made is no step",
          u2 && count("contacts") == n0 && !im3 && im3.error().message == "Pick a field for at least one column" && r4 &&
              r4->records == 0 && r4->refused.size() == 1 && j.undo_label() == top && !j.in_batch(),
          im3 ? "" : im3.error().message);
    // s020c: Stop (the worker's keep_going) -- what went in is kept as the one step.
    auto d5 = read_delimited("Ann,One,\nBen,Two,\nCal,Three,\nDee,Four,\n", ',');
    int asked = 0;
    auto im5 = d5 ? import_records(j, *ct, *d5, p2, [&](std::int64_t n) { ++asked; return n < 2; })
                  : std::unexpected(Error{Error::Code::Internal, ""});
    check(P + "import: stopped after 2 rows -- those 2 kept as ONE step, where it stopped said, the rest not read",
          im5 && im5->records == 2 && im5->rows == 2 && im5->stopped_at == 3 && asked == 3 && count("contacts") == n0 + 2 &&
              j.undo_label() == "Import 2 records into “contacts”" && !j.in_batch() &&
              import_says(*im5, "contacts") == "Imported 2 records into “contacts” -- stopped before line 3, the rest weren't read",
          im5 ? import_says(*im5, "contacts") + " / " + j.undo_label() : im5.error().message);
    auto u5 = j.undo();
    const std::string top5 = j.undo_label();
    auto im6 = d5 ? import_records(j, *ct, *d5, p2, [](std::int64_t) { return false; })
                  : std::unexpected(Error{Error::Code::Internal, ""});
    check(P + "import: Ctrl+Z takes the stopped import back; stopped before the first row -- nothing in, no step",
          u5 && count("contacts") == n0 && im6 && im6->records == 0 && im6->stopped_at == 1 && j.undo_label() == top5 &&
              !j.in_batch(),
          im6 ? std::to_string(im6->stopped_at) : im6.error().message);
    check(P + "import: every write through the door -- no strays", j.strays() == strays0 && count("contacts") == n0,
          std::to_string(j.strays()));
}

// ── s019a: accounts ────────────────────────────────────────────────────────
void test_accounts_pure() {
    check("accounts: the four rights in words, in order; SQL's words to rights",
          privs_says(kRead | kDelete) == "read · delete" && privs_says(kAllPrivs) == "read · add · change · delete" &&
              privs_says(0) == "nothing" && priv_of("select") == kRead && priv_of("DELETE") == kDelete &&
              priv_of("DELETE HISTORY") == 0 && priv_of("CREATE") == 0, privs_says(kRead | kDelete));
    check("accounts: a grantee read -- 'u'@'h', u@h, a role with no host",
          parse_grantee("'acorn'@'localhost'") == std::pair<std::string, std::string>{"acorn", "localhost"} &&
              parse_grantee("acorn@%") == std::pair<std::string, std::string>{"acorn", "%"} &&
              parse_grantee("reader") == std::pair<std::string, std::string>{"reader", ""}, "");
    Account a;
    a.name = "clerk"; a.host = "%"; a.you = true; a.admin = true; a.login = false;
    a.tables = {{"contacts", kRead | kAdd}, {"phones", kRead}};
    Account b;
    b.name = "boss"; b.every = kAllPrivs; b.super = true;
    Account c;
    c.name = "nobody"; c.reaches = false;
    const std::vector<std::string> ts{"contacts", "phones", "addresses"};
    check("accounts: what one may do in lines -- per table when they differ, one line when alike, nothing said so",
          account_lines(a, ts) == std::vector<std::string>{"contacts: read · add", "phones: read", "addresses: nothing"} &&
              account_lines(b, ts) == std::vector<std::string>{"Every table: read · add · change · delete"} &&
              account_lines(c, ts) == std::vector<std::string>{"Nothing in this database"} &&
              account_name(a) == "clerk@%" && account_name(b) == "boss" &&
              account_tags(a) == "you · makes accounts · can't log in" && account_tags(b) == "may do everything" &&
              account_tags(c) == "can't connect here",
          account_lines(a, ts).empty() ? "" : account_lines(a, ts)[0]);
}

// s019b -- every acorn_selftest_<pid>_* account on the server, dropped
// through g.inner() (fixture teardown, as the sample's DROP TABLEs are). Never
// any other name: the pattern is this run's own.
bool drop_test_accounts(Gate& g, Dialect d) {
    const std::string pat = "acorn\\_selftest\\_" + std::to_string(::getpid()) + "\\_%";
    const bool my = d == Dialect::MariaDB;
    auto r = g.inner().query(my ? "SELECT User, Host FROM mysql.user WHERE User LIKE ?"
                                : "SELECT rolname, '' FROM pg_roles WHERE rolname LIKE ?", {Value(pat)});
    if (!r) return false;
    bool ok = true;
    for (const auto& row : r->rows) {
        const std::string n = row[0].as_text() ? *row[0].as_text() : "";
        const std::string h = row[1].as_text() ? *row[1].as_text() : "";
        if (n.rfind("acorn_selftest_" + std::to_string(::getpid()) + "_", 0) != 0) { ok = false; continue; }
        if (my) { ok = g.inner().query("DROP USER '" + n + "'@'" + h + "'").has_value() && ok; continue; }
        (void)g.inner().query("REVOKE ALL ON ALL TABLES IN SCHEMA " + g.quote_ident("public") + " FROM " + g.quote_ident(n));
        auto sc = g.inner().query("SELECT current_schema()");
        if (sc && !sc->rows.empty() && sc->rows[0][0].as_text())
            (void)g.inner().query("REVOKE ALL ON ALL TABLES IN SCHEMA " + g.quote_ident(*sc->rows[0][0].as_text()) + " FROM " + g.quote_ident(n));
        ok = g.inner().query("DROP ROLE " + g.quote_ident(n)).has_value() && ok;
    }
    return ok;
}

// The walk's accounts as their pictures read (absent / present + rights).
std::string acct_says(Journal& j, const Store::Id& id) {
    j.store().clear_failed();
    auto got = j.store().get(id);
    if (j.store().failed()) { std::string e = j.store().error().message; j.store().clear_failed(); return "?" + e; }
    if (!got) return "absent";
    const auto& a = std::get<AccountDef>(*got);
    std::string s = "present " + std::string(a.login ? "login" : "nologin") + " every=" + std::to_string(a.every);
    // s021: how it logs in -- the lock, and a fingerprint of the stored hash
    // (never the hash itself in a check's words).
    s += std::string(a.locked ? " locked" : "") + " pw#" + std::to_string(std::hash<std::string>{}(a.secret) % 100003);
    for (const auto& [t, p] : a.tables) s += " " + t + "=" + std::to_string(p);
    return s;
}
std::string walk_accounts(Journal& j) {
    std::string s;
    for (const char* w : {"new", "give", "every", "take", "del", "pw", "lock", "unlock"}) {
        const std::string n = "acorn_selftest_" + std::to_string(::getpid()) + "_walk_" + w;
        s += std::string(w) + ": " + acct_says(j, Store::account_id(n, j.store().dialect() == Dialect::MariaDB ? "%" : "")) + "; ";
    }
    return s;
}

void test_accounts_changed_pure() {
    using D = Dialect;
    AccountDef r;
    r.name = "r";
    r.host = "%";
    check("accounts: New Account's SQL -- MariaDB, the password a literal (quote and backslash doubled), or hidden",
          account_sql(D::MariaDB, '`', "shop", std::nullopt, r, std::string("it's\\"), false) ==
                  std::vector<std::string>{"CREATE USER 'r'@'%' IDENTIFIED BY 'it''s\\\\'"} &&
              account_sql(D::MariaDB, '`', "shop", std::nullopt, r, std::string("it's\\"), true) ==
                  std::vector<std::string>{"CREATE USER 'r'@'%' IDENTIFIED BY '********'"},
          account_sql(D::MariaDB, '`', "shop", std::nullopt, r, std::string("it's\\"), false)[0]);
    AccountDef back = r;
    back.every = 1;
    back.tables = {{"contacts", 2}};
    const auto again = account_sql(D::MariaDB, '`', "shop", std::nullopt, back, std::nullopt, false);
    check("accounts: made again with no password (Ctrl+Z of Delete) -- MariaDB locks it, then its rights: db.*, per table",
          again == std::vector<std::string>{"CREATE USER 'r'@'%' ACCOUNT LOCK", "GRANT SELECT ON `shop`.* TO 'r'@'%'",
                                            "GRANT INSERT ON `shop`.`contacts` TO 'r'@'%'"},
          again.empty() ? "" : again.back());
    check("accounts: MariaDB's delete is one DROP USER",
          account_sql(D::MariaDB, '`', "shop", back, std::nullopt, std::nullopt, false) == std::vector<std::string>{"DROP USER 'r'@'%'"}, "");
    AccountDef p;
    p.name = "r";
    p.tables = {{"contacts", 1}, {"phones", 2}};
    const auto gone = account_sql(D::PostgreSQL, '"', "shop", p, std::nullopt, std::nullopt, false);
    check("accounts: PostgreSQL takes its grants here back first, then DROP ROLE",
          gone == std::vector<std::string>{"REVOKE SELECT ON \"contacts\" FROM \"r\"", "REVOKE INSERT ON \"phones\" FROM \"r\"",
                                           "DROP ROLE \"r\""}, gone.empty() ? "" : gone[0]);
    AccountDef p2 = p;
    p2.tables = {{"contacts", 1 | 4 | 8}};
    const auto ch = account_sql(D::PostgreSQL, '"', "shop", p, p2, std::nullopt, false);
    check("accounts: a change is a GRANT / REVOKE per table that differs, nothing for the rest",
          ch == std::vector<std::string>{"GRANT UPDATE, DELETE ON \"contacts\" TO \"r\"", "REVOKE INSERT ON \"phones\" FROM \"r\""},
          ch.empty() ? "" : ch[0]);
    const auto made = account_sql(D::PostgreSQL, '"', "shop", std::nullopt, AccountDef{"r", "", true, 0, {}}, std::string("pw"), false);
    check("accounts: PostgreSQL's New Account is CREATE ROLE .. LOGIN PASSWORD",
          made == std::vector<std::string>{"CREATE ROLE \"r\" LOGIN PASSWORD 'pw'"}, made.empty() ? "" : made[0]);
    check("accounts: names refused in words -- empty, end spaces, quotes / @, too long; MariaDB needs a host",
          !check_account_name(D::MariaDB, "", "%").empty() && !check_account_name(D::MariaDB, " a", "%").empty() &&
              !check_account_name(D::PostgreSQL, "a'b", "").empty() && !check_account_name(D::MariaDB, "a@b", "%").empty() &&
              !check_account_name(D::MariaDB, std::string(33, 'a'), "%").empty() &&
              check_account_name(D::PostgreSQL, std::string(33, 'a'), "").empty() &&
              !check_account_name(D::MariaDB, "clerk", "").empty() && check_account_name(D::MariaDB, "clerk", "%").empty(),
          check_account_name(D::MariaDB, "clerk", ""));
    std::string n, h;
    const auto id = Store::account_id("a:b", "10.0.%");
    check("accounts: an account's id read back -- a ':' in the name, a host",
          Store::parse_account_id(id, n, h) && n == "a:b" && h == "10.0.%" && Store::is_account(id) && !Store::is_local(id) &&
              Store::describe(id) == "the account “a:b@10.0.%”", id);
}

// s021 -- passwords and locks: the SQL, pure.
void test_passwords_pure() {
    using D = Dialect;
    AccountDef r;
    r.name = "r";
    r.host = "%";
    r.secret = "mysql_native_password\n*OLD";
    r.locked = true;
    AccountDef open = r;
    open.locked = false;
    auto set = account_sql(D::MariaDB, '`', "shop", r, open, std::string("it's"), false);
    auto shown = account_sql(D::MariaDB, '`', "shop", r, open, std::string("it's"), true);
    check("passwords: MariaDB's Set Password on a locked account -- ONE ALTER USER, typed, unlocked; the preview hides it",
          set == std::vector<std::string>{"ALTER USER 'r'@'%' IDENTIFIED BY 'it''s' ACCOUNT UNLOCK"} &&
              shown == std::vector<std::string>{"ALTER USER 'r'@'%' IDENTIFIED BY '********' ACCOUNT UNLOCK"},
          set.empty() ? "" : set[0]);
    AccountDef now = open;
    now.secret = "mysql_native_password\n*NEW";
    auto back = account_sql(D::MariaDB, '`', "shop", now, r, std::nullopt, false);
    auto back_shown = account_sql(D::MariaDB, '`', "shop", now, r, std::nullopt, true);
    check("passwords: MariaDB's Ctrl+Z -- the stored hash put back as kept, and the lock; the preview hides the hash",
          back == std::vector<std::string>{"ALTER USER 'r'@'%' IDENTIFIED VIA mysql_native_password USING '*OLD' ACCOUNT LOCK"} &&
              back_shown == std::vector<std::string>{"ALTER USER 'r'@'%' IDENTIFIED VIA mysql_native_password USING '********' ACCOUNT LOCK"},
          back.empty() ? "" : back[0]);
    auto lock = account_sql(D::MariaDB, '`', "shop", open, r, std::nullopt, false);
    check("passwords: Lock alone is ACCOUNT LOCK, the password untouched",
          lock == std::vector<std::string>{"ALTER USER 'r'@'%' ACCOUNT LOCK"}, lock.empty() ? "" : lock[0]);
    auto remade = account_sql(D::MariaDB, '`', "shop", std::nullopt, open, std::nullopt, false);
    AccountDef bad = open;
    bad.secret = "x; DROP\n*OLD";
    auto remade_bad = account_sql(D::MariaDB, '`', "shop", std::nullopt, bad, std::nullopt, false);
    check("passwords: made again (Ctrl+Z of Delete) with its hash; a plugin that isn't a word isn't put back -- locked",
          remade == std::vector<std::string>{"CREATE USER 'r'@'%' IDENTIFIED VIA mysql_native_password USING '*OLD'"} &&
              remade_bad == std::vector<std::string>{"CREATE USER 'r'@'%' ACCOUNT LOCK"},
          remade.empty() ? "" : remade[0]);
    AccountDef p;
    p.name = "r";
    p.secret = "SCRAM-SHA-256$4096:abc";
    AccountDef p2 = p;
    p2.secret = "";
    AccountDef ph = p;
    ph.secret = kSecretHidden;
    auto pset = account_sql(D::PostgreSQL, '"', "shop", p, p, std::string("pw"), false);
    auto pback = account_sql(D::PostgreSQL, '"', "shop", p2, p, std::nullopt, false);
    auto pnull = account_sql(D::PostgreSQL, '"', "shop", p, p2, std::nullopt, false);
    auto pmade = account_sql(D::PostgreSQL, '"', "shop", std::nullopt, p2, std::nullopt, false);
    auto phid = account_sql(D::PostgreSQL, '"', "shop", std::nullopt, ph, std::nullopt, false);
    auto phid_shown = account_sql(D::PostgreSQL, '"', "shop", std::nullopt, ph, std::nullopt, true);
    const std::string hp = "CREATE ROLE \"r\" LOGIN PASSWORD '";
    const bool hid_ok = phid.size() == 1 && phid[0].rfind(hp, 0) == 0 && phid[0].size() == hp.size() + 33 &&
                        phid[0].back() == '\'' && phid[0].find_first_not_of("0123456789abcdef", hp.size()) == phid[0].size() - 1;
    check("passwords: PostgreSQL -- typed; a hash put back; none is PASSWORD NULL; made again with none, or (hidden) one no "
          "one knows -- 32 hex digits, shown as ********",
          pset == std::vector<std::string>{"ALTER ROLE \"r\" PASSWORD 'pw'"} &&
              pback == std::vector<std::string>{"ALTER ROLE \"r\" PASSWORD 'SCRAM-SHA-256$4096:abc'"} &&
              pnull == std::vector<std::string>{"ALTER ROLE \"r\" PASSWORD NULL"} &&
              pmade == std::vector<std::string>{"CREATE ROLE \"r\" LOGIN"} && hid_ok &&
              phid_shown == std::vector<std::string>{"CREATE ROLE \"r\" LOGIN PASSWORD '********'"} &&
              account_sql(D::PostgreSQL, '"', "shop", ph, ph, std::nullopt, false).empty(),
          phid.empty() ? "" : phid[0]);
    AccountDef none = open;
    none.secret = "mysql_native_password\n";
    check("passwords: has a password -- MariaDB a stored hash, PostgreSQL one set or one it won't show",
          has_password(D::MariaDB, open) && !has_password(D::MariaDB, none) && has_password(D::PostgreSQL, p) &&
              !has_password(D::PostgreSQL, p2) && has_password(D::PostgreSQL, ph) &&
              verbs::password_label(r) == "Set Password for “r@%” and Unlock" && verbs::password_label(open) == "Set Password for “r@%”",
          verbs::password_label(r));
}

// On the sample Address Book, writable: New Account, rights given and taken,
// Delete Account -- each one step, Ctrl+Z / redo; refusals in words; never
// this connection's own account. Throwaway acorn_selftest_<pid>_* accounts only.
void test_accounts_changed(Journal& j, Gate& g, Dialect d, const std::string& P) {
    j.clear();
    const int strays0 = j.strays();
    const std::string who = "acorn_selftest_" + std::to_string(::getpid()) + "_clerk";
    const bool my = d == Dialect::MariaDB;
    const std::string host = my ? "%" : "";
    const auto id = Store::account_id(who, host);
    if (d == Dialect::SQLite) {
        const auto n = j.size();
        auto a = verbs::new_account(j, who, host, "pw");
        auto b = verbs::set_rights(j, id, "contacts", kRead, true);
        check(P + "accounts changed: SQLite -- New Account and rights refused in words, no step",
              !a && !b && a.error().message.find("no accounts") != std::string::npos && j.size() == n,
              a ? "made!" : a.error().message);
        return;
    }
    auto sch = g.introspect();
    if (!sch) { check(P + "accounts changed: the schema", false, ""); return; }
    std::vector<std::string> ts;
    for (const auto& t : sch->tables) ts.push_back(t.name);
    auto mine = [&]() -> const Account* {
        static std::optional<AccountList> list;
        auto r = read_accounts(g, d, *sch);
        list = r ? std::optional<AccountList>(*r) : std::nullopt;
        if (list) for (const auto& a : list->accounts) if (a.name == who && a.host == host) return &a;
        return nullptr;
    };
    auto says = [&](const Account* a, const std::string& t) { return a ? privs_says(a->tables.at(t)) : std::string("none"); };

    const std::string pw = "it's a \\ pass";
    const auto n0 = j.size();
    auto made = verbs::new_account(j, who, host, pw);
    const Account* a = mine();
    check(P + "accounts changed: New Account -- made, one step named for it, no rights yet",
          made && j.size() == n0 + 1 && j.undo_label() == "New Account " + account_words(who, host) && a &&
              a->every == 0 && says(a, "contacts") == "nothing",
          made ? j.undo_label() : made.error().message);
    check(P + "accounts changed: the password is nowhere in the step", j.undo_label().find(pw) == std::string::npos &&
          acct_says(j, id).find("pass") == std::string::npos, "");
    if (my) {
        auto pwok = g.query("SELECT authentication_string = PASSWORD(?) FROM mysql.user WHERE User = ? AND Host = ?",
                            {Value(pw), Value(who), Value(host)});
        check(P + "accounts changed: MariaDB has the password as typed (a quote and a backslash in it)",
              pwok && !pwok->rows.empty() && pwok->rows[0][0].as_int() == 1, pwok ? "" : pwok.error().message);
    }
    auto twice = verbs::new_account(j, who, host, "x");
    auto nopw = verbs::new_account(j, who + "_b", host, "");
    auto badname = verbs::new_account(j, "a'b", host, "x");
    check(P + "accounts changed: refused in words -- a name taken, no password, a bad name; no step",
          !twice && twice.error().code == Error::Code::Exists && !nopw && !badname && j.size() == n0 + 1,
          twice ? "made twice!" : twice.error().message);

    auto give = verbs::set_rights(j, id, "contacts", kRead, true);
    a = mine();
    check(P + "accounts changed: Give read on contacts -- one step, named; contacts read, phones nothing",
          give && j.undo_label() == "Give " + account_words(who, host) + " read on “contacts”" && says(a, "contacts") == "read" &&
              says(a, "phones") == "nothing", give ? j.undo_label() : give.error().message);
    const auto n1 = j.size();
    auto same = verbs::set_rights(j, id, "contacts", kRead, true);
    check(P + "accounts changed: giving what it has is no step", same && j.size() == n1, "");
    auto want = verbs::rights_wanted(j, id, "contacts", kChange, true);
    auto pv = want ? verbs::account_preview(j, id, *want) : std::unexpected(want.error());
    auto dbn = g.query(my ? "SELECT DATABASE()" : "SELECT current_database()");
    const std::string db = dbn && !dbn->rows.empty() && dbn->rows[0][0].as_text() ? *dbn->rows[0][0].as_text() : "";
    const std::string expect = my ? "GRANT UPDATE ON `" + db + "`.`contacts` TO '" + who + "'@'%'"
                                  : "GRANT UPDATE ON \"contacts\" TO \"" + who + "\"";
    check(P + "accounts changed: the preview is exactly the one GRANT the box will run",
          pv && *pv == std::vector<std::string>{expect}, pv && !pv->empty() ? pv->front() : (pv ? "none" : pv.error().message));
    auto chg = verbs::set_rights(j, id, "contacts", kChange, true);
    check(P + "accounts changed: Give change -- contacts read · change",
          chg && says(mine(), "contacts") == "read · change", chg ? says(mine(), "contacts") : chg.error().message);
    auto u = j.undo();
    const std::string after_undo = says(mine(), "contacts");
    auto r = j.redo();
    check(P + "accounts changed: Ctrl+Z takes change back (read), redo gives it again",
          u && r && after_undo == "read" && says(mine(), "contacts") == "read · change",
          after_undo + (u ? "" : " " + u.error().message));
    auto every = verbs::set_rights(j, id, "", kAdd, true);
    a = mine();
    bool all_add = a != nullptr;
    if (a) for (const auto& t : ts) all_add = all_add && (a->tables.at(t) & kAdd);
    check(P + "accounts changed: Give add on every table -- one step, every table may add" +
              std::string(my ? " (`db`.*)" : " (each table)"),
          every && all_add && j.undo_label() == "Give " + account_words(who, host) + " add on every table",
          every ? j.undo_label() : every.error().message);
    (void)j.undo();
    a = mine();
    check(P + "accounts changed: Ctrl+Z -- no table may add again", a && !(a->tables.at("phones") & kAdd) &&
          says(a, "contacts") == "read · change", says(a, "contacts"));

    // Behind Acorn's back: undo is refused, nothing touched.
    auto give2 = verbs::set_rights(j, id, "phones", kRead, true);
    (void)g.inner().query("REVOKE SELECT ON " + (my ? g.quote_ident(db) + "." : std::string{}) + g.quote_ident("phones") +
                          " FROM " + (my ? "'" + who + "'@'%'" : g.quote_ident(who)));
    auto ref = j.undo();
    check(P + "accounts changed: an account changed behind Acorn's back -- undo refused, nothing touched",
          give2 && !ref && ref.error().code == Error::Code::Changed, ref ? "undone!" : ref.error().message);
    j.clear();

    // Never the connection's own account.
    auto me = g.query(my ? "SELECT CURRENT_USER()" : "SELECT current_user::text");
    const auto mep = me && !me->rows.empty() && me->rows[0][0].as_text() ? parse_grantee(*me->rows[0][0].as_text())
                                                                          : std::pair<std::string, std::string>{};
    const auto own = Store::account_id(mep.first, my ? mep.second : "");
    auto take_own = verbs::set_rights(j, own, "contacts", kRead, false);
    auto del_own = verbs::delete_account(j, own);
    auto raw = j.run("by hand", {own}, [&](Store& s) { s.put(own, std::nullopt); });
    const Account* meacct = nullptr;
    auto lst = read_accounts(g, d, *sch);
    if (lst) for (const auto& x : lst->accounts) if (x.you) meacct = &x;
    check(P + "accounts changed: the connection's own account -- refused by the verbs AND the store; still may do all four",
          !take_own && !del_own && !raw && raw.error().message.find("this connection uses") != std::string::npos &&
              j.size() == 0 && meacct && (meacct->tables.at("contacts") & kAllPrivs) == kAllPrivs,
          raw ? "the store let it through!" : raw.error().message);
    if (my) {
        auto adm = verbs::delete_account(j, Store::account_id("acorn", mep.second == "%" ? "localhost" : "%"));
        check(P + "accounts changed: an account that makes accounts isn't deleted by Acorn -- said in words",
              !adm && adm.error().message.find("makes accounts itself") != std::string::npos && j.size() == 0,
              adm ? "deleted!" : adm.error().message);
    }

    // Delete Account: gone; Ctrl+Z makes it again with its rights -- not its password.
    const std::string before = acct_says(j, id);
    auto del = verbs::delete_account(j, id);
    check(P + "accounts changed: Delete Account -- one step, named, gone (" + before + ")",
          del && j.undo_label() == "Delete Account " + account_words(who, host) && !mine(), del ? j.undo_label() : del.error().message);
    auto back = j.undo();
    a = mine();
    check(P + "accounts changed: Ctrl+Z makes it again with its rights here (contacts read · change)",
          back && a && says(a, "contacts") == "read · change" && acct_says(j, id) == before,
          back ? acct_says(j, id) : back.error().message);
    if (my) {   // s021: with its own password back (the stored hash), not locked
        auto lk = g.query("SELECT COALESCE(JSON_VALUE(g.Priv, '$.account_locked'), 'false'), u.authentication_string = PASSWORD(?) "
                          "FROM mysql.user u JOIN mysql.global_priv g ON g.User = u.User AND g.Host = u.Host "
                          "WHERE u.User = ? AND u.Host = ?", {Value(pw), Value(who), Value(host)});
        const std::string l = lk && !lk->rows.empty() && lk->rows[0][0].as_text() ? *lk->rows[0][0].as_text() : "?";
        const bool same = lk && !lk->rows.empty() && lk->rows[0][1].as_int() == 1;
        check(P + "accounts changed: ... with its own password (MariaDB keeps the hash Acorn puts back), not locked",
              (l == "false" || l == "0") && same, l);
    }
    auto again = j.redo();
    check(P + "accounts changed: redo deletes it again", again && !mine(), again ? "" : again.error().message);
    j.clear();
    check(P + "accounts changed: the throwaway accounts dropped; no strays",
          drop_test_accounts(g, d) && !mine() && j.strays() == strays0, std::to_string(j.strays()));
}

// s021 -- Set Password, Lock / Unlock on the sample: each one step, Ctrl+Z puts
// the old password and lock back (the hash, where the server shows it). On
// MariaDB, LOGGING IN as the account says it worked (an @localhost account:
// the test server's anonymous ''@localhost would shadow a '%' one).
void test_passwords(Engine* e, const ConnectParams& rw, Journal& j, Gate& g, Dialect d, const std::string& P) {
    j.clear();
    const int strays0 = j.strays();
    const bool my = d == Dialect::MariaDB;
    const std::string who = "acorn_selftest_" + std::to_string(::getpid()) + "_login";
    const std::string host = my ? "localhost" : "";
    const auto id = Store::account_id(who, host);
    if (d == Dialect::SQLite) {
        const auto n = j.size();
        auto a = verbs::set_password(j, id, "pw");
        auto b = verbs::set_locked(j, id, true);
        check(P + "passwords: SQLite -- Set Password and Lock refused in words, no step",
              !a && !b && a.error().message.find("no accounts") != std::string::npos && j.size() == n,
              a ? "set!" : a.error().message);
        return;
    }
    // Logs in as it? "" = yes, else the server's words.
    auto login = [&](const std::string& pw) -> std::string {
        ConnectParams p = rw;
        p.user = who;
        p.host = "127.0.0.1";
        p.password = pw;
        p.read_only = true;
        auto c = e->open(p);
        if (!c) return c.error().message.empty() ? std::string("refused") : c.error().message;
        auto q = (*c)->query("SELECT 1");
        return q ? std::string{} : q.error().message;
    };
    const std::string pw1 = "first 'pw' \\ one", pw2 = "second pw";
    auto made = verbs::new_account(j, who, host, pw1);
    auto give = made ? verbs::set_rights(j, id, "contacts", kRead, true) : std::unexpected(made.error());
    check(P + "passwords: the fixture -- an account given read on contacts", made && give, made ? "" : made.error().message);
    if (!made) return;
    if (my) check(P + "passwords: it logs in with its password, not with another",
                  login(pw1).empty() && !login(pw2).empty(), login(pw1));

    const auto n0 = j.size();
    if (my) {
        auto lk = verbs::set_locked(j, id, true);
        const std::string locked_says = login(pw1);
        check(P + "passwords: Lock -- one step named for it; logging in is refused (locked)",
              lk && j.size() == n0 + 1 && j.undo_label() == "Lock " + account_words(who, host) &&
                  locked_says.find("locked") != std::string::npos,
              lk ? j.undo_label() + " / " + locked_says : lk.error().message);
        auto u = j.undo();
        const std::string unl = login(pw1);
        auto r = j.redo();
        check(P + "passwords: Lock's Ctrl+Z lets it in again; redo locks it again",
              u && r && unl.empty() && !login(pw1).empty(), unl);
        auto same = verbs::set_locked(j, id, true);
        check(P + "passwords: locking a locked account is no step", same && j.size() == n0 + 1, "");
    }

    const auto n1 = j.size();
    bool stepped = true;
    auto set = verbs::set_password(j, id, pw2, &stepped);
    const std::string label = "Set Password for " + account_words(who, host) + (my ? " and Unlock" : "");
    if (my) {
        check(P + "passwords: Set Password on a locked account -- ONE step, named, unlocked; logs in with the new one, "
                  "not the old",
              set && stepped && j.size() == n1 + 1 && j.undo_label() == label && login(pw2).empty() && !login(pw1).empty(),
              set ? j.undo_label() + " / " + login(pw2) : set.error().message);
        check(P + "passwords: neither password is anywhere in the step or the picture",
              j.undo_label().find("pw") == std::string::npos && acct_says(j, id).find("pw'") == std::string::npos &&
                  acct_says(j, id).find("second") == std::string::npos, acct_says(j, id));
        auto u = j.undo();
        const std::string old_locked = login(pw1);
        const std::string says = acct_says(j, id);
        auto r = j.redo();
        check(P + "passwords: Ctrl+Z puts the old password back AND the lock (refused: locked); redo -- the new one again",
              u && r && old_locked.find("locked") != std::string::npos && says.find(" locked") != std::string::npos &&
                  login(pw2).empty(), old_locked);
        auto unlock_back = j.undo();   // locked, old password
        auto unl = verbs::set_locked(j, id, false);
        check(P + "passwords: Unlock (it has a password) -- one step; the OLD password logs in again",
              unlock_back && unl && j.undo_label() == "Unlock " + account_words(who, host) && login(pw1).empty(), login(pw1));
    } else {
        const bool kept = j.store().shows_passwords();
        check(P + "passwords: PostgreSQL" + std::string(kept ? " (a superuser) -- one step" : " (not a superuser) -- set, NO step: "
                                                                                              "the server won't show the old one"),
              set && stepped == kept && j.size() == n1 + (kept ? 1 : 0),
              set ? std::to_string(j.size() - n1) : set.error().message);
        auto lk = verbs::set_locked(j, id, true);
        check(P + "passwords: PostgreSQL has no account lock -- refused in words, no step",
              !lk && lk.error().message.find("no account lock") != std::string::npos && j.size() == n1 + (kept ? 1 : 0),
              lk ? "locked!" : lk.error().message);
    }

    // Refused, in words, no step.
    const auto n2 = j.size();
    auto empty = verbs::set_password(j, id, "");
    auto me = g.query(my ? "SELECT CURRENT_USER()" : "SELECT current_user::text");
    const auto mep = me && !me->rows.empty() && me->rows[0][0].as_text() ? parse_grantee(*me->rows[0][0].as_text())
                                                                          : std::pair<std::string, std::string>{};
    auto own = verbs::set_password(j, Store::account_id(mep.first, my ? mep.second : ""), "x");
    auto own_lock = my ? verbs::set_locked(j, Store::account_id(mep.first, mep.second), true)
                       : std::expected<void, Error>(std::unexpected(Error{Error::Code::Unsupported, "this connection uses"}));
    check(P + "passwords: refused in words -- an empty password, your own account (password and lock); no step",
          !empty && !own && !own_lock && own.error().message.find("this connection uses") != std::string::npos &&
              j.size() == n2, own ? "set!" : own.error().message);
    if (my) {
        auto adm = verbs::set_password(j, Store::account_id("acorn", mep.second == "%" ? "localhost" : "%"), "x");
        check(P + "passwords: an account that makes accounts -- its password left to the server's tools, said in words",
              !adm && adm.error().message.find("makes accounts itself") != std::string::npos && j.size() == n2,
              adm ? "set!" : adm.error().message);
        // An account locked with NO password (an older Ctrl+Z of Delete, another
        // tool): Unlock is refused; Set Password unlocks it.
        const std::string bare = "acorn_selftest_" + std::to_string(::getpid()) + "_bare";
        (void)g.inner().query("CREATE USER '" + bare + "'@'%' ACCOUNT LOCK");   // fixture, behind the door
        const auto bid = Store::account_id(bare, "%");
        auto unl = verbs::set_locked(j, bid, false);
        bool st = false;
        auto setb = verbs::set_password(j, bid, "now one", &st);
        const std::string after = acct_says(j, bid);
        check(P + "passwords: locked with no password -- Unlock refused (anyone could log in); Set Password unlocks it, one step",
              !unl && unl.error().message.find("no password") != std::string::npos && setb && st &&
                  after.find(" locked") == std::string::npos && j.undo_label() == "Set Password for “" + bare + "@%” and Unlock",
              unl ? "unlocked!" : unl.error().message + " / " + after);
    }
    j.clear();
    check(P + "passwords: the throwaway accounts dropped; no strays", drop_test_accounts(g, d) && j.strays() == strays0,
          std::to_string(j.strays()));
}

// s020c: the Accounts area's read in one go (it runs on the worker) -- the
// list, may this connection change accounts, each changeable one's picture;
// Stop between pictures.
void test_account_cards(Journal& j, Gate& g, Dialect d, const std::string& P) {
    auto sch = g.introspect();
    if (!sch) { check(P + "account cards: the schema", false, ""); return; }
    const int strays0 = j.strays();
    if (d == Dialect::SQLite) {
        auto c = read_account_cards(g, d, *sch, &j.store(), true);
        check(P + "account cards: SQLite -- none, said in words, nothing more read",
              c.list && !c.list->supported && c.why.empty() && c.given.empty() && !c.stopped, c.list ? c.list->why : "");
        return;
    }
    j.clear();
    const std::string who = "acorn_selftest_" + std::to_string(::getpid()) + "_cards";
    const std::string host = d == Dialect::MariaDB ? "%" : "";
    const auto id = Store::account_id(who, host);
    auto made = verbs::new_account(j, who, host, "pw");
    auto c = read_account_cards(g, d, *sch, &j.store(), true);
    std::size_t changeable = 0;
    if (c.list) for (const auto& a : c.list->accounts) if (account_hold(a).empty()) ++changeable;
    const auto it = c.given.find(id);
    bool you_left = c.list.has_value();
    if (c.list) for (const auto& a : c.list->accounts) if (a.you) you_left = you_left && !c.given.contains(Store::account_id(a.name, a.host));
    check(P + "account cards: read in one go -- may change accounts, the database named, a picture per changeable account "
              "(the new one: given nothing), none for your own",
          made && c.list && c.why.empty() && !c.database.empty() && c.given.size() == changeable && it != c.given.end() &&
              it->second && it->second->every == 0 && it->second->tables.empty() && you_left && !c.stopped,
          made ? c.why + " " + std::to_string(c.given.size()) + "/" + std::to_string(changeable) : made.error().message);
    auto ro = read_account_cards(g, d, *sch, &j.store(), false);
    auto none = read_account_cards(g, d, *sch, nullptr, true);
    check(P + "account cards: a read-only connection (or no journal) -- why in words, no pictures read",
          ro.list && ro.why.find("read-only") != std::string::npos && ro.given.empty() && none.list &&
              none.why.find("read-only") != std::string::npos && none.given.empty(), ro.why);
    std::vector<std::int64_t> told;
    auto st0 = read_account_cards(g, d, *sch, &j.store(), true, [](std::int64_t) { return false; });
    auto st1 = read_account_cards(g, d, *sch, &j.store(), true, [&](std::int64_t n) { told.push_back(n); return n < 1; });
    check(P + "account cards: Stop -- before the first picture none, after the first one; told 0, 1",
          st0.stopped && st0.given.empty() && st0.list && (changeable < 2 || (st1.stopped && st1.given.size() == 1 &&
              told == std::vector<std::int64_t>{0, 1})),
          std::to_string(st1.given.size()) + " / " + std::to_string(changeable));
    j.clear();
    check(P + "account cards: the throwaway account dropped; reading wrote nothing (no strays)",
          drop_test_accounts(g, d) && j.strays() == strays0, std::to_string(j.strays()));
}

// On the sample Address Book: this connection's account, and a throwaway one
// given read on contacts only. Accounts live at SERVER level, outside the
// throwaway database, so only acorn_selftest_<pid>_* names are ever made or
// dropped (fixture setup, behind the door, as the sample's DROP TABLEs are).
void test_accounts(Gate& g, Dialect d, const std::string& P) {
    auto sch = g.introspect();
    if (!sch) { check(P + "accounts: the schema", false, ""); return; }
    const int strays0 = g.strays();
    if (d == Dialect::SQLite) {
        auto r = read_accounts(g, d, *sch);
        check(P + "accounts: a SQLite file has none -- said in words", r && !r->supported && r->accounts.empty() &&
              r->why.find("no accounts") != std::string::npos, r ? r->why : r.error().message);
        return;
    }
    const std::string who = "acorn_selftest_" + std::to_string(::getpid()) + "_reader";
    if (who.rfind("acorn_selftest_", 0) != 0) return;   // never any other name
    const bool my = d == Dialect::MariaDB;
    const std::string acct = my ? "'" + who + "'@'localhost'" : g.quote_ident(who);
    auto dbname = g.query(my ? "SELECT DATABASE()" : "SELECT current_database()");
    const std::string db = dbname && !dbname->rows.empty() && dbname->rows[0][0].as_text() ? *dbname->rows[0][0].as_text() : "";
    auto made = g.inner().query(my ? "CREATE USER " + acct : "CREATE ROLE " + acct);
    auto granted = g.inner().query("GRANT SELECT ON " + (my ? g.quote_ident(db) + "." : std::string{}) +
                                   g.quote_ident("contacts") + " TO " + acct);
    check(P + "accounts: a throwaway account made for the test, read on contacts given (fixture)", made && granted,
          made ? (granted ? "" : granted.error().message) : made.error().message);
    auto r = read_accounts(g, d, *sch);
    const Account* me = nullptr;
    const Account* reader = nullptr;
    if (r) for (const auto& a : r->accounts) { if (a.you) me = &a; if (a.name == who) reader = &a; }
    std::vector<std::string> ts;
    for (const auto& t : sch->tables) ts.push_back(t.name);
    const auto lr = reader ? account_lines(*reader, ts) : std::vector<std::string>{};
    check(P + "accounts: the list read (not only you); this connection's account is marked you -- every table, all four, "
              "makes accounts",
          r && r->supported && !r->only_you && me && me->name == "acorn" && (me->every & kAllPrivs) == kAllPrivs &&
              me->admin && me->tables.size() >= ts.size(),
          r ? (me ? account_name(*me) + " " + privs_says(me->every) + " " + account_tags(*me) : "no 'you'") : r.error().message);
    bool only_contacts = reader != nullptr && reader->every == 0;
    if (reader) for (const auto& t : ts) only_contacts = only_contacts && (reader->tables.at(t) == (t == "contacts" ? kRead : 0u));
    check(P + "accounts: the throwaway one -- read on contacts, nothing on the rest, said per table",
          only_contacts && std::find(lr.begin(), lr.end(), "contacts: read") != lr.end() &&
              std::find(lr.begin(), lr.end(), "phones: nothing") != lr.end(),
          lr.empty() ? "not found" : lr[0] + (lr.size() > 1 ? " | " + lr[1] : ""));
    if (!my) (void)g.inner().query("REVOKE ALL ON " + g.quote_ident("contacts") + " FROM " + acct);
    auto dropped = g.inner().query(my ? "DROP USER " + acct : "DROP ROLE " + acct);
    auto again = read_accounts(g, d, *sch);
    bool gone = again.has_value();
    if (again) for (const auto& a : again->accounts) gone = gone && a.name != who;
    check(P + "accounts: the throwaway account dropped; reading wrote nothing (no strays)",
          dropped && gone && g.strays() == strays0, dropped ? std::to_string(g.strays()) : dropped.error().message);
}

// On the sample Address Book, writable: whole runs through ScriptRun.
void test_scripts(Journal& j, Gate& g, core::Solution& sol, const std::string& P) {
    using core::Script; using K = core::StepKind;
    auto count = [&](const std::string& t) {
        auto r = g.query("SELECT COUNT(*) FROM " + g.quote_ident(t));
        return r && !r->rows.empty() ? num(r->rows[0][0]).value_or(-1) : -1.0;
    };
    auto one = [&](const std::string& sql) -> std::string {
        auto r = g.query(sql);
        if (!r || r->rows.empty() || r->rows[0][0].is_null()) return "(none)";
        const auto& v = r->rows[0][0];
        if (auto t = v.as_text()) return *t;
        if (auto i = v.as_int()) return std::to_string(*i);
        return "?";
    };
    const std::string C = g.quote_ident("contacts");
    auto ln = g.quote_ident("last_name");
    const std::string thorne_co = one("SELECT " + g.quote_ident("company") + " FROM " + C + " WHERE " + ln + " = 'Thorne'");
    // Kept: a refusal first, then a good one.
    auto bad = verbs::save_script(j, Script{"contacts", "x", {{K::SetField, "nope", "1"}}});
    Script tidy{"contacts", "Tidy", {{K::GoToLayout, kSampleListName}, {K::Find, "last_name", "thorne"},
                                     {K::SetField, "company", "Upper(company)"},
                                     {K::NewRecord, "", "first_name = \"Ada\"\nlast_name = \"Lovelace\""},
                                     {K::SetField, "notes", "first_name & \" \" & last_name"},
                                     {K::Sort, "last_name", "", true}, {K::Message, "", "Tidied."}}};
    auto kept = verbs::save_script(j, tidy);
    check(P + "scripts: Save Script refuses a field the table hasn't, keeps a good one (a step)",
          !bad && bad.error().message.find("no field") != std::string::npos && kept && sol.script("contacts", "Tidy") &&
          j.undo_label() == "New Script “Tidy”", kept ? (bad ? "" : bad.error().message) : kept.error().message);
    const auto steps0 = j.size();
    const int strays0 = g.strays();
    ScriptState st0{"contacts"};
    {
        ScriptRun run(j, sol, tidy, st0);
        const auto p1 = run.go();
        const bool msg = p1 == ScriptPause::Message && run.words() == "Tidied." && run.at() == 7;
        const auto p2 = run.go();
        const std::string ada = run.state().record;
        check(P + "scripts: a run -- Go to Layout, Find, Set Field, New Record, Set Field, Sort, Show Message -- "
                  "pauses at the message, then ends",
              msg && p2 == ScriptPause::Done && run.writes() == 3 && run.state().layout == kSampleListName &&
              run.state().sort == "last_name" && run.state().descending && run.state().found.size() == 1 && !ada.empty(),
              run.words());
        check(P + "scripts: its writes landed -- Thorne's company in capitals, Ada made with her notes worked out from her",
              count("contacts") == 13.0 &&
              one("SELECT " + g.quote_ident("company") + " FROM " + C + " WHERE " + ln + " = 'Thorne'") == "THORNE & DAUGHTERS BINDERY" &&
              one("SELECT " + g.quote_ident("notes") + " FROM " + C + " WHERE " + ln + " = 'Lovelace'") == "Ada Lovelace", "");
    }
    check(P + "scripts: the whole run is ONE step on Ctrl+Z, named for the script, through the door",
          j.size() == steps0 + 1 && j.undo_label() == "Script “Tidy”" && g.strays() == strays0,
          std::to_string(steps0) + " -> " + std::to_string(j.size()) + " " + std::to_string(g.strays()));
    auto u = j.undo();
    check(P + "scripts: Ctrl+Z puts every write back -- Ada gone, the company as it was",
          u && count("contacts") == 12.0 &&
          one("SELECT " + g.quote_ident("company") + " FROM " + C + " WHERE " + ln + " = 'Thorne'") == thorne_co,
          u ? "" : u.error().message);
    auto rd = j.redo();
    check(P + "scripts: redo does the run's writes again", rd && count("contacts") == 13.0, rd ? "" : rd.error().message);
    (void)j.undo();
    // Delete Record asks; No stops with nothing written.
    Script gone{"contacts", "Gone", {{K::Find, "last_name", "=Thorne"}, {K::DeleteRecord}, {K::ShowAll}}};
    const auto phones = count("phones");
    const auto steps1 = j.size();
    {
        ScriptRun run(j, sol, gone, st0);
        const auto p = run.go();
        const bool asks = p == ScriptPause::Ask && run.words().find("Delete") == 0;
        run.answer(false);
        check(P + "scripts: Delete Record asks; No stops the script -- nothing written, no step",
              asks && run.go() == ScriptPause::Stopped && run.writes() == 0 && count("contacts") == 12.0,
              run.words());
    }
    check(P + "scripts: ... and no step was kept", j.size() == steps1, std::to_string(j.size()));
    {
        ScriptRun run(j, sol, gone, st0);
        (void)run.go();
        run.answer(true);
        const auto p = run.go();
        check(P + "scripts: Yes deletes -- her phones follow by the Cascade link, in the same step; Show All runs after",
              p == ScriptPause::Done && count("contacts") == 11.0 && count("phones") < phones && run.state().found.empty() &&
              !run.state().record.empty(), run.words());
    }
    auto u2 = j.undo();
    check(P + "scripts: Ctrl+Z brings her and her phones back", u2 && j.undo_label() != "Script “Gone”" &&
          count("contacts") == 12.0 && count("phones") == phones, u2 ? "" : u2.error().message);
    // A refused write puts back everything the run wrote.
    const auto steps2 = j.size();
    Script key{"contacts", "Key", {{K::NewRecord, "", "last_name = \"Zed\""}, {K::SetField, "id", "5"}}};
    {
        ScriptRun run(j, sol, key, st0);
        const auto p = run.go();
        check(P + "scripts: a refused write (the key) stops the run and puts back what it wrote -- Zed never stays",
              p == ScriptPause::Stopped && run.put_back() && run.writes() == 0 && run.at() == 1 &&
              run.words().find("Step 2 (Set Field)") == 0 && run.words().find("put back") != std::string::npos &&
              count("contacts") == 12.0, run.words());
    }
    check(P + "scripts: ... and leaves no step", j.size() == steps2 && g.strays() == strays0,
          std::to_string(steps2) + " -> " + std::to_string(j.size()) + " " + std::to_string(g.strays()));
    {
        ScriptRun run(j, sol, Script{"contacts", "None", {{K::Find, "last_name", "zzzz"}, {K::ShowAll}}}, st0);
        const auto p = run.go();
        check(P + "scripts: a Find that finds nothing stops the run, saying so",
              p == ScriptPause::Stopped && run.words().find("no records match") != std::string::npos && run.at() == 0, run.words());
    }
    {
        ScriptRun run(j, sol, Script{"contacts", "Out", {{K::Preview}, {K::Print}, {K::SetField, "notes", "Length(last_name)"}}},
                      ScriptState{"contacts"});
        const auto a1 = run.go();
        const auto a2 = run.go();
        const auto a3 = run.go();
        check(P + "scripts: Preview and Print... pause for the screen; Set Field with no record picked stops in words",
              a1 == ScriptPause::Preview && a2 == ScriptPause::Print && a3 == ScriptPause::Stopped &&
              run.words().find("no record") != std::string::npos, run.words());
    }
    {   // s017b: a button's way in -- the run starts on the record it was clicked for
        ScriptRun f(j, sol, Script{"contacts", "F", {{K::Find, "last_name", "kettle"}}}, ScriptState{"contacts"});
        (void)f.go();
        const std::string kettle = f.state().record;
        ScriptState at{"contacts"};
        at.record = kettle;
        ScriptRun run(j, sol, Script{"contacts", "Mark", {{K::SetField, "notes", "\"by button\""}}}, at);
        const auto p = run.go();
        const std::string notes = one("SELECT " + g.quote_ident("notes") + " FROM " + C + " WHERE " + ln + " = 'Kettle'");
        check(P + "buttons: a run given the clicked record writes that record (one step); Ctrl+Z puts it back",
              !kettle.empty() && p == ScriptPause::Done && notes == "by button" && j.undo_label() == "Script “Mark”" && j.undo() &&
              one("SELECT " + g.quote_ident("notes") + " FROM " + C + " WHERE " + ln + " = 'Kettle'") != "by button",
              kettle + " " + notes + " " + j.undo_label());
    }
    {   // s017b: renaming a script carries the table's buttons with it, in the same step
        const core::Layout* card0 = sol.layout("contacts", kSampleLayoutName);
        const core::Layout* list0 = sol.layout("contacts", kSampleListName);
        bool laid = card0 && list0;
        if (laid) {
            core::Layout a = *card0, b = *list0;
            core::add_button(a, "Tidy", "Tidy");
            core::add_button(b, "Tidy row", "Tidy");
            core::add_button(b, "Other", "");
            laid = verbs::save_layout(j, a).has_value() && verbs::save_layout(j, b).has_value();
        }
        auto runs = [&](const char* layout, const std::string& script) {
            const auto* l = sol.layout("contacts", layout);
            int n = 0;
            if (l) for (const auto& o : l->objects) if (o.kind == core::ObjectKind::Button && o.script == script) ++n;
            return n;
        };
        Script renamed = *sol.script("contacts", "Tidy");
        renamed.name = "Tidy Up";
        auto rn = verbs::save_script(j, renamed, "Tidy");
        const bool moved = rn && runs(kSampleLayoutName, "Tidy Up") == 1 &&
                           runs(kSampleListName, "Tidy Up") == 1 && runs(kSampleListName, "") == 1 &&
                           runs(kSampleLayoutName, "Tidy") == 0 && j.undo_label() == "Change Script “Tidy”";
        const bool back = j.undo() && sol.script("contacts", "Tidy") && !sol.script("contacts", "Tidy Up") &&
                          runs(kSampleLayoutName, "Tidy") == 1 && runs(kSampleListName, "Tidy") == 1;
        check(P + "buttons: Rename a script -- the buttons that ran it run the new name (one step, the rest left be); "
                  "Ctrl+Z puts the name and the buttons back",
              laid && moved && back, rn ? j.undo_label() : rn.error().message);
        (void)j.undo();   // the two layouts as they were
        (void)j.undo();
    }
    auto del = verbs::delete_script(j, "contacts", "Tidy");
    check(P + "scripts: Delete Script; Ctrl+Z brings it back",
          del && !sol.script("contacts", "Tidy") && j.undo() && sol.script("contacts", "Tidy"), del ? "" : del.error().message);
}

void test_labels_pure(const fs::path& dir) {
    core::Layout l = sample_labels_layout();
    check("labels: the sample's Mailing Labels -- 2 across, 24 apart, a row 520 wide (fits Letter and A4 unscaled)",
          core::check_layout(l).empty() && core::is_labels(l) && core::labels_width(l) == 520 &&
          core::print_scale(core::labels_width(l), core::paper_a4()) == 1.0 && core::labels_label(l) == "2 across, 24 px apart",
          core::check_layout(l));
    using B = core::ReportBlock;
    const auto rows = core::label_blocks(l, 5);
    core::Layout three = l;
    three.across = 3;
    check("labels: rows -- 5 two across: 3 rows (0, 2, 4); 12 three across: 4; none: none; a list: one per record",
          rows == std::vector<B>{{core::Part::Body, -1, 0}, {core::Part::Body, -1, 2}, {core::Part::Body, -1, 4}} &&
          core::label_blocks(three, 12).size() == 4 && core::label_blocks(l, 0).empty() &&
          core::label_blocks(core::default_layout("t", {"a"}), 3).size() == 3, std::to_string(rows.size()));
    const int body = core::part_height(l, core::Part::Body);
    const auto pages = core::paginate_blocks(0, 0, std::vector<int>(6, body + l.gap), {}, 300);
    check("labels: pages -- rows of 112 on a 300 room: 2 a page (3 pages for 12 labels two across)",
          body == 88 && pages.size() == 3 && pages[0] == core::PrintPage{0, 2}, std::to_string(pages.size()));
    core::Layout bad = l, neg = l;
    bad.across = core::kLabelsMax + 1;
    neg.gap = -8;
    check("labels: check_layout -- 1 to 8 across, no negative gap",
          !core::check_layout(bad).empty() && !core::check_layout(neg).empty() && core::labels_label(core::Layout{}) == "one per row", "");
    core::Solution sol;
    sol.layouts["addresses"][l.name] = l;
    sol.layouts["addresses"]["plain"] = core::default_layout("addresses", {"city"});
    sol.layouts["addresses"]["plain"].name = "plain";
    const auto file = (dir / "labels.acorn").string();
    const bool saved = core::save_solution(file, sol);
    auto back = core::load_solution(file);
    const std::string text = slurp(file);
    check("labels: kept in the .acorn and read back (\"labels\" only on a labels layout)",
          saved && back && *back == sol && text.find("\"labels\"") != std::string::npos &&
          text.find("\"across\": 2") != std::string::npos, "");
}

// s015b: the sample's "Addresses by State" -- grouped, summed by group.
void test_report_book(Gate& g, const core::Solution& sol, const std::string& P) {
    auto sch = g.introspect();
    const Table* at = sch ? sch->table("addresses") : nullptr;
    const auto* rep = sol.layout("addresses", kSampleReportName);
    if (!at || !rep) { check(P + "report: the sample's addresses and its report", false, ""); return; }
    const auto strays0 = g.strays();
    check(P + "report: the sample's Addresses by State kept (a leading and a trailing sub by region)",
          *rep == sample_report_layout() && core::check_layout(*rep).empty() &&
          core::group_fields(*rep) == std::vector<std::string>{"region"}, core::check_layout(*rep));
    PageRequest r{0, 1000};
    r.group = core::group_fields(*rep);
    auto page = read_page(g, *at, r);
    std::size_t rk = 0;
    for (std::size_t i = 0; i < at->columns.size(); ++i) if (at->columns[i].name == "region") rk = i;
    std::vector<std::vector<std::string>> keys;
    for (const auto& row : page ? page->rows : std::vector<Row>{}) keys.push_back({cell_text(row[rk], FieldType::Text)});
    bool sorted = page && keys.size() == 12;
    for (std::size_t i = 1; i < keys.size(); ++i) if (keys[i] < keys[i - 1]) sorted = false;
    check(P + "report: the records ordered by the group's field first (KY, OR, TN)", sorted && keys.front()[0] == "KY" && keys.back()[0] == "TN",
          page ? std::to_string(keys.size()) : page.error().message);
    const auto& sums = sol.summaries_of("addresses");
    auto gs = read_group_summaries(g, *at, sums, {"region"});
    const auto sql = group_summary_sql(g, *at, {sums.begin()->second}, {"region"}, "");
    auto n = [&](const std::string& st) {
        if (!gs) return -1.0;
        auto it = gs->find({st});
        if (it == gs->end()) return -2.0;
        auto v = it->second.find(kSampleCountName);
        return v == it->second.end() ? -3.0 : std::get<double>(v->second);
    };
    check(P + "report: one GROUP BY SELECT -- TN 8, OR 3, KY 1",
          gs && gs->size() == 3 && n("TN") == 8 && n("OR") == 3 && n("KY") == 1 && sql.find("GROUP BY") != std::string::npos,
          gs ? std::to_string(n("TN")) + " " + sql : gs.error().message);
    auto found = read_group_summaries(g, *at, sums, {"region"}, {{"city", "Millbrook"}}, &*sch);
    check(P + "report: ... over the found set (Millbrook -- TN 3, no other state)",
          found && found->size() == 1 && found->begin()->first == std::vector<std::string>{"TN"} &&
          std::get<double>(found->begin()->second.at(kSampleCountName)) == 3, found ? std::to_string(found->size()) : found.error().message);
    // What prints: three states, each heading, its records, its count.
    const auto blocks = core::report_blocks(*rep, keys);
    int heads = 0, feet = 0, bodies = 0;
    std::string tn;
    for (const auto& b : blocks) {
        if (b.part == core::Part::Body) { ++bodies; continue; }
        const bool leading = rep->subs[static_cast<std::size_t>(b.sub)].leading;
        (leading ? heads : feet)++;
        if (!leading && keys[static_cast<std::size_t>(b.record)][0] == "TN" && gs) {
            const auto pr = print_record(g, *sch, *at, *rep, page->rows[static_cast<std::size_t>(b.record)], {}, gs->at({"TN"}), "2026-11-08");
            for (std::size_t i = 0; i < rep->objects.size(); ++i)
                if (rep->objects[i].field == kSampleCountName) tn = pr[i].text;
        }
    }
    check(P + "report: prints 3 headings, 12 addresses, 3 counts -- TN's foot says 8",
          heads == 3 && feet == 3 && bodies == 12 && blocks.size() == 18 && blocks.front().part == core::Part::Sub && tn == "8",
          std::to_string(blocks.size()) + " / " + tn);
    check(P + "report: reading is not a write (no strays)", g.strays() == strays0, std::to_string(g.strays()));
}

// ── s009b: the Address Book starter ──────────────────────────────────────────
void test_starter(Journal& j, Gate& g, const std::string& P) {
    auto count = [&](const std::string& t) {
        auto r = g.query("SELECT COUNT(*) FROM " + g.quote_ident(t));
        return r && !r->rows.empty() ? num(r->rows[0][0]).value_or(-1) : -1.0;
    };
    auto made = make_address_book(j);
    if (made) test_print_book(g, P);   // s015a
    auto s = g.introspect();
    bool links = false;
    if (s && s->table("addresses") && s->table("phones")) {
        const auto& a = s->table("addresses")->foreign_keys;
        const auto& p = s->table("phones")->foreign_keys;
        links = a.size() == 1 && p.size() == 1 && a[0].ref_table == "contacts" && p[0].ref_table == "contacts" &&
                upper_of(a[0].on_delete) == "CASCADE" && upper_of(p[0].on_delete) == "CASCADE";
    }
    check(P + "starter: the Address Book -- three tables, both links Cascade to contacts",
          made && s && s->table("contacts") && links, made ? "" : made.error().message);
    check(P + "starter: three people, their addresses and phones",
          count("contacts") == 3.0 && count("addresses") == 3.0 && count("phones") == 4.0, "");
    j.clear();
    check(P + "starter: arrives with nothing to undo (the caller clears the journal)", !j.can_undo() && !j.can_redo(), "");
    auto again = make_address_book(j);
    check(P + "starter: refused where its tables already are -- nothing touched",
          !again && again.error().code == Error::Code::Exists && count("contacts") == 3.0, again ? "made!" : again.error().message);
    const auto before = snapshot(g);
    auto del = verbs::delete_record(j, Store::row_id("contacts", {"id"}, {Value(std::int64_t{1})}));
    check(P + "starter: delete a contact -- its address and phones go with it",
          del && count("contacts") == 2.0 && count("addresses") == 2.0 && count("phones") == 2.0, del ? "" : del.error().message);
    auto u = j.undo();
    check(P + "starter: ... Ctrl+Z brings all of them back", u && snapshot(g) == before, u ? "" : u.error().message);
    j.clear();
}

// ── s010f: the sample Address Book ───────────────────────────────────────────
void test_sample(Journal& j, Gate& g, const std::string& P, Dialect d, Engine* e, const ConnectParams& rw) {
    for (const char* t : {"phones", "addresses", "contacts"})   // test_starter's tables
        (void)g.inner().query("DROP TABLE IF EXISTS " + g.quote_ident(t));
    auto count = [&](const std::string& t) {
        auto r = g.query("SELECT COUNT(*) FROM " + g.quote_ident(t));
        return r && !r->rows.empty() ? num(r->rows[0][0]).value_or(-1) : -1.0;
    };
    core::Solution sol;
    int saves = 0;
    j.store().set_solution(&sol, [&] { ++saves; return true; });
    auto made = make_sample(j);
    check(P + "sample: twelve people, their addresses and phones, through the door",
          made && count("contacts") == 12.0 && count("addresses") == 12.0 && count("phones") == 16.0,
          made ? std::to_string(count("contacts")) + " / " + std::to_string(count("phones")) : made.error().message);
    const auto* card = sol.layout("contacts", kSampleLayoutName);
    check(P + "sample: the Contact Card kept in the solution (saved once)", card && *card == sample_layout() && saves >= 1, "");
    const auto* lst = sol.layout("contacts", kSampleListName);   // s012b
    check(P + "sample: the Contact List kept beside it", lst && *lst == sample_list_layout(), "");
    // s013a: Find -- each found set checked against the same rule applied here to every record.
    if (auto sc = g.introspect(); sc && sc->table("contacts")) {
        const Table& ct = *sc->table("contacts");
        auto all = read_page(g, ct, {0, 100});
        auto col = [&](const std::string& n) {
            for (std::size_t i = 0; i < ct.columns.size(); ++i) if (ct.columns[i].name == n) return i;
            return std::size_t{0};
        };
        auto text_of = [&](const Row& r, const std::string& n) { return cell_text(r[col(n)], ct.column(n)->type); };
        auto lower = [](std::string x) { for (auto& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return x; };
        auto expect = [&](auto pred) {
            std::int64_t n = 0;
            if (all) for (const auto& r : all->rows) n += pred(r) ? 1 : 0;
            return n;
        };
        auto found = [&](std::vector<Criterion> c, PageRequest r = {0, 100}) { return read_found(g, ct, c, r); };
        auto has = [&](const std::string& n, const std::string& w) {
            return [&, n, w](const Row& r) { return lower(text_of(r, n)).find(lower(w)) != std::string::npos; };
        };
        const auto a_n = expect(has("last_name", "a"));
        auto fa = found({{"last_name", "A"}});
        check(P + "find: text contains, any case -- last_name “A” finds every last name with an a",
              all && fa && fa->total == a_n && a_n > 0 && static_cast<std::int64_t>(fa->rows.size()) == a_n,
              fa ? std::to_string(fa->total) + " / " + std::to_string(a_n) : fa.error().message);
        const auto e_n = expect([&](const Row& r) { return r[col("company")].is_null() || text_of(r, "company").empty(); });
        auto fe = found({{"company", " = "}});
        check(P + "find: “=” alone -- company is empty", fe && fe->total == e_n && e_n > 0,
              fe ? std::to_string(fe->total) + " / " + std::to_string(e_n) : fe.error().message);
        const auto both_n = expect([&](const Row& r) { return has("last_name", "e")(r) && (r[col("company")].is_null() || text_of(r, "company").empty()); });
        auto fb = found({{"last_name", "e"}, {"company", "="}, {"email", ""}});
        check(P + "find: two boxes -- both must match; a blank box asks nothing", fb && fb->total == both_n,
              fb ? std::to_string(fb->total) + " / " + std::to_string(both_n) : fb.error().message);
        const auto old_n = expect([&](const Row& r) { auto b = text_of(r, "birthday"); return !b.empty() && b < "1960-01-01"; });
        auto fo = found({{"birthday", "<1960-01-01"}});
        check(P + "find: “<” on a date -- born before 1960", fo && fo->total == old_n && old_n > 0,
              fo ? std::to_string(fo->total) + " / " + std::to_string(old_n) : fo.error().message);
        std::string key;
        for (const auto& c : ct.columns) if (c.pk_position > 0) key = c.name;
        auto f3 = found({{key, "3"}});
        check(P + "find: a number is equal -- id 3 is one record, id 3", f3 && f3->total == 1 && f3->rows.size() == 1 &&
              num(f3->rows[0][col(key)]) == 3.0, f3 ? std::to_string(f3->total) : f3.error().message);
        const auto pct_n = expect(has("notes", "%"));
        auto fp = found({{"notes", "%"}, {"first_name", "_"}});
        check(P + "find: % and _ are only themselves", fp && fp->total == pct_n,
              fp ? std::to_string(fp->total) : fp.error().message);
        auto pg = found({{"last_name", "a"}}, {0, 2});
        check(P + "find: a page of the found set -- two rows, the total counts all",
              pg && pg->rows.size() == std::min<std::size_t>(2, static_cast<std::size_t>(a_n)) && pg->total == a_n, "");
        // s022: requests (OR), Omit, ranges.
        auto req = [](std::string f, std::string t, int r, bool o = false) {
            Criterion c{std::move(f), std::move(t)};
            c.request = r;
            c.omit = o;
            return c;
        };
        auto empty_co = [&](const Row& r) { return r[col("company")].is_null() || text_of(r, "company").empty(); };
        auto kv = [&](const Row& r) { return num(r[col(key)]).value_or(-1); };
        const auto or_n = expect([&](const Row& r) { return has("last_name", "a")(r) || empty_co(r); });
        auto f_or = found({req("last_name", "a", 0), req("company", "=", 1)});
        check(P + "find: two requests -- a record matching EITHER is found (last_name has a, or company empty)",
              f_or && f_or->total == or_n && or_n > a_n, f_or ? std::to_string(f_or->total) + " / " + std::to_string(or_n) : f_or.error().message);
        const auto om_n = expect([&](const Row& r) { return has("last_name", "a")(r) && !empty_co(r); });
        auto f_om = found({req("last_name", "a", 0), req("company", "=", 1, true)});
        check(P + "find: an Omit request -- last_name has a, less those with no company",
              f_om && f_om->total == om_n && om_n < a_n, f_om ? std::to_string(f_om->total) + " / " + std::to_string(om_n) : f_om.error().message);
        const auto only_n = expect([&](const Row& r) { return !has("last_name", "a")(r); });
        auto f_only = found({req("last_name", "a", 0, true)});
        check(P + "find: only an Omit request -- every record but those (an empty last name isn't omitted)",
              f_only && f_only->total == only_n && only_n + a_n == expect([](const Row&) { return true; }),
              f_only ? std::to_string(f_only->total) + " / " + std::to_string(only_n) : f_only.error().message);
        const auto mix_n = expect([&](const Row& r) {
            return ((has("last_name", "a")(r) && kv(r) > 2) || kv(r) == 1) && !empty_co(r) && !has("first_name", "zz")(r);
        });
        auto f_mix = found({req("last_name", "a", 0), req(key, ">2", 0), req(key, "1", 1), req("company", "=", 2, true),
                            req("first_name", "zz", 3, true)});
        check(P + "find: (a AND >2) OR id 1, omitting no company OR first name zz -- the values bound in order",
              f_mix && f_mix->total == mix_n, f_mix ? std::to_string(f_mix->total) + " / " + std::to_string(mix_n) : f_mix.error().message);
        const auto r_n = expect([&](const Row& r) { return kv(r) >= 2 && kv(r) <= 4; });
        auto f_r = found({{key, "2..4"}});
        auto f_r3 = found({{key, " 2 ... 4 "}});
        check(P + "find: a range on a number -- 2..4 (and 2 ... 4) is 2, 3, 4", f_r && f_r3 && f_r->total == r_n && r_n == 3 &&
              f_r3->total == r_n, f_r ? std::to_string(f_r->total) : f_r.error().message);
        const auto d_n = expect([&](const Row& r) { auto b = text_of(r, "birthday"); return !b.empty() && b >= "1950-01-01" && b <= "1969-12-31"; });
        auto f_d = found({{"birthday", "1950-01-01..1969-12-31"}});
        check(P + "find: a range on a date -- born in the fifties and sixties", f_d && f_d->total == d_n && d_n > 0,
              f_d ? std::to_string(f_d->total) + " / " + std::to_string(d_n) : f_d.error().message);
        const auto t_n = expect([&](const Row& r) { auto l = lower(text_of(r, "last_name")); return !l.empty() && l >= "a" && l <= "m"; });
        auto f_t = found({{"last_name", "A..m"}});
        check(P + "find: a range on text, any case -- last names a to m", f_t && f_t->total == t_n && t_n > 0,
              f_t ? std::to_string(f_t->total) + " / " + std::to_string(t_n) : f_t.error().message);
        auto half = found({{key, "..5"}});
        auto badr = found({{key, "1..x"}});
        check(P + "find: a range missing an end, or with an end the field can't hold -- refused in words",
              !half && half.error().message.find("both ends") != std::string::npos && !badr,
              half ? "found!" : half.error().message);
        // s013b: through a link -- a portal's column, a related field.
        if (sc->table("phones") && sc->table("addresses")) {
            const Table& ph = *sc->table("phones");
            const Table& ad = *sc->table("addresses");
            auto phones = read_page(g, ph, {0, 500});
            auto addrs = read_page(g, ad, {0, 500});
            auto pcol = [&](const Table& tt, const std::string& n) {
                for (std::size_t i = 0; i < tt.columns.size(); ++i) if (tt.columns[i].name == n) return i;
                return std::size_t{0};
            };
            std::set<double> with;   // contacts with a phone holding "0177"
            if (phones) for (const auto& r : phones->rows)
                if (cell_text(r[pcol(ph, "number")], FieldType::Text).find("0177") != std::string::npos) with.insert(num(r[pcol(ph, "contact_id")]).value_or(-1));
            const auto ph_n = expect([&](const Row& r) { return with.count(num(r[col(key)]).value_or(-2)) > 0; });
            const Criterion by_phone{"number", "0177", "phones", key, "contact_id"};
            auto fph = read_found(g, ct, {by_phone}, {0, 100}, &*sc);
            check(P + "find: through a portal -- contacts with a phone holding “0177”", fph && fph->total == ph_n && ph_n > 0,
                  fph ? std::to_string(fph->total) + " / " + std::to_string(ph_n) : fph.error().message);
            const auto both = expect([&](const Row& r) { return with.count(num(r[col(key)]).value_or(-2)) > 0 && has("last_name", "o")(r); });
            auto fpb = read_found(g, ct, {by_phone, {"last_name", "o"}}, {0, 100}, &*sc);
            check(P + "find: a portal box and an own box -- both", fpb && fpb->total == both,
                  fpb ? std::to_string(fpb->total) + " / " + std::to_string(both) : fpb.error().message);
            std::set<double> th;   // contacts whose last name has "th"
            if (all) for (const auto& r : all->rows) if (has("last_name", "th")(r)) th.insert(num(r[col(key)]).value_or(-1));
            std::int64_t ad_n = 0;
            if (addrs) for (const auto& r : addrs->rows) ad_n += th.count(num(r[pcol(ad, "contact_id")]).value_or(-2)) ? 1 : 0;
            auto fad = read_found(g, ad, {{"last_name", "TH", "contacts", "contact_id", key}}, {0, 100}, &*sc);
            check(P + "find: through a related field -- addresses whose contact's last name has “th”",
                  fad && fad->total == ad_n && ad_n > 0, fad ? std::to_string(fad->total) + " / " + std::to_string(ad_n) : fad.error().message);
            auto nos = read_found(g, ct, {by_phone}, {0, 100});
            auto badt = read_found(g, ct, {{"number", "1", "nope", key, "contact_id"}}, {0, 100}, &*sc);
            auto badi = read_found(g, ct, {{"number", "1", "phones", key, "nope"}}, {0, 100}, &*sc);
            check(P + "find: a link without the schema / to no table / by a field it hasn't -> refused in words",
                  !nos && !badt && badt.error().message.find("nope") != std::string::npos && !badi, "");
        }
        auto none = found({{"last_name", "zzqx"}});
        auto blank = found({{"last_name", "  "}});
        auto bad = found({{key, "abc"}});
        auto gone = found({{"fax", "1"}});
        auto dangle = found({{key, ">="}});
        check(P + "find: nothing matches -> none; no criterion / a value the field can't hold / a field it hasn't / an operator alone -> refused in words",
              none && none->total == 0 && !blank && !bad && bad.error().message.find("whole number") != std::string::npos &&
              !gone && gone.error().message.find("fax") != std::string::npos && !dangle,
              bad ? "read!" : bad.error().message);
    }
    // s011a: a portal's rows -- each contact's phones, read by the link.
    if (auto sc = g.introspect(); sc && sc->table("phones") && sc->table("contacts")) {
        const Table& ph = *sc->table("phones");
        auto all = read_page(g, *sc->table("contacts"), {0, 50});
        std::int64_t sum = 0;
        bool each_ok = all.has_value(), one_page = true, every_row_points = true;
        if (all)
            for (const auto& row : all->rows) {
                auto rel = read_related(g, ph, {"contact_id"}, {row[0]}, {0, 50});
                if (!rel) { each_ok = false; break; }
                sum += rel->total;
                one_page = one_page && static_cast<std::int64_t>(rel->rows.size()) == rel->total;
                const auto ci = static_cast<std::size_t>(std::find(rel->columns.begin(), rel->columns.end(), "contact_id") - rel->columns.begin());
                for (const auto& r : rel->rows)
                    every_row_points = every_row_points && ci < r.size() && num(r[ci]) == num(row[0]);
            }
        check(P + "portals: each contact's phones by the link -- every row points at it, the counts add up to 16",
              each_ok && sum == 16 && one_page && every_row_points, std::to_string(sum));
        auto paged = all && !all->rows.empty() ? read_related(g, ph, {"contact_id"}, {all->rows[0][0]}, {0, 1}) : std::expected<Page, Error>{};
        check(P + "portals: a page of them -- one row, the total still counts every match",
              paged && paged->rows.size() <= 1 && paged->total >= static_cast<std::int64_t>(paged->rows.size()), "");
        auto none = read_related(g, ph, {"contact_id"}, {Value()}, {0, 50});
        auto nobody = read_related(g, ph, {"contact_id"}, {Value(std::int64_t{999999})}, {0, 50});
        auto bad = read_related(g, ph, {"no_such"}, {Value(std::int64_t{1})}, {0, 50});
        auto odd = read_related(g, ph, {"contact_id"}, {}, {0, 50});
        check(P + "portals: an empty key -> none; a key nothing points at -> none; a field it hasn't / a mismatch -> refused in words",
              none && none->rows.empty() && none->total == 0 && nobody && nobody->rows.empty() && nobody->total == 0 &&
              !bad && bad.error().message.find("no_such") != std::string::npos && !odd, bad ? "read!" : bad.error().message);
        // s011d: a related field's read -- each address's contact by its contact_id (the Form's own call: the
        // parent's key bound, one record), the same last_name as the contact read directly.
        if (all && sc->table("addresses")) {
            auto ad = read_page(g, *sc->table("addresses"), {0, 50});
            const Table& ct = *sc->table("contacts");
            std::string key;
            for (const auto& c : ct.columns) if (c.pk_position > 0) key = c.name;
            auto at = [](const Page& p, const std::string& c) {
                return static_cast<std::size_t>(std::find(p.columns.begin(), p.columns.end(), c) - p.columns.begin());
            };
            int matched = 0;
            bool same = ad.has_value() && !key.empty();
            if (same)
                for (const auto& row : ad->rows) {
                    const auto& link = row[at(*ad, "contact_id")];
                    auto parent = read_related(g, ct, {key}, {link}, {0, 1});
                    if (!parent || parent->rows.size() != 1) { same = false; break; }
                    for (const auto& c : all->rows)
                        if (num(c[at(*all, key)]) == num(link) &&
                            cell_text(c[at(*all, "last_name")], FieldType::Text) ==
                                cell_text(parent->rows[0][at(*parent, "last_name")], FieldType::Text)) ++matched;
                }
            auto nowhere = read_related(g, ct, {key}, {Value(std::int64_t{999999})}, {0, 1});
            check(P + "related: each address's contact by its link -- one record, the same last_name; a link to nothing -> none",
                  same && matched == 12 && nowhere && nowhere->rows.empty(), std::to_string(matched));
        }
        // s011b: what a portal does -- New with the link filled in, an edit, Delete; each one step, each undone.
        if (all && !all->rows.empty()) {
            const Value who = all->rows[0][0];
            auto before = read_related(g, ph, {"contact_id"}, {who}, {0, 50});
            const auto n0 = before ? before->total : -1;
            const auto steps = j.size();
            auto made = verbs::new_record(j, "phones", {"contact_id"}, {who});
            auto after = read_related(g, ph, {"contact_id"}, {who}, {0, 50});
            check(P + "portals: New -- one step, the link filled in, it lists with the others",
                  made && after && after->total == n0 + 1 && j.size() == steps + 1 &&
                  std::find(after->ids.begin(), after->ids.end(), *made) != after->ids.end(),
                  made ? j.undo_label() : made.error().message);
            auto ed = made ? verbs::set_field(j, *made, "number", Value("555-0199")) : std::expected<void, Error>{};
            auto seen = read_related(g, ph, {"contact_id"}, {who}, {0, 50});
            bool has = false;
            if (seen) for (const auto& r : seen->rows) for (const auto& v : r) has = has || (v.as_text() && *v.as_text() == "555-0199");
            check(P + "portals: a row edited in place -- saved, still pointing here", ed && has, ed ? "" : ed.error().message);
            auto u1 = j.undo();
            auto u2 = j.undo();
            auto back = read_related(g, ph, {"contact_id"}, {who}, {0, 50});
            check(P + "portals: Ctrl+Z twice -- the edit, then the new record, gone", u1 && u2 && back && back->total == n0, "");
            if (back && !back->ids.empty()) {
                auto del = verbs::delete_record(j, back->ids.front());
                auto less = read_related(g, ph, {"contact_id"}, {who}, {0, 50});
                auto ud = j.undo();
                auto again = read_related(g, ph, {"contact_id"}, {who}, {0, 50});
                check(P + "portals: Delete a row -- one fewer; Ctrl+Z brings it back, the same id",
                      del && less && less->total == n0 - 1 && ud && again && again->total == n0 &&
                      std::find(again->ids.begin(), again->ids.end(), back->ids.front()) != again->ids.end(),
                      del ? "" : del.error().message);
            }
        }
        const auto sql = related_sql(g, ph, {"contact_id"}, {0, 10});
        check(P + "portals: the value is bound, never written into the SQL", sql.find("= ?") != std::string::npos &&
              sql.find("WHERE") != std::string::npos, sql);
    } else {
        check(P + "portals: the sample read back", false, "");
    }
    j.clear();
    check(P + "sample: arrives with nothing to undo (the caller clears the journal)", !j.can_undo() && !j.can_redo(), "");
    test_report_book(g, sol, P);   // s015b
    test_scripts(j, g, sol, P);    // s017a
    test_export_book(g, P);        // s018a
    test_import_book(j, g, P);     // s018b
    test_accounts(g, d, P);        // s019a
    test_accounts_changed(j, g, d, P);   // s019b
    test_account_cards(j, g, d, P);      // s020c
    test_passwords(e, rw, j, g, d, P);   // s021
    {   // s016
        const auto* lab = sol.layout("addresses", kSampleLabelsName);
        check(P + "labels: the sample's Mailing Labels kept beside the report", lab && *lab == sample_labels_layout(), "");
    }
    j.store().set_solution(nullptr, {});
    for (const char* t : {"phones", "addresses", "contacts"})
        (void)g.inner().query("DROP TABLE IF EXISTS " + g.quote_ident(t));
    j.store().forget_schema();
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

    // s009: the walk's journal keeps its boxes in a solution of its own.
    core::Solution walk_solution;
    int walk_saves = 0;
    j.store().set_solution(&walk_solution, [&] { ++walk_saves; return true; });

    // Walk every verb the UI offers: a step, nothing strays, undo puts it back
    // exactly, redo reproduces it exactly.
    for (const auto& v : verbs::all()) {
        if (v.accounts && d == Dialect::SQLite) {   // s019b: no accounts to change
            const auto n = j.size();
            auto r = v.run(j);
            check(P + "verb " + v.name + ": a SQLite file has no accounts -- refused in words, no step",
                  !r && r.error().message.find("no accounts") != std::string::npos && j.size() == n,
                  r ? "ran!" : r.error().message);
            continue;
        }
        if (v.setup) {
            auto su = v.setup(j);
            check(P + "verb " + v.name + ": its fixture, through the door", su.has_value(), su ? "" : su.error().message);
        }
        if (const auto why = v.refused_here ? v.refused_here(j) : std::string{}; !why.empty()) {   // s021
            const auto n = j.size();
            auto r = v.run(j);
            check(P + "verb " + v.name + ": not on this server -- refused in words (" + why + "), no step",
                  !r && r.error().message.find(why) != std::string::npos && j.size() == n, r ? "ran!" : r.error().message);
            continue;
        }
        if (v.no_step_here && v.no_step_here(j)) {   // s021: runs, but the server keeps nothing Acorn could put back
            const auto n = j.size();
            const bool redo = j.can_redo();
            auto r = v.run(j);
            check(P + "verb " + v.name + ": runs here but keeps no step (the server won't show what to put back)",
                  r.has_value() && j.size() == n && j.can_redo() == redo, r ? std::to_string(j.size()) : r.error().message);
            continue;
        }
        const auto before = snapshot(g);
        const auto sol_before = walk_solution;
        const auto acc_before = v.accounts ? walk_accounts(j) : std::string{};
        const auto steps = j.size() - (j.can_redo() ? 1 : 0);   // s019b: a redo the step discards
        auto r = v.run(j);
        const auto after = snapshot(g);
        const auto sol_after = walk_solution;
        const auto acc_after = v.accounts ? walk_accounts(j) : std::string{};
        if (v.accounts) {   // s019b: the server's accounts change, this database's tables don't
            check(P + "verb " + v.name + ": runs, is one step, changes the server's accounts -- not the tables",
                  r.has_value() && j.size() == steps + 1 && j.can_undo() && after == before && acc_after != acc_before,
                  r ? acc_before + " -> " + acc_after : r.error().message);
            auto u = j.undo();
            check(P + "verb " + v.name + ": undo puts the account back exactly",
                  u.has_value() && walk_accounts(j) == acc_before && snapshot(g) == before && j.can_redo(),
                  u ? walk_accounts(j) : u.error().message);
            auto re = j.redo();
            check(P + "verb " + v.name + ": redo reproduces it exactly",
                  re.has_value() && walk_accounts(j) == acc_after && snapshot(g) == after, re ? walk_accounts(j) : re.error().message);
            (void)j.undo();
            continue;
        }
        if (v.local) {   // s009: the solution changes, the database never does
            check(P + "verb " + v.name + ": runs, is one step, changes the solution -- not the database",
                  r.has_value() && j.size() == steps + 1 && j.can_undo() && after == before && sol_after != sol_before,
                  r ? std::to_string(steps) + " -> " + std::to_string(j.size()) + (after == before ? "" : ", db changed") +
                          (sol_after != sol_before ? "" : ", solution unchanged")
                    : r.error().message);
        } else {
            check(P + "verb " + v.name + ": runs, is one step, changes the database",
                  r.has_value() && j.size() == steps + 1 && j.can_undo() && after != before,
                  r ? "" : r.error().message);
        }
        auto u = j.undo();
        check(P + "verb " + v.name + ": undo puts the schema back exactly",
              u.has_value() && snapshot(g) == before && walk_solution == sol_before && j.can_redo(), u ? "" : u.error().message);
        auto re = j.redo();
        check(P + "verb " + v.name + ": redo reproduces it exactly",
              re.has_value() && snapshot(g) == after && walk_solution == sol_after, re ? "" : re.error().message);
        (void)j.undo();
    }
    check(P + "no verb wrote outside the door", j.strays() == 1, std::to_string(j.strays()) + " strays");
    if (d != Dialect::SQLite)   // s019b: the walk's throwaway accounts go (fixture teardown, behind the door)
        check(P + "the walk's throwaway accounts dropped", drop_test_accounts(g, d) && walk_accounts(j).find("present") == std::string::npos,
              walk_accounts(j));

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
    test_design(j, g, e, P);
    test_key(j, g, e, P);
    test_links(j, g, e, P);
    test_starter(j, g, P);
    test_sample(j, g, P, d, e, rw);
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
    test_boxes(e, T.ro, std::string(e->id()) + ": ");   // s009
    test_layouts(e, T.ro, std::string(e->id()) + ": "); // s010
    test_calcs(e, T.ro, std::string(e->id()) + ": ");   // s014a
    test_summaries(e, T.ro, std::string(e->id()) + ": ");   // s014b
    test_value_lists(e, T.ro, std::string(e->id()) + ": "); // s014c
    test_print(e, T.ro, std::string(e->id()) + ": ");       // s015a

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
    test_design_pure();
    test_key_pure();
    test_links_pure();
    test_convert_pure();
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
        test_solution_pure(tmp.p);            // s009
        test_boxes(e, T.ro, "sqlite: ");      // s009: boxes move on a read-only connection
        test_layouts_pure(tmp.p);             // s010
        test_layouts(e, T.ro, "sqlite: ");    // s010: layouts kept on a read-only connection
        test_calc_pure(tmp.p);                // s014a
        test_calcs(e, T.ro, "sqlite: ");      // s014a: calculations kept on a read-only connection
        test_summary_pure(tmp.p);             // s014b
        test_summaries(e, T.ro, "sqlite: ");  // s014b: summaries kept read-only, worked out by the database
        test_value_list_pure(tmp.p);          // s014c
        test_value_lists(e, T.ro, "sqlite: ");  // s014c: value lists kept read-only, read from a field
        test_print_pure();                    // s015a
        test_print(e, T.ro, "sqlite: ");      // s015a: a record's layout resolved to what it prints
        test_report_pure(tmp.p);              // s015b: sub-summary parts
        test_labels_pure(tmp.p);              // s016: labels
        test_script_pure(tmp.p);              // s017a: scripts
        test_button_pure(tmp.p);              // s017b: buttons
        test_export_pure();                   // s018a: export
        test_import_pure();                   // s018b: import
        test_accounts_pure();                 // s019a: accounts
        test_accounts_changed_pure();         // s019b: accounts changed
        test_passwords_pure();                // s021: passwords and locks
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
