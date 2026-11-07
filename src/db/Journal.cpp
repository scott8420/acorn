#include "db/Journal.hpp"
#include "db/Convert.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

// db/Journal.cpp -- the door (s005b). Read include/db/Journal.hpp first.

namespace acorn::db {

namespace {

std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
std::string curly(const std::string& name) { return "“" + name + "”"; }
template <class Step>
std::vector<Store::Id> ids_of(const Step& st) {
    std::vector<Store::Id> v;
    for (const auto& e : st.entries) v.push_back(e.id);
    return v;
}
std::string name_of(const Store::Id& id) {
    return id.rfind("table:", 0) == 0 ? id.substr(6) : id;
}

}  // namespace

// ── pictures ────────────────────────────────────────────────────────────────
TableDef TableDef::from(const Table& t) {
    TableDef d;
    d.name = t.name;
    for (const auto& c : t.columns)
        d.columns.push_back({c.name, c.decl_type, c.nullable, c.default_sql, c.pk_position, c.auto_increment});
    for (const auto& ix : t.indexes)
        if (!ix.primary) d.indexes.push_back({ix.name, ix.columns, ix.unique, ix.partial});
    std::sort(d.indexes.begin(), d.indexes.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return d;
}

const ColumnDef* TableDef::column(const std::string& n) const {
    for (const auto& c : columns) if (c.name == n) return &c;
    return nullptr;
}

bool TableDef::operator==(const TableDef& o) const {
    if (name != o.name || rows != o.rows || columns.size() != o.columns.size()) return false;
    for (const auto& c : columns) {
        const ColumnDef* m = o.column(c.name);
        if (!m || !(*m == c)) return false;
    }
    auto a = indexes, b = o.indexes;
    auto by_name = [](const IndexDef& x, const IndexDef& y) { return x.name < y.name; };
    std::sort(a.begin(), a.end(), by_name);
    std::sort(b.begin(), b.end(), by_name);
    return a == b;
}

bool RowDef::operator==(const RowDef& o) const {
    if (table != o.table || columns.size() != o.columns.size() || values.size() != columns.size() ||
        o.values.size() != o.columns.size())
        return false;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const Value* v = o.field(columns[i]);
        if (!v || !(*v == values[i])) return false;
    }
    return true;
}

// ── the DDL writer ──────────────────────────────────────────────────────────
std::string decl_for(Dialect d, FieldType t) {
    switch (d) {
    case Dialect::SQLite:
        switch (t) {
        case FieldType::Integer:   return "INTEGER";
        case FieldType::Number:    return "REAL";
        case FieldType::Date:      return "DATE";
        case FieldType::Time:      return "TIME";
        case FieldType::Timestamp: return "DATETIME";
        case FieldType::Boolean:   return "BOOLEAN";
        case FieldType::Container: return "BLOB";
        default:                   return "TEXT";
        }
    case Dialect::MariaDB:
        switch (t) {
        case FieldType::Integer:   return "BIGINT";
        case FieldType::Number:    return "DOUBLE";
        case FieldType::Date:      return "DATE";
        case FieldType::Time:      return "TIME";
        case FieldType::Timestamp: return "DATETIME";
        case FieldType::Boolean:   return "BOOLEAN";
        case FieldType::Container: return "LONGBLOB";
        default:                   return "VARCHAR(255)";
        }
    case Dialect::PostgreSQL:
        switch (t) {
        case FieldType::Integer:   return "bigint";
        case FieldType::Number:    return "double precision";
        case FieldType::Date:      return "date";
        case FieldType::Time:      return "time";
        case FieldType::Timestamp: return "timestamp";
        case FieldType::Boolean:   return "boolean";
        case FieldType::Container: return "bytea";
        default:                   return "text";
        }
    }
    return "TEXT";
}

std::string create_table_sql(Dialect d, char q, const TableDef& t) {
    std::vector<const ColumnDef*> pk;
    for (const auto& c : t.columns) if (c.pk_position > 0) pk.push_back(&c);
    std::sort(pk.begin(), pk.end(), [](auto* a, auto* b) { return a->pk_position < b->pk_position; });
    // SQLite's counting key must be written inline: INTEGER PRIMARY KEY
    // AUTOINCREMENT (ids are never reused, like a FileMaker serial).
    const bool inline_pk = d == Dialect::SQLite && pk.size() == 1 && pk[0]->auto_increment;

    std::string s = "CREATE TABLE " + quote_ident_with(q, t.name) + " (";
    bool first = true;
    for (const auto& c : t.columns) {
        s += first ? "\n  " : ",\n  ";
        first = false;
        s += quote_ident_with(q, c.name) + " " + c.decl_type;
        if (inline_pk && &c == pk[0]) { s += " PRIMARY KEY AUTOINCREMENT"; continue; }
        if (!c.nullable) s += " NOT NULL";
        if (c.auto_increment && d == Dialect::PostgreSQL) s += " GENERATED BY DEFAULT AS IDENTITY";
        else if (c.default_sql) s += " DEFAULT " + *c.default_sql;
        if (c.auto_increment && d == Dialect::MariaDB) s += " AUTO_INCREMENT";
    }
    if (!pk.empty() && !inline_pk) {
        s += ",\n  PRIMARY KEY (";
        for (std::size_t i = 0; i < pk.size(); ++i)
            s += (i ? ", " : "") + quote_ident_with(q, pk[i]->name);
        s += ")";
    }
    s += "\n)";
    return s;
}

std::string drop_table_sql(char q, const std::string& name) {
    return "DROP TABLE " + quote_ident_with(q, name);
}

// ── s007: the designer's SQL ─────────────────────────────────────────────────
std::string create_index_sql(Dialect, char q, const std::string& table, const IndexDef& ix) {
    std::string s = std::string("CREATE ") + (ix.unique ? "UNIQUE " : "") + "INDEX " + quote_ident_with(q, ix.name) +
                    " ON " + quote_ident_with(q, table) + " (";
    for (std::size_t i = 0; i < ix.columns.size(); ++i) s += (i ? ", " : "") + quote_ident_with(q, ix.columns[i]);
    return s + ")";
}

std::string drop_index_sql(Dialect d, char q, const std::string& table, const std::string& index) {
    std::string s = "DROP INDEX " + quote_ident_with(q, index);
    if (d == Dialect::MariaDB) s += " ON " + quote_ident_with(q, table);
    return s;
}

bool index_name_taken(const Schema& s, const std::string& name) {
    for (const auto& t : s.tables)
        for (const auto& ix : t.indexes)
            if (lower(ix.name) == lower(name)) return true;
    return false;
}

namespace {

// A field as ADD COLUMN writes it (never a key field -- plan_alter refuses).
std::string field_decl(char q, const ColumnDef& c) {
    std::string s = quote_ident_with(q, c.name) + " " + c.decl_type;
    if (!c.nullable) s += " NOT NULL";
    if (c.default_sql) s += " DEFAULT " + *c.default_sql;
    return s;
}

bool same_but_name(const ColumnDef& a, const ColumnDef& b) {
    ColumnDef x = a;
    x.name = b.name;
    return x == b;
}

// One field kept by name whose type / required / default changed (s007c).
struct Change {
    const ColumnDef* live = nullptr;
    const ColumnDef* want = nullptr;
    bool type = false;       // the declared type differs: the values are converted
    bool fill = false;       // becomes required with a default: empty records take it
};

// What one design step does to a table, field by field (shared by plan_alter
// and the store's data rule, so the two can't disagree).
struct Diff {
    std::optional<std::pair<std::string, std::string>> rename;   // from, to
    std::vector<const ColumnDef*> dropped;                        // live's
    std::vector<const ColumnDef*> added;                          // want's, in want's order
    std::vector<Change>           changed;                        // in live's order
    std::vector<std::string>      idx_drop;                       // names, live's
    std::vector<const IndexDef*>  idx_make;                       // want's
};

std::expected<Diff, std::string> diff_tables(const TableDef& live, const TableDef& want) {
    Diff df;
    if (live.name != want.name) return std::unexpected("renaming a table is not done here");
    for (const auto& c : live.columns) if (!want.column(c.name)) df.dropped.push_back(&c);
    for (const auto& c : want.columns) if (!live.column(c.name)) df.added.push_back(&c);
    for (const auto& c : live.columns) {
        const ColumnDef* w = want.column(c.name);
        if (!w || *w == c) continue;
        if (c.pk_position > 0 || w->pk_position > 0 || c.auto_increment || w->auto_increment)
            return std::unexpected(curly(c.name) + (c.pk_position > 0 ? " is" : " would be") +
                                   " part of the key -- the key is not changed here yet");
        Change ch{&c, w, c.decl_type != w->decl_type, false};
        ch.fill = !w->nullable && w->default_sql && (c.nullable || ch.type);
        df.changed.push_back(ch);
    }
    if (df.dropped.size() == 1 && df.added.size() == 1 && same_but_name(*df.dropped[0], *df.added[0])) {
        df.rename = {df.dropped[0]->name, df.added[0]->name};
        df.dropped.clear();
        df.added.clear();
    }
    if (df.rename && (!df.dropped.empty() || !df.added.empty() || !df.changed.empty()))
        return std::unexpected("a rename is a step of its own");
    for (const auto* c : df.dropped)
        if (c->pk_position > 0) return std::unexpected(curly(c->name) + " is part of the key -- the key is not changed here");
    for (const auto* c : df.added) {
        if (c->pk_position > 0 || c->auto_increment)
            return std::unexpected(curly(c->name) + " would join the key -- the key is not changed here");
        if (!c->nullable && !c->default_sql)
            return std::unexpected(curly(c->name) + " is required, so it needs a default for the records already there");
    }
    if (want.columns.empty()) return std::unexpected("a table needs at least one field");

    // Indexes: live's, with the rename applied, against want's -- by name.
    auto renamed = [&](IndexDef ix) {
        if (df.rename)
            for (auto& col : ix.columns) if (col == df.rename->first) col = df.rename->second;
        return ix;
    };
    for (const auto& lx : live.indexes) {
        const IndexDef* w = nullptr;
        for (const auto& wx : want.indexes) if (wx.name == lx.name) w = &wx;
        if (w && *w == renamed(lx)) continue;
        if (lower(lx.name).rfind("sqlite_autoindex", 0) == 0)
            return std::unexpected(curly(lx.name) + " belongs to a UNIQUE rule written into the table -- not dropped here");
        df.idx_drop.push_back(lx.name);
    }
    for (const auto& wx : want.indexes) {
        const IndexDef* l = nullptr;
        for (const auto& lx : live.indexes) if (lx.name == wx.name) l = &lx;
        if (l && renamed(*l) == wx) continue;
        if (wx.partial) return std::unexpected("Acorn doesn't make partial indexes");
        if (auto why = check_name("The index", wx.name); !why.empty()) return std::unexpected(why);
        if (wx.columns.empty()) return std::unexpected("the index " + curly(wx.name) + " needs at least one field");
        for (std::size_t i = 0; i < wx.columns.size(); ++i) {
            if (!want.column(wx.columns[i]))
                return std::unexpected("the index " + curly(wx.name) + " uses " + curly(wx.columns[i]) +
                                       ", which the table won't have");
            for (std::size_t k = 0; k < i; ++k)
                if (wx.columns[k] == wx.columns[i])
                    return std::unexpected("the index " + curly(wx.name) + " names " + curly(wx.columns[i]) + " twice");
        }
        df.idx_make.push_back(&wx);
    }
    // An index the table keeps must not use a field that is going.
    for (const auto& wx : want.indexes)
        for (const auto& col : wx.columns)
            if (!want.column(col))
                return std::unexpected("the index " + curly(wx.name) + " uses " + curly(col) + ", which is going");
    return df;
}

}  // namespace

namespace {

// A step of the plan: a statement, or -- for a field whose type changed -- the
// point where Acorn writes each record's value back, converted by its own
// rules (db/Convert). The preview shows that point as a comment line.
struct Stmt {
    std::string sql;
    std::string write_back;   // the field's (want) name; "" = sql is a statement
};

std::string sql_text(const std::string& s) {   // 'it''s'
    std::string out = "'";
    for (char ch : s) { out += ch; if (ch == '\'') out += '\''; }
    return out + "'";
}

std::string write_back_words(char q, const Change& ch, Dialect d) {
    return "-- then each record's " + quote_ident_with(q, ch.want->name) + " is written back as " +
           kind_words(map_type(d, ch.want->decl_type)) + " (Acorn's rules; what won't fit " +
           (ch.want->nullable ? "becomes empty" : "takes the default") + ")";
}

// SQLite can't change a field in place: the table is made again -- new table,
// copy, swap, indexes back -- inside the step's one transaction.
std::vector<Stmt> plan_rebuild(char q, const TableDef& live, const TableDef& want, const Diff& df) {
    std::vector<Stmt> out;
    const std::string tmp = "acorn_rebuild_" + live.name;
    TableDef nt = want;
    nt.name = tmp;
    out.push_back({create_table_sql(Dialect::SQLite, q, nt), {}});
    std::string cols, exprs;
    for (const auto& w : want.columns) {
        const ColumnDef* l = live.column(w.name);
        if (!l) continue;   // a new field: its default / NULL
        const std::string c = quote_ident_with(q, w.name);
        bool fill = false;
        for (const auto& ch : df.changed) if (ch.want == &w && ch.fill) fill = true;
        cols  += (cols.empty() ? "" : ", ") + c;
        exprs += (exprs.empty() ? "" : ", ") + (fill ? "COALESCE(" + c + ", " + *w.default_sql + ")" : c);
    }
    out.push_back({"INSERT INTO " + quote_ident_with(q, tmp) + " (" + cols + ")\n  SELECT " + exprs + " FROM " +
                   quote_ident_with(q, live.name), {}});
    bool counting = false;
    for (const auto& c : live.columns) if (c.pk_position > 0 && c.auto_increment) counting = true;
    if (counting) {   // ids are never reused: the count carries over
        out.push_back({"DELETE FROM sqlite_sequence WHERE name = " + sql_text(tmp), {}});
        out.push_back({"INSERT INTO sqlite_sequence (name, seq)\n  SELECT " + sql_text(tmp) +
                       ", seq FROM sqlite_sequence WHERE name = " + sql_text(live.name), {}});
    }
    out.push_back({drop_table_sql(q, live.name), {}});
    out.push_back({"ALTER TABLE " + quote_ident_with(q, tmp) + " RENAME TO " + quote_ident_with(q, live.name), {}});
    for (const auto& ch : df.changed)
        if (ch.type) out.push_back({write_back_words(q, ch, Dialect::SQLite), ch.want->name});
    for (const auto& ix : want.indexes) out.push_back({create_index_sql(Dialect::SQLite, q, live.name, ix), {}});
    return out;
}

std::expected<std::vector<Stmt>, std::string>
plan_steps(Dialect d, char q, const TableDef& live, const TableDef& want) {
    auto df = diff_tables(live, want);
    if (!df) return std::unexpected(df.error());
    const std::string t = quote_ident_with(q, live.name);
    std::vector<Stmt> out;
    if (d == Dialect::SQLite && !df->changed.empty()) {
        for (const auto& lx : live.indexes)
            if (lower(lx.name).rfind("sqlite_autoindex", 0) == 0)
                return std::unexpected(curly(live.name) + " has a UNIQUE rule written into the table -- Acorn can't "
                                       "make the table again with it yet");
        return plan_rebuild(q, live, want, *df);
    }
    if (d == Dialect::MariaDB) {   // ONE statement: atomic on an engine that can't roll DDL back
        std::vector<std::string> cl;
        for (const auto& n : df->idx_drop) cl.push_back("DROP INDEX " + quote_ident_with(q, n));
        if (df->rename)
            cl.push_back("RENAME COLUMN " + quote_ident_with(q, df->rename->first) + " TO " +
                         quote_ident_with(q, df->rename->second));
        for (const auto* c : df->dropped) cl.push_back("DROP COLUMN " + quote_ident_with(q, c->name));
        for (const auto& ch : df->changed) cl.push_back("MODIFY COLUMN " + field_decl(q, *ch.want));
        for (const auto* c : df->added) cl.push_back("ADD COLUMN " + field_decl(q, *c));
        for (const auto* ix : df->idx_make) {
            std::string a = std::string("ADD ") + (ix->unique ? "UNIQUE " : "") + "INDEX " + quote_ident_with(q, ix->name) + " (";
            for (std::size_t i = 0; i < ix->columns.size(); ++i) a += (i ? ", " : "") + quote_ident_with(q, ix->columns[i]);
            cl.push_back(a + ")");
        }
        if (cl.empty()) return out;
        std::string s = "ALTER TABLE " + t;
        for (std::size_t i = 0; i < cl.size(); ++i) s += (i ? ",\n  " : "\n  ") + cl[i];
        out.push_back({s, {}});
        // The engine casts; Acorn then puts its own reading where they differ.
        for (const auto& ch : df->changed)
            if (ch.type) out.push_back({write_back_words(q, ch, d), ch.want->name});
        return out;
    }
    for (const auto& n : df->idx_drop) out.push_back({drop_index_sql(d, q, live.name, n), {}});
    if (df->rename)
        out.push_back({"ALTER TABLE " + t + " RENAME COLUMN " + quote_ident_with(q, df->rename->first) + " TO " +
                       quote_ident_with(q, df->rename->second), {}});
    for (const auto* c : df->dropped) out.push_back({"ALTER TABLE " + t + " DROP COLUMN " + quote_ident_with(q, c->name), {}});
    // PostgreSQL (SQLite never gets here with a change): ALTER COLUMN, one
    // clause per statement, in an order every engine state allows -- a type
    // change empties the field first (USING NULL) and Acorn writes each value
    // back, converted; required comes last, after the empties are filled.
    for (const auto& ch : df->changed) {
        const ColumnDef& l = *ch.live;
        const ColumnDef& w = *ch.want;
        const std::string c = quote_ident_with(q, w.name);
        const std::string a = "ALTER TABLE " + t + " ALTER COLUMN " + c + " ";
        if (ch.type) {
            if (l.default_sql) out.push_back({a + "DROP DEFAULT", {}});
            if (!l.nullable) out.push_back({a + "DROP NOT NULL", {}});
            out.push_back({a + "TYPE " + w.decl_type + " USING NULL::" + w.decl_type, {}});
            out.push_back({write_back_words(q, ch, d), w.name});
            if (w.default_sql) out.push_back({a + "SET DEFAULT " + *w.default_sql, {}});
        } else if (l.default_sql != w.default_sql) {
            out.push_back({a + (w.default_sql ? "SET DEFAULT " + *w.default_sql : std::string("DROP DEFAULT")), {}});
        }
        if (ch.fill) out.push_back({"UPDATE " + t + " SET " + c + " = " + *w.default_sql + " WHERE " + c + " IS NULL", {}});
        if (!w.nullable && (l.nullable || ch.type)) out.push_back({a + "SET NOT NULL", {}});
        else if (w.nullable && !l.nullable && !ch.type) out.push_back({a + "DROP NOT NULL", {}});
    }
    for (const auto* c : df->added) out.push_back({"ALTER TABLE " + t + " ADD COLUMN " + field_decl(q, *c), {}});
    for (const auto* ix : df->idx_make) out.push_back({create_index_sql(d, q, live.name, *ix), {}});
    return out;
}

// Why SQLite can't make `table` again faithfully ("" = it can): the rebuild
// writes the table from its picture, so anything the picture doesn't carry --
// a link, a CHECK, a COLLATE, a view or trigger naming it -- would be lost or
// broken. Refused in words instead.
std::string rebuild_blocker(Connection& c, const std::string& table) {
    auto r = c.query("SELECT type, name, tbl_name, sql FROM sqlite_master WHERE sql IS NOT NULL");
    if (!r) return "could not read the schema: " + r.error().message;
    auto strip = [](const std::string& s) {   // quoted names and strings out, upper-cased
        std::string out;
        char in = 0;
        for (char ch : s) {
            if (in) { if (ch == in) in = 0; continue; }
            if (ch == '"' || ch == '\'' || ch == '`' || ch == '[') { in = ch == '[' ? ']' : ch; out += ' '; continue; }
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return out;
    };
    for (const auto& row : r->rows) {
        const std::string type = row[0].as_text() ? *row[0].as_text() : "";
        const std::string name = row[1].as_text() ? *row[1].as_text() : "";
        const std::string tbl = row[2].as_text() ? *row[2].as_text() : "";
        const std::string sql = row[3].as_text() ? *row[3].as_text() : "";
        if (type == "table" && name == table) {
            const std::string s = strip(sql);
            for (const char* word : {"CHECK", "COLLATE", "REFERENCES", "UNIQUE", "GENERATED", "CONSTRAINT",
                                     "WITHOUT ROWID", " AS "})
                if (s.find(word) != std::string::npos)
                    return curly(table) + " was made with rules Acorn doesn't write (" + std::string(word) +
                           ") -- it can't make the table again without losing them";
            bool alias = false;
            if (auto sch = c.introspect(); sch && sch->table(table))
                for (const auto& col : sch->table(table)->columns)
                    if (col.pk_position > 0 && col.auto_increment) alias = true;
            if (alias && s.find("AUTOINCREMENT") == std::string::npos)
                return curly(table) + "'s key counts without AUTOINCREMENT -- Acorn can't make the table again yet";
            continue;
        }
        if (type == "trigger" && tbl == table)
            return "the trigger " + curly(name) + " belongs to " + curly(table) + " -- Acorn can't make the table again yet";
        if (type == "view" && sql.find(table) != std::string::npos)
            return "the view " + curly(name) + " reads " + curly(table) + " -- Acorn can't make the table again yet";
        if (type == "table" && name != table && strip(sql).find("REFERENCES") != std::string::npos &&
            sql.find(table) != std::string::npos)
            return curly(name) + " links to " + curly(table) + " -- Acorn can't make the table again yet";
    }
    return {};
}

}  // namespace

std::expected<std::vector<std::string>, std::string>
plan_alter(Dialect d, char q, const TableDef& live, const TableDef& want) {
    auto steps = plan_steps(d, q, live, want);
    if (!steps) return std::unexpected(steps.error());
    std::vector<std::string> out;
    for (auto& s : *steps) out.push_back(std::move(s.sql));
    return out;
}

std::string check_name(const std::string& what, const std::string& name) {
    if (name.empty()) return what + " needs a name";
    if (std::isspace(static_cast<unsigned char>(name.front())) ||
        std::isspace(static_cast<unsigned char>(name.back())))
        return what + " " + curly(name) + " starts or ends with a space";
    for (unsigned char ch : name)
        if (ch < 0x20 || ch == 0x7f) return what + " " + curly(name) + " has a control character in it";
    if (name.size() > 64) return what + " " + curly(name) + " is longer than 64 bytes";
    return {};
}

std::string check_new_table(const TableDef& t, const Schema& existing) {
    if (auto why = check_name("The table", t.name); !why.empty()) return why;
    if (lower(t.name).rfind("sqlite_", 0) == 0) return "Names starting with sqlite_ belong to SQLite";
    for (const auto& x : existing.tables)
        if (lower(x.name) == lower(t.name))
            return std::string("There is already a ") + (x.kind == Table::Kind::View ? "view" : "table") +
                   " named " + curly(x.name);
    if (t.columns.empty()) return "The table needs at least one column";
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        if (auto why = check_name("Column " + std::to_string(i + 1), t.columns[i].name); !why.empty())
            return why;
        for (std::size_t k = 0; k < i; ++k)
            if (lower(t.columns[k].name) == lower(t.columns[i].name))
                return "Two columns are named " + curly(t.columns[i].name);
    }
    return {};
}

// ── the gate ────────────────────────────────────────────────────────────────
bool is_read(const std::string& sql) {
    std::size_t i = 0;
    const auto n = sql.size();
    // Skip leading space and comments.
    for (;;) {
        while (i < n && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
        if (sql.compare(i, 2, "--") == 0) { while (i < n && sql[i] != '\n') ++i; continue; }
        if (sql.compare(i, 2, "/*") == 0) {
            auto e = sql.find("*/", i + 2);
            i = e == std::string::npos ? n : e + 2;
            continue;
        }
        break;
    }
    std::size_t j = i;
    while (j < n && std::isalpha(static_cast<unsigned char>(sql[j]))) ++j;
    const std::string w = upper(sql.substr(i, j - i));
    if (w == "SELECT" || w == "SHOW" || w == "EXPLAIN" || w == "DESCRIBE" || w == "DESC") return true;
    if (w == "PRAGMA") return sql.find('=', j) == std::string::npos;
    return false;
}

std::expected<Result, Error> Gate::query(const std::string& sql, const std::vector<Value>& params) {
    if (!is_read(sql) && !(m_door && m_door())) {
        ++m_strays;
        return std::unexpected(Error{Error::Code::ReadOnly,
            "refused: a write outside the change journal (" + sql.substr(0, 40) + ")"});
    }
    return m_inner->query(sql, params);
}

// ── the store ───────────────────────────────────────────────────────────────
const Value* RowDef::field(const std::string& column) const {
    for (std::size_t i = 0; i < columns.size() && i < values.size(); ++i)
        if (columns[i] == column) return &values[i];
    return nullptr;
}

// Key parts, each self-delimiting: i<int>;  r<real>;  s<bytes>:<text>
Store::Id Store::row_id(const std::string& table, const std::vector<db::Value>& key) {
    if (key.empty()) return {};
    std::string k;
    for (const auto& v : key) {
        if (auto i = v.as_int(); i && v.kind() == db::Value::Kind::Integer) k += "i" + std::to_string(*i) + ";";
        else if (v.kind() == db::Value::Kind::Real) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", *v.as_real());
            k += std::string("r") + buf + ";";
        } else if (const auto* t = v.as_text()) k += "s" + std::to_string(t->size()) + ":" + *t;
        else return {};   // NULL or a blob: not a key Acorn writes by
    }
    return "row:" + std::to_string(table.size()) + ":" + table + ":" + k;
}

bool Store::parse_row_id(const Id& id, std::string& table, std::vector<db::Value>& key) {
    if (id.rfind("row:", 0) != 0) return false;
    std::size_t i = 4;
    auto num = [&](char end, std::size_t& out) {
        std::size_t j = id.find(end, i);
        if (j == std::string::npos || j == i) return false;
        for (std::size_t k = i; k < j; ++k) if (!std::isdigit(static_cast<unsigned char>(id[k]))) return false;
        out = std::stoull(id.substr(i, j - i));
        i = j + 1;
        return true;
    };
    std::size_t n = 0;
    if (!num(':', n) || i + n + 1 > id.size() || id[i + n] != ':') return false;
    table = id.substr(i, n);
    i += n + 1;
    key.clear();
    while (i < id.size()) {
        const char tag = id[i++];
        if (tag == 'i' || tag == 'r') {
            std::size_t j = id.find(';', i);
            if (j == std::string::npos) return false;
            const std::string w = id.substr(i, j - i);
            try {
                if (tag == 'i') key.emplace_back(static_cast<std::int64_t>(std::stoll(w)));
                else key.emplace_back(std::stod(w));
            } catch (...) { return false; }
            i = j + 1;
        } else if (tag == 's') {
            std::size_t len = 0;
            if (!num(':', len) || i + len > id.size()) return false;
            key.emplace_back(id.substr(i, len));
            i += len;
        } else {
            return false;
        }
    }
    return !key.empty();
}

Store::Id Store::field_id(const std::string& table, const std::string& column) {
    return "field:" + std::to_string(table.size()) + ":" + table + ":" + column;
}

bool Store::parse_field_id(const Id& id, std::string& table, std::string& column) {
    if (id.rfind("field:", 0) != 0) return false;
    const std::size_t c = id.find(':', 6);
    if (c == std::string::npos || c == 6) return false;
    std::size_t n = 0;
    for (std::size_t k = 6; k < c; ++k) {
        if (!std::isdigit(static_cast<unsigned char>(id[k]))) return false;
        n = n * 10 + static_cast<std::size_t>(id[k] - '0');
        if (n > id.size()) return false;
    }
    if (c + 1 + n >= id.size() || id[c + 1 + n] != ':') return false;
    table  = id.substr(c + 1, n);
    column = id.substr(c + 2 + n);
    return !table.empty() && !column.empty();
}

std::string Store::describe(const Id& id) {
    std::string table;
    std::vector<db::Value> key;
    if (parse_row_id(id, table, key)) {
        std::string k;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (i) k += ", ";
            const auto* t = key[i].as_text();
            k += t ? *t : key[i].to_display();
        }
        return curly(table) + " record " + k;
    }
    if (id.rfind("new:", 0) == 0) return "a new record in " + curly(id.substr(4));
    std::string column;
    if (parse_field_id(id, table, column)) return "the values of " + curly(table) + "." + curly(column);
    return curly(name_of(id));
}

const Table* Store::table_info(const std::string& name) const {
    if (!m_schema) {
        auto s = m_conn.introspect();
        if (!s) { fail(s.error().code, s.error().message); return nullptr; }
        m_schema = std::move(*s);
    }
    return m_schema->table(name);
}

std::string Store::why_not_writable(const Table& t) {
    if (t.kind == Table::Kind::View) return curly(t.name) + " is a view -- its records are read here, not written";
    if (t.primary_key().empty())
        return curly(t.name) + " has no primary key -- Acorn finds a record by its key, so it only reads this table";
    return {};
}

std::string Store::where_key(const Table& t) const {
    std::string w;
    for (const auto& k : t.primary_key()) w += (w.empty() ? " WHERE " : " AND ") + m_conn.quote_ident(k) + " = ?";
    return w;
}

std::optional<Record> Store::get(const Id& id) const {
    if (id.rfind("table:", 0) == 0) return get_table(name_of(id));
    if (id.rfind("row:", 0) == 0) return get_row(id);
    if (id.rfind("field:", 0) == 0) return get_field(id);
    return std::nullopt;   // "new:..." -- a declaration, never a record
}

std::optional<Record> Store::get_table(const std::string& name) const {
    m_schema.reset();   // a table's shape is always read fresh
    const Table* t = table_info(name);
    if (failed() || !t || t->kind != Table::Kind::Table) return std::nullopt;
    TableDef d = TableDef::from(*t);
    auto n = m_conn.query("SELECT COUNT(*) FROM " + m_conn.quote_ident(name));
    if (!n || n->rows.empty()) { fail(Error::Code::Internal, "could not count the rows of " + curly(name)); return d; }
    const auto& v = n->rows[0][0];
    if (auto i = v.as_int()) d.rows = *i;
    else if (auto txt = v.as_text()) d.rows = std::stoll(*txt);
    return d;
}

std::optional<Record> Store::get_row(const Id& id) const {
    std::string table;
    std::vector<db::Value> key;
    if (!parse_row_id(id, table, key)) { fail(Error::Code::Internal, "not a record: " + id); return std::nullopt; }
    const Table* t = table_info(table);
    if (failed() || !t || t->kind != Table::Kind::Table) return std::nullopt;
    if (t->primary_key().size() != key.size()) return std::nullopt;   // the key changed shape: not this record
    std::string sql = "SELECT ";
    for (std::size_t i = 0; i < t->columns.size(); ++i)
        sql += (i ? ", " : "") + m_conn.quote_ident(t->columns[i].name);
    sql += " FROM " + m_conn.quote_ident(table) + where_key(*t);
    auto r = m_conn.query(sql, key);
    if (!r) { fail(r.error().code, r.error().message); return std::nullopt; }
    if (r->rows.empty()) return std::nullopt;
    if (r->rows.size() > 1) { fail(Error::Code::Internal, describe(id) + " matches more than one row"); return std::nullopt; }
    RowDef d;
    d.table = table;
    for (const auto& c : t->columns) d.columns.push_back(c.name);
    d.values = r->rows[0];
    return d;
}

std::optional<Record> Store::get_field(const Id& id) const {
    std::string table, column;
    if (!parse_field_id(id, table, column)) { fail(Error::Code::Internal, "not a field: " + id); return std::nullopt; }
    m_schema.reset();   // a field may have come or gone this step
    const Table* t = table_info(table);
    if (failed() || !t || t->kind != Table::Kind::Table || !t->column(column)) return std::nullopt;
    const auto pk = t->primary_key();
    const std::string c = m_conn.quote_ident(column);
    FieldData d{table, column, {}, {}};
    if (pk.empty()) {
        auto n = m_conn.query("SELECT COUNT(*) FROM " + m_conn.quote_ident(table) + " WHERE " + c + " IS NOT NULL");
        if (!n || n->rows.empty()) { fail(Error::Code::Internal, "could not count " + describe(id)); return std::nullopt; }
        const auto& v = n->rows[0][0];
        const std::int64_t k = v.as_int() ? *v.as_int() : (v.as_text() ? std::stoll(*v.as_text()) : 0);
        if (k > 0) fail(Error::Code::Unsupported, curly(table) + " has no primary key -- Acorn can't keep " +
                        curly(column) + "'s values to bring them back");
        return d;
    }
    std::string sql = "SELECT ", order;
    for (std::size_t i = 0; i < pk.size(); ++i) {
        sql += m_conn.quote_ident(pk[i]) + ", ";
        order += (i ? ", " : "") + m_conn.quote_ident(pk[i]);
    }
    sql += c + " FROM " + m_conn.quote_ident(table) + " WHERE " + c + " IS NOT NULL ORDER BY " + order;
    auto r = m_conn.query(sql);
    if (!r) { fail(r.error().code, r.error().message); return std::nullopt; }
    for (auto& row : r->rows) {
        d.values.push_back(row.back());
        row.pop_back();
        d.keys.push_back(std::move(row));
    }
    return d;
}

void Store::put_field(const Id& id, const FieldData* v) {
    // Absent: the field going is the TABLE's change (Delete Field declares
    // both); there is nothing to write here.
    if (!v) return;
    std::string table, column;
    if (!parse_field_id(id, table, column)) { fail(Error::Code::Internal, "not a field: " + id); return; }
    if (v->table != table || v->column != column) { fail(Error::Code::Internal, id + " given another field's values"); return; }
    m_schema.reset();
    const Table* t = table_info(table);
    if (failed()) return;
    if (!t || !t->column(column)) { fail(Error::Code::Changed, curly(table) + "." + curly(column) + " is not there"); return; }
    const auto pk = t->primary_key();
    if (pk.empty()) {
        if (!v->values.empty()) fail(Error::Code::Unsupported, curly(table) + " has no primary key -- values can't be put back");
        return;
    }
    const auto got = get_field(id);
    if (failed()) return;
    const FieldData live = got ? std::get<FieldData>(*got) : FieldData{table, column, {}, {}};
    if (live == *v) return;
    // s007c: values kept from another kind of field (redo of a type change puts
    // the converted values back BEFORE the table changes kind again) go in as
    // this field's kind, by Acorn's rules; a value already of its kind as is.
    FieldData adapted = *v;
    const FieldType kind = t->column(column)->type;
    for (auto& val : adapted.values)
        if (!native_to(val, kind))
            if (auto c = convert_value(val, FieldType::Unknown, kind)) val = *c;
    v = &adapted;
    auto set_one = [&](const std::vector<db::Value>& key, const db::Value& val) {
        std::vector<db::Value> params{val};
        params.insert(params.end(), key.begin(), key.end());
        auto r = m_conn.query("UPDATE " + m_conn.quote_ident(table) + " SET " + m_conn.quote_ident(column) + " = ?" +
                              where_key(*t), params);
        if (!r) { fail(r.error().code, r.error().message); return false; }
        if (r->affected != 1) {
            fail(Error::Code::Changed, describe(row_id(table, key)) + " is not there any more -- " +
                 curly(column) + "'s values were not put back");
            return false;
        }
        return true;
    };
    auto find = [](const FieldData& f, const std::vector<db::Value>& key) -> const db::Value* {
        for (std::size_t i = 0; i < f.keys.size(); ++i) if (f.keys[i] == key) return &f.values[i];
        return nullptr;
    };
    for (std::size_t i = 0; i < live.keys.size(); ++i)
        if (!find(*v, live.keys[i]) && !set_one(live.keys[i], db::Value())) return;
    for (std::size_t i = 0; i < v->keys.size(); ++i) {
        const db::Value* now = find(live, v->keys[i]);
        if (now && *now == v->values[i]) continue;
        if (!set_one(v->keys[i], v->values[i])) return;
    }
}

std::int64_t Store::count_where(const std::string& table, const std::string& column, const char* cond) const {
    auto n = m_conn.query("SELECT COUNT(*) FROM " + m_conn.quote_ident(table) + " WHERE " +
                          m_conn.quote_ident(column) + " " + cond);
    if (!n || n->rows.empty()) return -1;
    const auto& v = n->rows[0][0];
    if (auto i = v.as_int()) return *i;
    if (auto t = v.as_text()) return std::stoll(*t);
    return -1;
}

std::vector<std::string> Store::primary_key_of(const std::string& table) const {
    const Table* t = table_info(table);
    return t ? t->primary_key() : std::vector<std::string>{};
}

// s007c: after a field's type changed, each record's value as Acorn converts
// it (db/Convert) -- written only where the field doesn't already hold it.
// What won't fit becomes empty, or the default for a required field.
bool Store::write_back(const std::string& table, const ColumnDef& from, const ColumnDef& to, const FieldData& before) {
    m_schema.reset();
    const Table* t = table_info(table);
    if (failed()) return false;
    if (!t || !t->column(to.name)) { fail(Error::Code::Changed, curly(table) + "." + curly(to.name) + " is not there"); return false; }
    const FieldType ft = map_type(m_dialect, from.decl_type), tt = map_type(m_dialect, to.decl_type);
    const auto now = get_field(field_id(table, to.name));
    if (failed()) return false;
    const FieldData live = now ? std::get<FieldData>(*now) : FieldData{table, to.name, {}, {}};
    auto find = [](const FieldData& f, const std::vector<db::Value>& key) -> const db::Value* {
        for (std::size_t i = 0; i < f.keys.size(); ++i) if (f.keys[i] == key) return &f.values[i];
        return nullptr;
    };
    std::int64_t misfits = 0;
    for (const auto& v : before.values) if (!convert_value(v, ft, tt)) ++misfits;
    if (misfits > 0 && !to.nullable && !to.default_sql) {
        fail(Error::Code::Constraint, std::to_string(misfits) + " of " + curly(to.name) + "'s values won't fit " +
             kind_words(tt) + ", and a required field needs a default for them");
        return false;
    }
    const std::string c = m_conn.quote_ident(to.name);
    for (std::size_t i = 0; i < before.keys.size(); ++i) {
        const auto& key = before.keys[i];
        const auto conv = convert_value(before.values[i], ft, tt);
        const db::Value* cur = find(live, key);
        std::string set;
        std::vector<db::Value> params;
        if (conv) {
            if (cur && *cur == *conv) continue;
            set = c + " = ?";
            params.push_back(*conv);
        } else if (to.nullable) {
            if (!cur) continue;
            set = c + " = NULL";
        } else {
            set = c + " = " + *to.default_sql;
        }
        params.insert(params.end(), key.begin(), key.end());
        auto r = m_conn.query("UPDATE " + m_conn.quote_ident(table) + " SET " + set + where_key(*t), params);
        if (!r) { fail(r.error().code, r.error().message); return false; }
        if (r->affected > 1) {
            fail(Error::Code::Changed, describe(row_id(table, key)) + " matches more than one record");
            return false;
        }
    }
    return true;
}

bool Store::exec_all(const std::vector<std::string>& sql) {
    for (const auto& s : sql) if (!exec(s)) return false;
    return true;
}

bool Store::exec(const std::string& sql) {
    auto r = m_conn.query(sql);
    if (!r) fail(r.error().code, r.error().message);
    return r.has_value();
}

bool Store::door_and_declared(const Id& id) {
    if (failed()) return false;   // one refusal ends the step
    if (!(m_door && m_door())) {
        ++m_strays;
        fail(Error::Code::ReadOnly, "refused: a write outside the change journal");
        return false;
    }
    if (std::find(m_allowed.begin(), m_allowed.end(), id) == m_allowed.end()) {
        fail(Error::Code::Internal, "refused: the step did not declare " + id);
        return false;
    }
    return true;
}

void Store::put(const Id& id, const std::optional<Record>& v) {
    if (id.rfind("new:", 0) == 0) return;   // a declaration: nothing to put
    if (!door_and_declared(id)) return;
    if (id.rfind("table:", 0) == 0) {
        if (v && !std::holds_alternative<TableDef>(*v)) { fail(Error::Code::Internal, id + " given a record"); return; }
        put_table(id, v ? std::optional<TableDef>(std::get<TableDef>(*v)) : std::nullopt);
        return;
    }
    if (id.rfind("row:", 0) == 0) {
        if (v && !std::holds_alternative<RowDef>(*v)) { fail(Error::Code::Internal, id + " given a table"); return; }
        put_row(id, v ? &std::get<RowDef>(*v) : nullptr);
        return;
    }
    if (id.rfind("field:", 0) == 0) {
        if (v && !std::holds_alternative<FieldData>(*v)) { fail(Error::Code::Internal, id + " given a table or record"); return; }
        put_field(id, v ? &std::get<FieldData>(*v) : nullptr);
        return;
    }
    fail(Error::Code::Internal, "unknown record " + id);
}

void Store::put_table(const Id& id, const std::optional<TableDef>& v) {
    const std::string name = name_of(id);
    const auto got = get_table(name);
    if (failed()) return;
    const std::optional<TableDef> live = got ? std::optional<TableDef>(std::get<TableDef>(*got)) : std::nullopt;
    if (live == v) return;
    const char q = m_conn.quote_ident("x").front();
    m_schema.reset();   // whatever happens next, the shape changes

    if (!v) {   // present -> absent
        if (live->rows > 0) {
            fail(Error::Code::Constraint, curly(name) + " holds " + std::to_string(live->rows) +
                 " row" + (live->rows == 1 ? "" : "s") + " -- Acorn will not drop a table with data in it");
            return;
        }
        exec(drop_table_sql(q, name));
        return;
    }
    if (v->name != name) { fail(Error::Code::Internal, "record " + id + " does not hold table " + v->name); return; }
    if (live) {   // present -> another shape: ALTER (s007)
        auto df = diff_tables(*live, *v);
        if (!df) { fail(Error::Code::Unsupported, df.error()); return; }
        // The data rule: a field holding values is dropped only when the step
        // keeps them (declared its field id) -- else it is refused.
        for (const auto* c : df->dropped) {
            const Id fid = field_id(name, c->name);
            if (std::find(m_allowed.begin(), m_allowed.end(), fid) != m_allowed.end()) continue;
            auto n = m_conn.query("SELECT COUNT(*) FROM " + m_conn.quote_ident(name) + " WHERE " +
                                  m_conn.quote_ident(c->name) + " IS NOT NULL");
            if (!n || n->rows.empty()) { fail(Error::Code::Internal, "could not count " + curly(c->name) + "'s values"); return; }
            const auto& cv = n->rows[0][0];
            const std::int64_t k = cv.as_int() ? *cv.as_int() : (cv.as_text() ? std::stoll(*cv.as_text()) : 0);
            if (k > 0) {
                fail(Error::Code::Constraint, curly(c->name) + " holds " + std::to_string(k) + " value" +
                     (k == 1 ? "" : "s") + " -- Acorn will not drop a field with data in it unless the step keeps them");
                return;
            }
        }
        // s007c -- a field whose type / required / default changed. Its
        // values change with it (converted; empties filled), so a field holding
        // any is changed only when the step keeps them -- the same rule.
        std::vector<std::pair<const Change*, FieldData>> before;   // type changes: the values as they were
        for (const auto& ch : df->changed) {
            const std::string& cn = ch.live->name;
            const std::int64_t held = count_where(name, cn, "IS NOT NULL");
            const std::int64_t empty = ch.fill || (!ch.want->nullable && ch.live->nullable)
                                           ? count_where(name, cn, "IS NULL") : 0;
            if (held < 0 || empty < 0) { fail(Error::Code::Internal, "could not count " + curly(cn) + "'s values"); return; }
            if (!ch.want->nullable && ch.live->nullable && empty > 0 && !ch.want->default_sql) {
                fail(Error::Code::Constraint, std::to_string(empty) + " record" + (empty == 1 ? " has" : "s have") +
                     " no " + curly(cn) + " -- a required field needs a default for " +
                     (empty == 1 ? "it (or fill it first)" : "them (or fill them first)"));
                return;
            }
            const bool keeps = std::find(m_allowed.begin(), m_allowed.end(), field_id(name, cn)) != m_allowed.end();
            if (!keeps && ((ch.type && held > 0) || empty > 0)) {
                fail(Error::Code::Constraint, "changing " + curly(cn) + " changes the values it holds -- Acorn does "
                     "that only when the step keeps them");
                return;
            }
            if (m_dialect == Dialect::MariaDB && empty > 0) {
                fail(Error::Code::Unsupported, "this engine can't roll a table change back, so Acorn won't fill " +
                     curly(cn) + "'s empty records here -- fill them first");
                return;
            }
            if (ch.type) {
                const auto got = get_field(field_id(name, cn));
                if (failed()) return;
                before.push_back({&ch, got ? std::get<FieldData>(*got) : FieldData{name, cn, {}, {}}});
                if (!before.back().second.keys.empty() && primary_key_of(name).empty()) {
                    fail(Error::Code::Unsupported, curly(name) + " has no primary key -- Acorn can't write " +
                         curly(cn) + "'s values back");
                    return;
                }
            }
        }
        if (!df->changed.empty() && m_dialect == Dialect::SQLite)
            if (auto why = rebuild_blocker(m_conn, name); !why.empty()) { fail(Error::Code::Unsupported, why); return; }
        auto plan = plan_steps(m_dialect, q, *live, *v);
        if (!plan) { fail(Error::Code::Unsupported, plan.error()); return; }
        // MariaDB: the engine casts in its one statement -- a value Acorn reads
        // as not fitting would be emptied with no way back. Refused first.
        if (m_dialect == Dialect::MariaDB)
            for (const auto& [ch, vals] : before)
                for (const auto& val : vals.values)
                    if (!convert_value(val, map_type(m_dialect, ch->live->decl_type), map_type(m_dialect, ch->want->decl_type))) {
                        fail(Error::Code::Unsupported, "this engine can't roll a table change back, so Acorn won't empty " +
                             curly(ch->live->name) + "'s values that don't fit " +
                             kind_words(map_type(m_dialect, ch->want->decl_type)) + " -- fix those records first");
                        return;
                    }
        for (const auto& st : *plan) {
            if (st.write_back.empty()) { if (!exec(st.sql)) return; continue; }
            for (const auto& [ch, vals] : before)
                if (ch->want->name == st.write_back && !write_back(name, *ch->live, *ch->want, vals)) return;
        }
        return;
    }
    if (v->rows > 0) {
        fail(Error::Code::Unsupported, "a table's rows can't be put back yet");
        return;
    }
    if (!exec(create_table_sql(m_dialect, q, *v))) return;
    for (const auto& ix : v->indexes) {
        if (ix.partial) { fail(Error::Code::Unsupported, "Acorn doesn't make partial indexes"); return; }
        if (!exec(create_index_sql(m_dialect, q, v->name, ix))) return;
    }
}

void Store::put_row(const Id& id, const RowDef* v) {
    std::string table;
    std::vector<db::Value> key;
    if (!parse_row_id(id, table, key)) { fail(Error::Code::Internal, "not a record: " + id); return; }
    const Table* t = table_info(table);
    if (failed()) return;
    if (!t) { fail(Error::Code::Changed, curly(table) + " is not there any more"); return; }
    if (auto why = why_not_writable(*t); !why.empty()) { fail(Error::Code::Unsupported, why); return; }
    const auto got = get_row(id);
    if (failed()) return;
    const RowDef* live = got ? &std::get<RowDef>(*got) : nullptr;
    if (!live && !v) return;
    if (live && v && *live == *v) return;
    const auto pk = t->primary_key();

    if (!v) {   // present -> absent
        auto r = m_conn.query("DELETE FROM " + m_conn.quote_ident(table) + where_key(*t), key);
        if (!r) { fail(r.error().code, r.error().message); return; }
        if (r->affected != 1) fail(Error::Code::Changed, describe(id) + ": the delete touched " +
                                   std::to_string(r->affected) + " rows, not 1");
        return;
    }
    if (v->table != table) { fail(Error::Code::Internal, id + " given a record of " + v->table); return; }
    // The picture must be this record: its key fields hold this key.
    for (std::size_t i = 0; i < pk.size(); ++i) {
        const auto* f = v->field(pk[i]);
        if (!f || !(*f == key[i])) { fail(Error::Code::Unsupported, "a record's key field is not changed here"); return; }
    }
    // ... and the table must still have exactly the columns it was taken with.
    bool same = t->columns.size() == v->columns.size();
    for (std::size_t i = 0; same && i < v->columns.size(); ++i) same = t->columns[i].name == v->columns[i];
    if (!same) { fail(Error::Code::Changed, curly(table) + "'s columns changed since -- nothing was written"); return; }

    if (!live) {   // absent -> present: every field, the key included
        std::string cols, marks;
        for (std::size_t i = 0; i < v->columns.size(); ++i) {
            cols  += (i ? ", " : "") + m_conn.quote_ident(v->columns[i]);
            marks += i ? ", ?" : "?";
        }
        auto r = m_conn.query("INSERT INTO " + m_conn.quote_ident(table) + " (" + cols + ") VALUES (" + marks + ")",
                              v->values);
        if (!r) fail(r.error().code, r.error().message);
        return;
    }
    // changed: only the fields that differ
    std::string set;
    std::vector<db::Value> params;
    for (std::size_t i = 0; i < v->columns.size(); ++i) {
        if (live->values[i] == v->values[i]) continue;
        set += (set.empty() ? "" : ", ") + m_conn.quote_ident(v->columns[i]) + " = ?";
        params.push_back(v->values[i]);
    }
    for (const auto& k : key) params.push_back(k);
    auto r = m_conn.query("UPDATE " + m_conn.quote_ident(table) + " SET " + set + where_key(*t), params);
    if (!r) { fail(r.error().code, r.error().message); return; }
    // MariaDB reports rows CHANGED, not matched; a value equal to itself was
    // filtered above, so 1 is right on every engine.
    if (r->affected != 1) fail(Error::Code::Changed, describe(id) + ": the change touched " +
                               std::to_string(r->affected) + " rows, not 1");
}

Store::Id Store::insert(const std::string& table, const std::vector<std::string>& columns,
                        const std::vector<db::Value>& values) {
    if (!door_and_declared(new_row_id(table))) return {};
    const Table* t = table_info(table);
    if (failed()) return {};
    if (!t) { fail(Error::Code::NotFound, "there is no table " + curly(table)); return {}; }
    if (auto why = why_not_writable(*t); !why.empty()) { fail(Error::Code::Unsupported, why); return {}; }
    if (columns.size() != values.size()) { fail(Error::Code::Internal, "columns and values don't pair up"); return {}; }
    const auto pk = t->primary_key();

    std::string sql = "INSERT INTO " + m_conn.quote_ident(table);
    if (columns.empty()) {
        sql += m_dialect == Dialect::MariaDB ? " () VALUES ()" : " DEFAULT VALUES";
    } else {
        std::string cols, marks;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (!t->column(columns[i])) { fail(Error::Code::NotFound, curly(table) + " has no column " + curly(columns[i])); return {}; }
            cols  += (i ? ", " : "") + m_conn.quote_ident(columns[i]);
            marks += i ? ", ?" : "?";
        }
        sql += " (" + cols + ") VALUES (" + marks + ")";
    }
    if (m_caps.returning) {
        sql += " RETURNING ";
        for (std::size_t i = 0; i < pk.size(); ++i) sql += (i ? ", " : "") + m_conn.quote_ident(pk[i]);
    }
    auto r = m_conn.query(sql, values);
    if (!r) { fail(r.error().code, r.error().message); return {}; }

    // The made record's key: RETURNING; else the engine's last insert id for a
    // one-column counting key; else the values given.
    std::vector<db::Value> key;
    if (m_caps.returning && r->rows.size() == 1 && r->rows[0].size() == pk.size()) {
        key = r->rows[0];
    } else {
        for (const auto& k : pk) {
            auto it = std::find(columns.begin(), columns.end(), k);
            if (it != columns.end()) key.push_back(values[static_cast<std::size_t>(it - columns.begin())]);
            else if (pk.size() == 1 && r->last_insert_id) key.emplace_back(*r->last_insert_id);
            else { fail(Error::Code::Internal, "the new record's key could not be learned"); return {}; }
        }
    }
    const Id id = row_id(table, key);
    if (id.empty()) { fail(Error::Code::Unsupported, "the new record's key is NULL or binary"); return {}; }
    m_allowed.push_back(id);
    m_made.push_back(id);
    return id;
}

// ── the journal ─────────────────────────────────────────────────────────────
Journal::Journal(Gate& gate, Dialect d, const Capabilities& caps)
    : m_gate(gate), m_dialect(d), m_caps(caps), m_store(gate, d, caps), m_core(m_store) {
    m_gate.set_door([this] { return open(); });
    m_store.set_door([this] { return open(); });
    m_store.set_settle([this] { return commit(); });
}

Journal::~Journal() { m_gate.set_door({}); }

namespace {
bool touches_tables(const std::vector<Store::Id>& ids) {
    for (const auto& id : ids) if (id.rfind("table:", 0) == 0) return true;
    return false;
}
// Everything a step declares except a field's kept values.
std::size_t not_fields(const std::vector<Store::Id>& ids) {
    std::size_t n = 0;
    for (const auto& id : ids) if (id.rfind("field:", 0) != 0) ++n;
    return n;
}
}  // namespace

std::expected<void, Error> Journal::begin(const std::vector<Store::Id>& ids) {
    if (m_gate.read_only())
        return std::unexpected(Error{Error::Code::ReadOnly, "this database is open read-only"});
    m_store.clear_failed();
    m_store.forget_schema();
    m_txn = true;
    // DDL rolls back with the transaction on SQLite and PostgreSQL. MariaDB
    // would commit implicitly at the first DDL statement -- there a table step
    // is one statement, atomic on its own, and no BEGIN pretends otherwise.
    // Records roll back everywhere.
    if (m_caps.transactional_ddl || !touches_tables(ids)) {
        auto r = m_gate.query(m_dialect == Dialect::MariaDB ? "START TRANSACTION" : "BEGIN");
        if (!r) { m_txn = false; return std::unexpected(r.error()); }
        m_in_txn = true;
    }
    return {};
}

bool Journal::commit() {
    if (!m_in_txn) return true;
    auto r = m_gate.query("COMMIT");
    if (!r) { m_store.refuse(r.error()); return false; }
    m_in_txn = false;
    return true;
}

void Journal::rollback() {
    if (m_in_txn) (void)m_gate.query("ROLLBACK");
    m_in_txn = false;
}

std::expected<void, Error> Journal::run(const std::string& label, const std::vector<Store::Id>& ids,
                                        const std::function<void(Store&)>& op) {
    if (m_txn) return std::unexpected(Error{Error::Code::Internal, "a step is already running"});
    // One table per step where DDL can't roll back. A field's kept values
    // (Delete Field) ride along: put back AFTER the one ALTER on undo.
    if (!m_caps.transactional_ddl && touches_tables(ids) && not_fields(ids) > 1)
        return std::unexpected(Error{Error::Code::Unsupported,
            "this engine can't roll back a table change, so Acorn changes one table per step"});
    if (auto b = begin(ids); !b) return b;
    m_store.set_allowed(ids);
    // A "new:" declaration is not a record: the journal records the made ones.
    std::vector<Store::Id> recs;
    for (const auto& id : ids) if (id.rfind("new:", 0) != 0) recs.push_back(id);
    (void)m_store.take_made();
    m_core.run(label, recs, [&] { op(m_store); return m_store.take_made(); });
    std::expected<void, Error> out;
    if (m_store.failed()) {
        out = std::unexpected(m_store.error());
        m_store.clear_failed();
    }
    rollback();   // nothing if the step committed (or was a no-op that committed)
    m_store.set_allowed({});
    m_txn = false;
    return out;
}

std::expected<void, Error> Journal::verify(const Core::Step& st, bool undoing) {
    m_store.forget_schema();
    for (const auto& e : st.entries) {
        const auto live = m_store.get(e.id);
        if (m_store.failed()) {
            Error err = m_store.error();
            m_store.clear_failed();
            return std::unexpected(err);
        }
        if (live != (undoing ? e.after : e.before))
            return std::unexpected(Error{Error::Code::Changed,
                Store::describe(e.id) + " has changed since " + st.label + " -- not " +
                (undoing ? "undone" : "redone") + "; nothing was touched"});
    }
    return {};
}

std::expected<std::string, Error> Journal::undo() {
    const auto* st = m_core.next_undo();
    if (!st) return std::unexpected(Error{Error::Code::NotFound, "nothing to undo"});
    if (m_txn) return std::unexpected(Error{Error::Code::Internal, "a step is already running"});
    const std::string label = st->label;
    if (auto v = verify(*st, true); !v) return std::unexpected(v.error());
    if (auto b = begin(ids_of(*st)); !b) return std::unexpected(b.error());
    m_store.set_allowed(ids_of(*st));
    const auto r = m_core.undo();
    std::expected<std::string, Error> out = label;
    if (!r.done) {
        out = std::unexpected(m_store.failed() ? m_store.error()
                                               : Error{Error::Code::Internal, "undo did not run"});
        m_store.clear_failed();
    }
    rollback();
    m_store.set_allowed({});
    m_txn = false;
    return out;
}

std::expected<std::string, Error> Journal::redo() {
    const auto* st = m_core.next_redo();
    if (!st) return std::unexpected(Error{Error::Code::NotFound, "nothing to redo"});
    if (m_txn) return std::unexpected(Error{Error::Code::Internal, "a step is already running"});
    const std::string label = st->label;
    if (auto v = verify(*st, false); !v) return std::unexpected(v.error());
    if (auto b = begin(ids_of(*st)); !b) return std::unexpected(b.error());
    m_store.set_allowed(ids_of(*st));
    const auto r = m_core.redo();
    std::expected<std::string, Error> out = label;
    if (!r.done) {
        out = std::unexpected(m_store.failed() ? m_store.error()
                                               : Error{Error::Code::Internal, "redo did not run"});
        m_store.clear_failed();
    }
    rollback();
    m_store.set_allowed({});
    m_txn = false;
    return out;
}

// ── the verbs ───────────────────────────────────────────────────────────────
namespace verbs {

std::expected<void, Error> new_table(Journal& j, const TableDef& t) {
    const auto id = Store::table_id(t.name);
    return j.run("New Table " + curly(t.name), {id}, [&](Store& s) {
        // NEW: anything already there is refused -- even an identical table,
        // which put() alone would treat as "nothing to do".
        if (s.get(id) || s.failed()) {
            if (!s.failed())
                s.refuse(Error{Error::Code::Exists, "there is already a table named " + curly(t.name)});
            return;
        }
        TableDef d = t;
        d.rows = 0;
        s.put(id, d);
    });
}

std::expected<Store::Id, Error> new_record(Journal& j, const std::string& table,
                                           const std::vector<std::string>& columns,
                                           const std::vector<Value>& values) {
    Store::Id made;
    auto r = j.run("New Record in " + curly(table), {Store::new_row_id(table)}, [&](Store& s) {
        made = s.insert(table, columns, values);
    });
    if (!r) return std::unexpected(r.error());
    return made;
}

std::expected<void, Error> set_field(Journal& j, const Store::Id& row, const std::string& column,
                                     const Value& v) {
    return j.run("Edit " + curly(column), {row}, [&](Store& s) {
        auto got = s.get(row);
        if (s.failed()) return;
        if (!got) { s.refuse(Error{Error::Code::Changed, Store::describe(row) + " is not there any more"}); return; }
        RowDef d = std::get<RowDef>(*got);
        if (const Table* t = s.table_info(d.table); t) {
            const Column* c = t->column(column);
            if (c && c->pk_position > 0) {
                s.refuse(Error{Error::Code::Unsupported, curly(column) + " is the record's key -- Acorn finds the record by it, so it is not changed here"});
                return;
            }
        }
        bool found = false;
        for (std::size_t i = 0; i < d.columns.size(); ++i)
            if (d.columns[i] == column) { d.values[i] = v; found = true; }
        if (!found) { s.refuse(Error{Error::Code::NotFound, curly(d.table) + " has no column " + curly(column)}); return; }
        s.put(row, d);
    });
}

std::expected<void, Error> delete_record(Journal& j, const Store::Id& row) {
    std::string table;
    std::vector<Value> key;
    if (!Store::parse_row_id(row, table, key))
        return std::unexpected(Error{Error::Code::Internal, "not a record: " + row});
    return j.run("Delete Record from " + curly(table), {row}, [&](Store& s) {
        if (!s.get(row) && !s.failed()) {
            s.refuse(Error{Error::Code::Changed, Store::describe(row) + " is not there any more"});
            return;
        }
        s.put(row, std::nullopt);
    });
}

// ── s007: the table designer ────────────────────────────────────────────────
namespace {

// The live table, as a picture, inside a step ("" refused already).
std::optional<TableDef> live_table(Store& s, const std::string& table) {
    auto got = s.get(Store::table_id(table));
    if (s.failed()) return std::nullopt;
    if (!got) { s.refuse(Error{Error::Code::NotFound, "there is no table " + curly(table)}); return std::nullopt; }
    return std::get<TableDef>(*got);
}

const ColumnDef* named_like(const TableDef& t, const std::string& n, const std::string& except = {}) {
    for (const auto& c : t.columns)
        if (lower(c.name) == lower(n) && c.name != except) return &c;
    return nullptr;
}

std::int64_t count_values(Connection& c, const std::string& table, const std::string& column) {
    auto n = c.query("SELECT COUNT(*) FROM " + c.quote_ident(table) + " WHERE " + c.quote_ident(column) + " IS NOT NULL");
    if (!n || n->rows.empty()) return -1;
    const auto& v = n->rows[0][0];
    if (auto i = v.as_int()) return *i;
    if (auto t = v.as_text()) return std::stoll(*t);
    return -1;
}

std::int64_t count_null(Connection& c, const std::string& table, const std::string& column) {
    auto n = c.query("SELECT COUNT(*) FROM " + c.quote_ident(table) + " WHERE " + c.quote_ident(column) + " IS NULL");
    if (!n || n->rows.empty()) return -1;
    const auto& v = n->rows[0][0];
    if (auto i = v.as_int()) return *i;
    if (auto t = v.as_text()) return std::stoll(*t);
    return -1;
}

}  // namespace

std::expected<void, Error> new_field(Journal& j, const std::string& table, const ColumnDef& c) {
    if (auto why = check_name("The field", c.name); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto id = Store::table_id(table);
    return j.run("New Field " + curly(c.name) + " in " + curly(table), {id}, [&](Store& s) {
        auto t = live_table(s, table);
        if (!t) return;
        if (const auto* x = named_like(*t, c.name)) {
            s.refuse(Error{Error::Code::Exists, curly(table) + " already has a field named " + curly(x->name)});
            return;
        }
        t->columns.push_back(c);
        s.put(id, *t);
    });
}

std::expected<void, Error> rename_field(Journal& j, const std::string& table, const std::string& from,
                                        const std::string& to) {
    if (from == to) return {};
    if (auto why = check_name("The field", to); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto id = Store::table_id(table);
    return j.run("Rename Field " + curly(from) + " to " + curly(to), {id}, [&](Store& s) {
        auto t = live_table(s, table);
        if (!t) return;
        if (!t->column(from)) { s.refuse(Error{Error::Code::NotFound, curly(table) + " has no field " + curly(from)}); return; }
        if (const auto* x = named_like(*t, to, from)) {
            s.refuse(Error{Error::Code::Exists, curly(table) + " already has a field named " + curly(x->name)});
            return;
        }
        for (auto& c : t->columns) if (c.name == from) c.name = to;
        for (auto& ix : t->indexes)
            for (auto& col : ix.columns) if (col == from) col = to;
        s.put(id, *t);
    });
}

std::expected<void, Error> delete_field(Journal& j, const std::string& table, const std::string& column) {
    // Decide what the step declares BEFORE it runs: the field's values ride
    // along when there are any, so undo can put them back.
    Store& st = j.store();
    st.forget_schema();
    const Table* info = st.table_info(table);
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!info || info->kind != Table::Kind::Table)
        return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(table)});
    const Column* col = info->column(column);
    if (!col) return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no field " + curly(column)});
    if (col->pk_position > 0)
        return std::unexpected(Error{Error::Code::Unsupported, curly(column) + " is part of the key -- the key is not changed here"});
    if (info->columns.size() == 1)
        return std::unexpected(Error{Error::Code::Unsupported, "a table needs at least one field -- " + curly(column) + " is its last"});
    const std::int64_t held = count_values(st.connection(), table, column);
    if (held < 0) return std::unexpected(Error{Error::Code::Internal, "could not count " + curly(column) + "'s values"});
    if (held > 0 && info->primary_key().empty())
        return std::unexpected(Error{Error::Code::Unsupported, curly(table) + " has no primary key -- Acorn can't keep " +
                                     curly(column) + "'s values to bring them back, so it won't delete it"});
    const auto tid = Store::table_id(table);
    std::vector<Store::Id> ids;
    if (held > 0) ids.push_back(Store::field_id(table, column));   // put back AFTER the table, on undo
    ids.push_back(tid);
    return j.run("Delete Field " + curly(column) + " from " + curly(table), ids, [&](Store& s) {
        auto t = live_table(s, table);
        if (!t) return;
        std::erase_if(t->columns, [&](const ColumnDef& c) { return c.name == column; });
        std::erase_if(t->indexes, [&](const IndexDef& ix) {
            return std::find(ix.columns.begin(), ix.columns.end(), column) != ix.columns.end();
        });
        s.put(tid, *t);
    });
}

std::expected<void, Error> new_index(Journal& j, const std::string& table, const IndexDef& ix) {
    if (auto why = check_name("The index", ix.name); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto id = Store::table_id(table);
    return j.run("New Index " + curly(ix.name) + " on " + curly(table), {id}, [&](Store& s) {
        auto t = live_table(s, table);
        if (!t) return;
        auto all = s.connection().introspect();
        if (!all) { s.refuse(all.error()); return; }
        if (index_name_taken(*all, ix.name)) {
            s.refuse(Error{Error::Code::Exists, "there is already an index named " + curly(ix.name)});
            return;
        }
        IndexDef x = ix;
        x.partial = false;
        t->indexes.push_back(x);
        std::sort(t->indexes.begin(), t->indexes.end(), [](auto& a, auto& b) { return a.name < b.name; });
        s.put(id, *t);
    });
}

std::expected<void, Error> delete_index(Journal& j, const std::string& table, const std::string& index) {
    const auto id = Store::table_id(table);
    return j.run("Delete Index " + curly(index) + " from " + curly(table), {id}, [&](Store& s) {
        auto t = live_table(s, table);
        if (!t) return;
        const auto n = t->indexes.size();
        std::erase_if(t->indexes, [&](const IndexDef& x) { return x.name == index; });
        if (t->indexes.size() == n) { s.refuse(Error{Error::Code::NotFound, curly(table) + " has no index " + curly(index)}); return; }
        s.put(id, *t);
    });
}

std::string design_label(const TableDef& live, const TableDef& want) {
    const std::string t = curly(live.name);
    auto df = diff_tables(live, want);
    if (!df) return "Change Table " + t;
    const auto& d = *df;
    const bool fields = d.rename || !d.dropped.empty() || !d.added.empty() || !d.changed.empty();
    const bool others = d.rename || !d.dropped.empty() || !d.added.empty();
    if (d.rename && d.dropped.empty() && d.added.empty() && d.idx_drop.empty() && d.idx_make.empty())
        return "Rename Field " + curly(d.rename->first) + " to " + curly(d.rename->second);
    if (!others && d.changed.size() == 1 && d.idx_drop.empty() && d.idx_make.empty())
        return "Change Field " + curly(d.changed[0].live->name) + " in " + t;
    if (!d.rename && d.changed.empty() && d.dropped.size() == 1 && d.added.empty() && d.idx_make.empty()) {
        bool only_its = true;   // the indexes that go are the field's own
        for (const auto& n : d.idx_drop)
            for (const auto& lx : live.indexes)
                if (lx.name == n && std::find(lx.columns.begin(), lx.columns.end(), d.dropped[0]->name) == lx.columns.end())
                    only_its = false;
        if (only_its) return "Delete Field " + curly(d.dropped[0]->name) + " from " + t;
    }
    if (!d.rename && d.changed.empty() && d.dropped.empty() && d.added.size() == 1 && d.idx_drop.empty() && d.idx_make.empty())
        return "New Field " + curly(d.added[0]->name) + " in " + t;
    if (!fields && d.idx_make.size() == 1 && d.idx_drop.empty()) return "New Index " + curly(d.idx_make[0]->name) + " on " + t;
    if (!fields && d.idx_drop.size() == 1 && d.idx_make.empty()) return "Delete Index " + curly(d.idx_drop[0]) + " from " + t;
    return "Change Table " + t;
}

namespace {
std::string plural(std::int64_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// What a design step will do to the data, read outside a step: the fields it
// drops (values held) and the fields it changes (values that won't fit the
// new kind, empty records a new "required" fills). Refusals come back as
// errors, in words -- the window shows them before Apply.
struct Reckoning {
    TableDef                  live;
    std::vector<std::string>  names;     // dropped
    std::vector<std::int64_t> held;      // non-NULL values in each
    std::vector<std::string>  keep;      // changed fields whose values the step must keep
    std::vector<std::string>  words;     // what is lost or changed, one line each
    bool                      keyless = false;
};
std::expected<Reckoning, Error> reckon(Journal& j, const TableDef& want) {
    Store& st = j.store();
    st.forget_schema();
    auto got = st.get(Store::table_id(want.name));
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!got) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(want.name)});
    Reckoning out;
    out.live = std::get<TableDef>(*got);
    auto df = diff_tables(out.live, want);
    if (!df) return std::unexpected(Error{Error::Code::Unsupported, df.error()});
    const Dialect dl = st.dialect();
    // One field gone + one come of the same kind is how a RENAME looks to the
    // planner (pictures carry no field identity). As a delete + add it would
    // be read back as a rename on redo -- so it is two steps. (Compared by
    // Acorn's field type: an engine reads its own type names back.)
    if (!df->rename && df->dropped.size() == 1 && df->added.size() == 1) {
        const ColumnDef& a = *df->dropped[0];
        const ColumnDef& b = *df->added[0];
        if (map_type(dl, a.decl_type) == map_type(dl, b.decl_type) && a.nullable == b.nullable &&
            a.default_sql == b.default_sql && a.pk_position == b.pk_position && a.auto_increment == b.auto_increment)
            return std::unexpected(Error{Error::Code::Unsupported,
                "deleting " + curly(a.name) + " and adding " + curly(b.name) +
                " (the same kind of field) in one step would read as a rename -- apply the delete first"});
    }
    out.keyless = true;
    for (const auto& c : out.live.columns) if (c.pk_position > 0) out.keyless = false;
    Connection& cn = st.connection();
    for (const auto* c : df->dropped) {
        const std::int64_t n = count_values(cn, want.name, c->name);
        if (n < 0) return std::unexpected(Error{Error::Code::Internal, "could not count " + curly(c->name) + "'s values"});
        out.names.push_back(c->name);
        out.held.push_back(n);
        if (n > 0) out.words.push_back(curly(c->name) + " holds " + plural(n, "value", "values") + " -- they go with the field");
    }
    if (!df->changed.empty() && dl == Dialect::SQLite)
        if (auto why = rebuild_blocker(cn, want.name); !why.empty())
            return std::unexpected(Error{Error::Code::Unsupported, why});
    for (const auto& ch : df->changed) {
        const ColumnDef& l = *ch.live;
        const ColumnDef& w = *ch.want;
        const FieldType from = map_type(dl, l.decl_type), to = map_type(dl, w.decl_type);
        const std::string q = curly(l.name);
        std::int64_t held = 0, misfits = 0;
        if (ch.type) {
            auto r = cn.query("SELECT " + cn.quote_ident(l.name) + " FROM " + cn.quote_ident(want.name) + " WHERE " +
                              cn.quote_ident(l.name) + " IS NOT NULL");
            if (!r) return std::unexpected(r.error());
            for (const auto& row : r->rows) {
                ++held;
                if (!convert_value(row[0], from, to)) ++misfits;
            }
        }
        const std::int64_t empties = (!w.nullable && l.nullable) ? count_null(cn, want.name, l.name) : 0;
        if (empties < 0) return std::unexpected(Error{Error::Code::Internal, "could not count " + q + "'s empty records"});
        const std::string def = default_text(w.default_sql);
        if (misfits > 0 && !w.nullable && !w.default_sql)
            return std::unexpected(Error{Error::Code::Constraint,
                std::to_string(misfits) + " of " + q + "'s values won't fit " + kind_words(to) +
                " -- a required field needs a default for " + (misfits == 1 ? "it" : "them")});
        if (empties > 0 && !w.default_sql)
            return std::unexpected(Error{Error::Code::Constraint,
                plural(empties, "record has", "records have") + " no " + q + " -- a required field needs a default for " +
                (empties == 1 ? "it (or fill it first)" : "them (or fill them first)")});
        if (dl == Dialect::MariaDB && (misfits > 0 || empties > 0))
            return std::unexpected(Error{Error::Code::Unsupported,
                "this engine can't roll a table change back, so Acorn won't " + std::string(misfits ? "empty" : "fill") +
                " " + q + "'s " + (misfits ? "values that don't fit " + kind_words(to) : std::string("empty records")) +
                " here -- fix those records first"});
        if (misfits > 0) {
            out.words.push_back(std::to_string(misfits) + " of " + q + "'s " + plural(held, "value", "values") +
                                " won't fit " + kind_words(to) + " -- " +
                                (w.nullable ? (misfits == 1 ? "it becomes" : "they become") + std::string(" empty")
                                            : (misfits == 1 ? "it takes" : "they take") + std::string(" the default ") + curly(def)));
        }
        if (empties > 0) {
            out.words.push_back(plural(empties, "record has", "records have") + " no " + q + " -- " +
                                (empties == 1 ? "it takes" : "they take") + " the default " + curly(def));
        }
        if (held > 0 || empties > 0) out.keep.push_back(l.name);
    }
    return out;
}
}  // namespace

std::expected<std::vector<std::string>, Error> design_losses(Journal& j, const TableDef& want) {
    auto rk = reckon(j, want);
    if (!rk) return std::unexpected(rk.error());
    return rk->words;
}

std::expected<void, Error> design_table(Journal& j, const TableDef& want) {
    auto rk = reckon(j, want);
    if (!rk) return std::unexpected(rk.error());
    const auto tid = Store::table_id(want.name);
    std::vector<Store::Id> ids;
    auto keep = [&](const std::string& col) -> std::expected<void, Error> {
        if (rk->keyless)
            return std::unexpected(Error{Error::Code::Unsupported, curly(want.name) + " has no primary key -- Acorn can't keep " +
                                         curly(col) + "'s values to bring them back, so it won't change it"});
        ids.push_back(Store::field_id(want.name, col));   // put back AFTER the table, on undo
        return {};
    };
    for (std::size_t i = 0; i < rk->names.size(); ++i)
        if (rk->held[i] > 0)
            if (auto k = keep(rk->names[i]); !k) return k;
    for (const auto& col : rk->keep)
        if (auto k = keep(col); !k) return k;
    ids.push_back(tid);
    TableDef w = want;
    w.rows = rk->live.rows;
    const std::string label = design_label(rk->live, w);
    return j.run(label, ids, [&](Store& s) {
        auto t = live_table(s, want.name);
        if (!t) return;
        if (*t == w) return;   // nothing to do: no step
        w.rows = t->rows;
        s.put(tid, w);
    });
}

std::expected<void, Error> change_field(Journal& j, const std::string& table, const ColumnDef& c) {
    Store& st = j.store();
    st.forget_schema();
    auto got = st.get(Store::table_id(table));
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!got) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(table)});
    TableDef w = std::get<TableDef>(*got);
    bool found = false;
    for (auto& x : w.columns) if (x.name == c.name) { x = c; found = true; }
    if (!found) return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no field " + curly(c.name)});
    return design_table(j, w);
}

std::expected<std::vector<std::string>, Error> preview(Journal& j, const TableDef& want) {
    Store& st = j.store();
    st.forget_schema();
    auto got = st.get(Store::table_id(want.name));
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    const char q = st.connection().quote_ident("x").front();
    if (!got) {
        std::vector<std::string> out{create_table_sql(st.dialect(), q, want)};
        for (const auto& ix : want.indexes) out.push_back(create_index_sql(st.dialect(), q, want.name, ix));
        return out;
    }
    auto plan = plan_alter(st.dialect(), q, std::get<TableDef>(*got), want);
    if (!plan) return std::unexpected(Error{Error::Code::Unsupported, plan.error()});
    return *plan;
}

namespace {
// The walk's fixture for the designer verbs: a keyed table with an index and
// a few records holding values -- made through the door.
constexpr const char* kDesign = "acorn_verb_design";
std::expected<void, Error> design_fixture(Journal& j) {
    const Dialect d = j.store().dialect();
    j.store().forget_schema();
    if (!j.store().table_info(kDesign)) {
        j.store().clear_failed();
        TableDef t;
        t.name = kDesign;
        t.columns = {
            {"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
            {"title", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
            {"n", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false},
            {"note", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
        };
        t.indexes = {{"acorn_verb_design_n", {"n"}, false, false}};
        if (auto r = new_table(j, t); !r) return r;
        for (int i = 1; i <= 2; ++i)
            if (auto r = new_record(j, kDesign, {"title", "n"}, {Value("t" + std::to_string(i)), Value(i * 10)}); !r)
                return std::unexpected(r.error());
    }
    // Always one more record (a step), like the records fixture.
    if (auto r = new_record(j, kDesign, {"title", "n"}, {Value("seed"), Value(7)}); !r) return std::unexpected(r.error());
    return {};
}
}  // namespace

namespace {
// The walk's fixture for the record verbs: a table with a counting key and one
// record in it -- made through the door, like everything else.
constexpr const char* kRows = "acorn_verb_rows";
std::expected<void, Error> rows_fixture(Journal& j) {
    const Dialect d = j.store().dialect();
    j.store().forget_schema();
    if (!j.store().table_info(kRows)) {
        j.store().clear_failed();
        TableDef t;
        t.name = kRows;
        t.columns = {
            {"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
            {"title", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
            {"n", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false},
        };
        if (auto r = new_table(j, t); !r) return r;
    }
    auto r = new_record(j, kRows, {"title", "n"}, {Value("seed"), Value(1)});
    if (!r) return std::unexpected(r.error());
    return {};
}
// The newest record of the fixture (the one setup made).
std::expected<Store::Id, Error> newest_row(Journal& j) {
    auto r = j.store().connection().query("SELECT MAX(" + j.store().connection().quote_ident("id") + ") FROM " +
                                          j.store().connection().quote_ident(kRows));
    if (!r) return std::unexpected(r.error());
    if (r->rows.empty() || r->rows[0][0].is_null()) return std::unexpected(Error{Error::Code::NotFound, "no record"});
    auto id = r->rows[0][0].as_int();
    if (!id) return std::unexpected(Error{Error::Code::Internal, "the key is not an integer"});
    return Store::row_id(kRows, {Value(*id)});
}
}  // namespace

const std::vector<Verb>& all() {
    static const std::vector<Verb> kVerbs = {
        {"New Table", [](Journal& j) {
             const Dialect d = j.store().dialect();
             TableDef t;
             t.name = "acorn_verb_new_table";
             t.columns = {
                 {"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                 {"title", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
                 {"when", decl_for(d, FieldType::Timestamp), true, std::nullopt, 0, false},
             };
             return new_table(j, t);
         }},
        {"New Record", [](Journal& j) -> std::expected<void, Error> {
             auto r = new_record(j, kRows, {"title"}, {Value("made by the walk")});
             if (!r) return std::unexpected(r.error());
             return {};
         }, rows_fixture},
        {"Edit Field", [](Journal& j) -> std::expected<void, Error> {
             auto row = newest_row(j);
             if (!row) return std::unexpected(row.error());
             return set_field(j, *row, "title", Value("edited by the walk"));
         }, rows_fixture},
        {"Delete Record", [](Journal& j) -> std::expected<void, Error> {
             auto row = newest_row(j);
             if (!row) return std::unexpected(row.error());
             return delete_record(j, *row);
         }, rows_fixture},
        // s007 -- the table designer
        {"New Field", [](Journal& j) {
             return new_field(j, kDesign, {"added", decl_for(j.store().dialect(), FieldType::Text), true,
                                           std::nullopt, 0, false});
         }, design_fixture},
        {"Rename Field", [](Journal& j) { return rename_field(j, kDesign, "n", "amount"); }, design_fixture},
        {"Delete Field", [](Journal& j) { return delete_field(j, kDesign, "n"); }, design_fixture},
        {"New Index", [](Journal& j) {
             return new_index(j, kDesign, {"acorn_verb_design_title", {"title"}, false, false});
         }, design_fixture},
        {"Delete Index", [](Journal& j) { return delete_index(j, kDesign, "acorn_verb_design_n"); }, design_fixture},
        // s007c: a whole number field (holding 10, 20, 7 ...) becomes text --
        // the values converted, kept in the step; undo puts back the numbers.
        {"Change Field", [](Journal& j) {
             return change_field(j, kDesign, {"n", decl_for(j.store().dialect(), FieldType::Text), true,
                                              std::nullopt, 0, false});
         }, design_fixture},
        {"Design Table", [](Journal& j) -> std::expected<void, Error> {   // s007b: several changes, one step
             j.store().forget_schema();
             const Table* t = j.store().table_info(kDesign);
             if (!t) return std::unexpected(Error{Error::Code::NotFound, "no fixture"});
             TableDef w = TableDef::from(*t);
             std::erase_if(w.columns, [](const ColumnDef& c) { return c.name == "n"; });   // holds values
             w.indexes.clear();
             w.columns.push_back({"extra", decl_for(j.store().dialect(), FieldType::Text), true, std::nullopt, 0, false});
             w.indexes.push_back({"acorn_verb_design_title", {"title"}, false, false});
             return design_table(j, w);
         }, design_fixture},
    };
    return kVerbs;
}

}  // namespace verbs

}  // namespace acorn::db
