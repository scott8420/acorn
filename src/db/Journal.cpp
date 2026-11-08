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
std::string field_list_of(const std::vector<std::string>& f) {   // “a”, “b” and “c”
    std::string s;
    for (std::size_t i = 0; i < f.size(); ++i) s += (i == 0 ? "" : i + 1 == f.size() ? " and " : ", ") + curly(f[i]);
    return s;
}
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
namespace {
// An engine's words for a rule, as Acorn compares them: NO ACTION is RESTRICT.
std::string rule_of(const std::string& r) {
    std::string u = upper(r);
    for (auto& ch : u) if (ch == '_') ch = ' ';
    return u == "NO ACTION" || u.empty() ? "RESTRICT" : u;
}
}  // namespace

bool LinkDef::operator==(const LinkDef& o) const {
    return columns == o.columns && ref_table == o.ref_table && ref_columns == o.ref_columns &&
           rule_of(on_delete) == rule_of(o.on_delete) && rule_of(on_update) == rule_of(o.on_update);
}

OnDelete LinkDef::deleting() const {
    const std::string r = rule_of(on_delete);
    if (r == "CASCADE") return OnDelete::Cascade;
    if (r == "SET NULL") return OnDelete::SetEmpty;
    return OnDelete::Restrict;
}

std::string on_delete_sql(OnDelete o) {
    switch (o) {
    case OnDelete::Cascade:  return "CASCADE";
    case OnDelete::SetEmpty: return "SET NULL";
    default:                 return "RESTRICT";
    }
}

std::string on_delete_words(OnDelete o) {
    switch (o) {
    case OnDelete::Cascade:  return "Cascade";
    case OnDelete::SetEmpty: return "Set Empty";
    default:                 return "Restrict";
    }
}

const LinkDef* TableDef::link(const std::vector<std::string>& cols) const {
    for (const auto& l : links) if (l.columns == cols) return &l;
    return nullptr;
}

TableDef TableDef::from(const Table& t, const Schema* sch) {
    TableDef d;
    d.name = t.name;
    for (const auto& c : t.columns)
        d.columns.push_back({c.name, c.decl_type, c.nullable, c.default_sql, c.pk_position, c.auto_increment});
    for (const auto& ix : t.indexes) {
        if (!ix.primary) d.indexes.push_back({ix.name, ix.columns, ix.unique, ix.partial});
        else d.key_name = ix.name;
    }
    std::sort(d.indexes.begin(), d.indexes.end(), [](auto& a, auto& b) { return a.name < b.name; });
    for (const auto& fk : t.foreign_keys)
        d.links.push_back({fk.name, fk.columns, fk.ref_table, sch ? sch->ref_columns(fk) : fk.ref_columns,
                           fk.on_delete, fk.on_update});
    std::sort(d.links.begin(), d.links.end(), [](auto& a, auto& b) { return a.columns < b.columns; });
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
    if (!(a == b)) return false;
    auto la = links, lb = o.links;
    auto by_fields = [](const LinkDef& x, const LinkDef& y) { return x.columns < y.columns; };
    std::sort(la.begin(), la.end(), by_fields);
    std::sort(lb.begin(), lb.end(), by_fields);
    return la == lb;
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
    for (const auto& l : t.links) s += ",\n  " + link_sql(q, l);
    s += "\n)";
    return s;
}

std::string link_sql(char q, const LinkDef& l) {
    auto list = [&](const std::vector<std::string>& v) {
        std::string out;
        for (std::size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + quote_ident_with(q, v[i]);
        return out;
    };
    std::string s = "FOREIGN KEY (" + list(l.columns) + ") REFERENCES " + quote_ident_with(q, l.ref_table) + " (" +
                    list(l.ref_columns) + ") ON DELETE " + rule_of(l.on_delete);
    if (rule_of(l.on_update) != "RESTRICT") s += " ON UPDATE " + rule_of(l.on_update);
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
    bool key = false;        // s007d: its place in the key, or its counting, changed
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
    bool                          key = false;                    // s007d: the key changes (a step of its own)
    std::vector<const LinkDef*>   link_drop;                      // s008: live's
    std::vector<const LinkDef*>   link_make;                      // want's
    bool links() const { return !link_drop.empty() || !link_make.empty(); }
};

// The key's fields in key order; the fields that count up.
std::vector<std::string> key_fields(const TableDef& t) {
    std::vector<const ColumnDef*> k;
    for (const auto& c : t.columns) if (c.pk_position > 0) k.push_back(&c);
    std::sort(k.begin(), k.end(), [](auto* a, auto* b) { return a->pk_position < b->pk_position; });
    std::vector<std::string> out;
    for (const auto* c : k) out.push_back(c->name);
    return out;
}
std::vector<std::string> counting_fields(const TableDef& t) {
    std::vector<std::string> out;
    for (const auto& c : t.columns) if (c.auto_increment) out.push_back(c.name);
    return out;
}
constexpr const char* kKeyAlone = "the key changes in a step of its own -- apply the other changes first";

std::expected<Diff, std::string> diff_tables(const TableDef& live, const TableDef& want) {
    Diff df;
    if (live.name != want.name) return std::unexpected("renaming a table is not done here");
    for (const auto& c : live.columns) if (!want.column(c.name)) df.dropped.push_back(&c);
    for (const auto& c : want.columns) if (!live.column(c.name)) df.added.push_back(&c);
    for (const auto& c : live.columns) {
        const ColumnDef* w = want.column(c.name);
        if (!w || *w == c) continue;
        if (c.pk_position != w->pk_position || c.auto_increment != w->auto_increment) {
            // s007d: a field whose place in the key or counting changes may
            // change nothing else but required, in that step.
            ColumnDef x = *w;
            x.pk_position = c.pk_position;
            x.auto_increment = c.auto_increment;
            x.nullable = c.nullable;
            if (!(x == c)) return std::unexpected(kKeyAlone);
            df.changed.push_back({&c, w, false, false, true});
            continue;
        }
        if (c.pk_position > 0)
            return std::unexpected(curly(c.name) + " is in the key -- its kind, required and default stay as they are");
        Change ch{&c, w, c.decl_type != w->decl_type, false, false};
        ch.fill = !w->nullable && w->default_sql && (c.nullable || ch.type);
        df.changed.push_back(ch);
    }
    if (df.dropped.size() == 1 && df.added.size() == 1 && same_but_name(*df.dropped[0], *df.added[0])) {
        df.rename = {df.dropped[0]->name, df.added[0]->name};
        df.dropped.clear();
        df.added.clear();
    }
    {   // the key, with a rename applied (a renamed key field is the same key)
        auto lk = key_fields(live), lc = counting_fields(live);
        if (df.rename)
            for (auto* v : {&lk, &lc})
                for (auto& n : *v) if (n == df.rename->first) n = df.rename->second;
        df.key = lk != key_fields(want) || lc != counting_fields(want);
    }
    if (df.key) {
        if (df.rename || !df.dropped.empty() || !df.added.empty()) return std::unexpected(kKeyAlone);
        for (const auto& ch : df.changed) if (!ch.key) return std::unexpected(kKeyAlone);
        if (key_fields(want).empty())
            return std::unexpected("a table Acorn writes needs a key -- choose the field(s) records are found by");
    }
    if (df.rename && (!df.dropped.empty() || !df.added.empty() || !df.changed.empty()))
        return std::unexpected("a rename is a step of its own");
    for (const auto* c : df.dropped)
        if (c->pk_position > 0) return std::unexpected(curly(c->name) + " is part of the key -- move the key off it first");
    for (const auto* c : df.added) {
        if (c->pk_position > 0 || c->auto_increment)
            return std::unexpected(curly(c->name) + " is new -- add it first, then make it the key");
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
    if (df.key && (!df.idx_drop.empty() || !df.idx_make.empty())) return std::unexpected(kKeyAlone);

    // s008 -- links: live's (the rename applied) against want's, by fields.
    auto relinked = [&](LinkDef l) {
        if (df.rename)
            for (auto& col : l.columns) if (col == df.rename->first) col = df.rename->second;
        return l;
    };
    for (const auto& ll : live.links) {
        const LinkDef r = relinked(ll);
        const LinkDef* w = want.link(r.columns);
        if (!w || !(*w == r)) df.link_drop.push_back(&ll);
    }
    for (const auto& wl : want.links) {
        bool kept = false;
        for (const auto& ll : live.links) if (relinked(ll) == wl) kept = true;
        if (kept) continue;
        if (wl.columns.empty() || wl.columns.size() != wl.ref_columns.size())
            return std::unexpected("a link joins as many fields as the key it points at");
        df.link_make.push_back(&wl);
    }
    for (const auto& wl : want.links)
        for (std::size_t i = 0; i < wl.columns.size(); ++i) {
            if (!want.column(wl.columns[i]))
                return std::unexpected("the link to " + curly(wl.ref_table) + " uses " + curly(wl.columns[i]) +
                                       ", which is going -- remove the link first");
            for (std::size_t k = 0; k < i; ++k)
                if (wl.columns[k] == wl.columns[i])
                    return std::unexpected("the link to " + curly(wl.ref_table) + " names " + curly(wl.columns[i]) + " twice");
        }
    if (df.links()) {
        // A link changes in a step of its own -- with one exception: an index on
        // exactly a link's fields (MariaDB makes one for a link, and keeps it
        // when the link goes) may come and go with it.
        constexpr const char* kLinkAlone = "a link changes in a step of its own -- apply the other changes first";
        if (df.key || df.rename || !df.dropped.empty() || !df.added.empty() || !df.changed.empty())
            return std::unexpected(kLinkAlone);
        auto backs = [&](const std::vector<std::string>& cols) {
            for (const auto* l : df.link_drop) if (l->columns == cols) return true;
            for (const auto* l : df.link_make) if (l->columns == cols) return true;
            return false;
        };
        for (const auto& n : df.idx_drop)
            for (const auto& lx : live.indexes)
                if (lx.name == n && !backs(lx.columns)) return std::unexpected(kLinkAlone);
        for (const auto* ix : df.idx_make)
            if (!backs(ix->columns)) return std::unexpected(kLinkAlone);
    }
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
    // The id count carries over (ids are never reused) -- also through a key
    // that stops counting and counts again (s007d): the count stays in
    // sqlite_sequence under the table's name, and comes back with it.
    bool lc = false, wc = false;
    for (const auto& c : live.columns) if (c.pk_position > 0 && c.auto_increment) lc = true;
    for (const auto& c : want.columns) if (c.pk_position > 0 && c.auto_increment) wc = true;
    if (lc || wc) {
        out.push_back({"DELETE FROM sqlite_sequence WHERE name = " + sql_text(tmp), {}});
        out.push_back({"INSERT INTO sqlite_sequence (name, seq)\n  SELECT " + sql_text(tmp) +
                       ", seq FROM sqlite_sequence WHERE name = " + sql_text(live.name), {}});
    }
    out.push_back({drop_table_sql(q, live.name), {}});
    if (lc != wc) out.push_back({"DELETE FROM sqlite_sequence WHERE name = " + sql_text(live.name), {}});
    out.push_back({"ALTER TABLE " + quote_ident_with(q, tmp) + " RENAME TO " + quote_ident_with(q, live.name), {}});
    for (const auto& ch : df.changed)
        if (ch.type) out.push_back({write_back_words(q, ch, Dialect::SQLite), ch.want->name});
    for (const auto& ix : want.indexes) out.push_back({create_index_sql(Dialect::SQLite, q, live.name, ix), {}});
    return out;
}

// s007d: what a key may be, per engine ("" = fine). Its fields are checked
// against the records elsewhere (Store::key_blocker); this is the shape.
std::string key_rules(Dialect d, const TableDef& live, const TableDef& want) {
    const auto wk = key_fields(want);
    for (const auto& c : want.columns) {
        if (!c.auto_increment) continue;
        if (wk.size() != 1 || wk[0] != c.name)
            return "only a key of one field can count up -- " + curly(c.name) + " would count alone";
        if (map_type(d, c.decl_type) != FieldType::Integer)
            return "a counting key is a whole number -- change " + curly(c.name) + " to a whole number first";
        const ColumnDef* l = live.column(c.name);
        if (c.default_sql && !(l && l->auto_increment))
            return curly(c.name) + " has a default -- a counting key makes its own; clear the default first";
        if (d == Dialect::SQLite && upper(c.decl_type) != "INTEGER")
            return "SQLite counts up only a field declared INTEGER -- " + curly(c.name) + " is " + c.decl_type;
    }
    if (d == Dialect::SQLite && wk.size() == 1) {
        const ColumnDef* k = want.column(wk[0]);
        if (k && upper(k->decl_type) == "INTEGER" && !k->auto_increment)
            return "on SQLite a key of one whole-number field always counts up (the engine's own rule) -- " +
                   curly(k->name) + " can't stop counting while it is the key alone";
    }
    if (d == Dialect::PostgreSQL)
        for (const auto& c : live.columns)
            if (c.auto_increment && c.default_sql && !(want.column(c.name) && want.column(c.name)->auto_increment))
                return curly(c.name) + " counts by a sequence default -- Acorn doesn't change that kind of counting key";
    return {};
}

// s007e: a field coming back (undo of Delete Field) belongs somewhere other
// than the end when a field the table keeps comes after it in `want`.
bool out_of_place(const TableDef& want, const Diff& df) {
    bool seen_added = false;
    for (const auto& c : want.columns) {
        bool added = false;
        for (const auto* a : df.added) if (a->name == c.name) added = true;
        if (added) seen_added = true;
        else if (seen_added) return true;
    }
    return false;
}

std::expected<std::vector<Stmt>, std::string>
plan_steps(Dialect d, char q, const TableDef& live, const TableDef& want, bool in_place) {
    auto df = diff_tables(live, want);
    if (!df) return std::unexpected(df.error());
    if (df->key)
        if (auto why = key_rules(d, live, want); !why.empty()) return std::unexpected(why);
    const std::string t = quote_ident_with(q, live.name);
    std::vector<Stmt> out;
    const auto lk = key_fields(live), wk = key_fields(want);
    const bool new_key = df->key && lk != wk;   // the key's fields change (not only its counting)
    auto key_list = [&] {
        std::string s;
        for (std::size_t i = 0; i < wk.size(); ++i) s += (i ? ", " : "") + quote_ident_with(q, wk[i]);
        return s;
    };
    const bool misplaced = out_of_place(want, *df);
    // s008: SQLite can't add or drop a link in place either -- the child is
    // made again with its links written in.
    if (d == Dialect::SQLite && (!df->changed.empty() || df->links())) {
        for (const auto& lx : live.indexes)
            if (lower(lx.name).rfind("sqlite_autoindex", 0) == 0)
                return std::unexpected(curly(live.name) + " has a UNIQUE rule written into the table -- Acorn can't "
                                       "make the table again with it yet");
        return plan_rebuild(q, live, want, *df);
    }
    // s007e: a field put back in its old place -- SQLite can only add at the
    // end, so the table is made again in `want`'s order. When it can't be (a
    // trigger / view / link / rule the picture doesn't carry: in_place false,
    // or a UNIQUE written into the table), ADD COLUMN at the end, as before.
    if (d == Dialect::SQLite && in_place && misplaced && df->dropped.empty() && !df->rename) {
        bool inline_unique = false;
        for (const auto& lx : live.indexes)
            if (lower(lx.name).rfind("sqlite_autoindex", 0) == 0) inline_unique = true;
        if (!inline_unique) return plan_rebuild(q, live, want, *df);
    }
    if (d == Dialect::MariaDB) {   // ONE statement: atomic on an engine that can't roll DDL back
        std::vector<std::string> cl;
        for (const auto* l : df->link_drop) {   // s008: links first, so an index they used may go
            if (l->name.empty()) return std::unexpected("the link to " + curly(l->ref_table) + " has no name Acorn can read");
            cl.push_back("DROP FOREIGN KEY " + quote_ident_with(q, l->name));
        }
        if (new_key && !lk.empty()) cl.push_back("DROP PRIMARY KEY");
        for (const auto& n : df->idx_drop) cl.push_back("DROP INDEX " + quote_ident_with(q, n));
        if (df->rename)
            cl.push_back("RENAME COLUMN " + quote_ident_with(q, df->rename->first) + " TO " +
                         quote_ident_with(q, df->rename->second));
        for (const auto* c : df->dropped) cl.push_back("DROP COLUMN " + quote_ident_with(q, c->name));
        for (const auto& ch : df->changed)
            cl.push_back("MODIFY COLUMN " + field_decl(q, *ch.want) + (ch.want->auto_increment ? " AUTO_INCREMENT" : ""));
        if (new_key) cl.push_back("ADD PRIMARY KEY (" + key_list() + ")");
        for (const auto* c : df->added) {
            std::string a = "ADD COLUMN " + field_decl(q, *c);
            if (misplaced) {   // s007e: back in its place -- after the field before it in `want`
                const ColumnDef* prev = nullptr;
                for (const auto& w : want.columns) { if (w.name == c->name) break; prev = &w; }
                a += prev ? " AFTER " + quote_ident_with(q, prev->name) : std::string(" FIRST");
            }
            cl.push_back(a);
        }
        for (const auto* ix : df->idx_make) {
            std::string a = std::string("ADD ") + (ix->unique ? "UNIQUE " : "") + "INDEX " + quote_ident_with(q, ix->name) + " (";
            for (std::size_t i = 0; i < ix->columns.size(); ++i) a += (i ? ", " : "") + quote_ident_with(q, ix->columns[i]);
            cl.push_back(a + ")");
        }
        for (const auto* l : df->link_make) cl.push_back("ADD " + link_sql(q, *l));
        if (cl.empty()) return out;
        std::string s = "ALTER TABLE " + t;
        for (std::size_t i = 0; i < cl.size(); ++i) s += (i ? ",\n  " : "\n  ") + cl[i];
        out.push_back({s, {}});
        // The engine casts; Acorn then puts its own reading where they differ.
        for (const auto& ch : df->changed)
            if (ch.type) out.push_back({write_back_words(q, ch, d), ch.want->name});
        return out;
    }
    // PostgreSQL's key (s007d): counting and the key's constraint go first,
    // so a field leaving the key may become optional; the new key and its
    // counting come after the fields are required -- the count starting past
    // the highest number already there.
    for (const auto* l : df->link_drop) {   // s008: PostgreSQL drops a link by its name
        if (l->name.empty()) return std::unexpected("the link to " + curly(l->ref_table) + " has no name Acorn can read");
        out.push_back({"ALTER TABLE " + t + " DROP CONSTRAINT " + quote_ident_with(q, l->name), {}});
    }
    if (df->key) {
        for (const auto& ch : df->changed)
            if (ch.live->auto_increment && !ch.want->auto_increment)
                out.push_back({"ALTER TABLE " + t + " ALTER COLUMN " + quote_ident_with(q, ch.live->name) + " DROP IDENTITY", {}});
        if (new_key && !lk.empty()) {
            if (live.key_name.empty()) return std::unexpected(curly(live.name) + "'s key has no name Acorn can read");
            out.push_back({"ALTER TABLE " + t + " DROP CONSTRAINT " + quote_ident_with(q, live.key_name), {}});
        }
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
    if (df->key) {
        if (new_key) out.push_back({"ALTER TABLE " + t + " ADD PRIMARY KEY (" + key_list() + ")", {}});
        for (const auto& ch : df->changed)
            if (ch.want->auto_increment && !ch.live->auto_increment) {
                const std::string c = quote_ident_with(q, ch.want->name);
                out.push_back({"ALTER TABLE " + t + " ALTER COLUMN " + c + " ADD GENERATED BY DEFAULT AS IDENTITY", {}});
                out.push_back({"SELECT setval(pg_get_serial_sequence(" + sql_text(t) + ", " + sql_text(ch.want->name) +
                               "), COALESCE(MAX(" + c + "), 0) + 1, false) FROM " + t, {}});
            }
    }
    for (const auto* c : df->added) out.push_back({"ALTER TABLE " + t + " ADD COLUMN " + field_decl(q, *c), {}});
    for (const auto* ix : df->idx_make) out.push_back({create_index_sql(d, q, live.name, *ix), {}});
    for (const auto* l : df->link_make) out.push_back({"ALTER TABLE " + t + " ADD " + link_sql(q, *l), {}});
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
            // s008: REFERENCES is no longer here -- the picture carries the
            // table's links and the rebuild writes them back.
            for (const char* word : {"CHECK", "COLLATE", "UNIQUE", "GENERATED", "CONSTRAINT", "DEFERRABLE", "MATCH",
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
    }
    // A table others point at (or that points at itself) can't be dropped and
    // made again inside a transaction: SQLite would need foreign_keys off,
    // which it allows only outside one (s008: the parent keeps this refusal).
    auto sch = c.introspect();
    if (!sch) return "could not read the schema: " + sch.error().message;
    for (const auto& in : sch->links_to(table))
        return in.from->name == table
                   ? curly(table) + " links to itself -- SQLite can't make such a table again inside a step"
                   : curly(in.from->name) + " links to " + curly(table) + " -- Acorn can't make the table again yet";
    return {};
}

// s007e: may SQLite make `live` again to put a field back in its place? Only
// asked when a field comes back somewhere other than the end; "no" is not a
// refusal -- the field goes on the end, as it always did.
bool can_place(Connection& c, Dialect d, const TableDef& live, const TableDef& want) {
    if (d != Dialect::SQLite) return true;
    auto df = diff_tables(live, want);
    if (!df || !df->changed.empty() || !out_of_place(want, *df)) return true;
    return rebuild_blocker(c, live.name).empty();
}

std::int64_t count_of(Connection& c, const std::string& sql) {
    auto n = c.query(sql);
    if (!n || n->rows.empty()) return -1;
    const auto& v = n->rows[0][0];
    if (auto i = v.as_int()) return *i;
    if (auto t = v.as_text()) return std::stoll(*t);
    return -1;
}

// s007d: why the records can't be found by `want`'s key ("" = they can) --
// every record must have the key's fields filled, no two the same; a key
// made to count up must hold numbers the engine will keep as they are.
std::string key_blocker(Connection& c, Dialect d, const TableDef& live, const TableDef& want) {
    const auto wk = key_fields(want);
    if (wk.empty()) return {};
    const std::string t = c.quote_ident(want.name);
    std::string cols, names;
    for (std::size_t i = 0; i < wk.size(); ++i) {
        const std::string q = c.quote_ident(wk[i]);
        cols += (i ? ", " : "") + q;
        names += (i == 0 ? "" : i + 1 == wk.size() ? " and " : ", ") + curly(wk[i]);
    }
    if (key_fields(live) != wk) {
        for (const auto& f : wk) {   // the field that is empty, by name
            const std::int64_t empty = count_of(c, "SELECT COUNT(*) FROM " + t + " WHERE " + c.quote_ident(f) + " IS NULL");
            if (empty < 0) return "could not count the records without " + curly(f);
            if (empty > 0)
                return std::to_string(empty) + (empty == 1 ? " record has" : " records have") + " no " + curly(f) +
                       " -- every record needs one to be found by " + names + "; fill " +
                       (empty == 1 ? "it" : "them") + " first";
        }
        const std::int64_t dup = count_of(c, "SELECT COUNT(*) FROM (SELECT " + cols + " FROM " + t + " GROUP BY " + cols +
                                              " HAVING COUNT(*) > 1) AS acorn_dup");
        if (dup < 0) return "could not look for records sharing " + names;
        if (dup > 0)
            return std::to_string(dup) + (dup == 1 ? " value of " : " values of ") + names +
                   (dup == 1 ? " is" : " are") + " shared by more than one record -- a key tells each record "
                   "apart; make them different first";
    }
    for (const auto& w : want.columns) {
        if (!w.auto_increment) continue;
        const ColumnDef* l = live.column(w.name);
        if (l && l->auto_increment) continue;
        const std::string q = c.quote_ident(w.name);
        if (d == Dialect::SQLite) {
            const std::int64_t odd = count_of(c, "SELECT COUNT(*) FROM " + t + " WHERE typeof(" + q + ") <> 'integer'");
            if (odd < 0) return "could not read " + curly(w.name) + "'s values";
            if (odd > 0)
                return std::to_string(odd) + " of " + curly(w.name) + "'s values " + (odd == 1 ? "isn't" : "aren't") +
                       " a whole number -- a counting key holds only whole numbers; fix " +
                       (odd == 1 ? "it" : "them") + " first";
        }
        if (d == Dialect::MariaDB) {
            const std::int64_t zero = count_of(c, "SELECT COUNT(*) FROM " + t + " WHERE " + q + " = 0");
            if (zero < 0) return "could not read " + curly(w.name) + "'s values";
            if (zero > 0)
                return "a record has 0 in " + curly(w.name) + " -- this engine would give it a new number when the key "
                       "starts counting; change it first";
        }
    }
    return {};
}

// s008: the records of `table` whose `l` points at no record of its parent
// -- every link field filled, no parent record with that key. SQL shared by
// the count (the refusal) and the list (unmatched()).
std::string unmatched_where(Connection& c, const std::string& table, const LinkDef& l) {
    const std::string a = c.quote_ident("acorn_c"), b = c.quote_ident("acorn_p");
    std::string filled, match;
    for (std::size_t i = 0; i < l.columns.size(); ++i) {
        filled += (i ? " AND " : "") + a + "." + c.quote_ident(l.columns[i]) + " IS NOT NULL";
        match += (i ? " AND " : "") + b + "." + c.quote_ident(l.ref_columns[i]) + " = " + a + "." + c.quote_ident(l.columns[i]);
    }
    return " FROM " + c.quote_ident(table) + " " + a + " WHERE " + filled + " AND NOT EXISTS (SELECT 1 FROM " +
           c.quote_ident(l.ref_table) + " " + b + " WHERE " + match + ")";
}

// s008: why this step can't change the links it changes, or the fields other
// tables' links point at ("" = it can). Read from the live database: the
// parent's key, the kinds, the records that don't match.
std::string link_blocker(Connection& c, Dialect d, const TableDef& live, const TableDef& want, const Diff& df) {
    auto sch = c.introspect();
    if (!sch) return "could not read the schema: " + sch.error().message;
    // The parent side: what other tables point at stays as it is.
    for (const auto& in : sch->links_to(live.name)) {
        if (in.from->name == live.name) continue;   // a link to itself is this table's own, below
        const auto refs = sch->ref_columns(*in.fk);
        const std::string who = curly(in.from->name) + " links to " + curly(live.name);
        if (df.key) return who + " -- remove the link before changing " + curly(live.name) + "'s key";
        for (const auto& rc : refs) {
            if (df.rename && df.rename->first == rc) return who + "'s " + curly(rc) + " -- remove the link before renaming it";
            for (const auto* x : df.dropped)
                if (x->name == rc) return who + "'s " + curly(rc) + " -- remove the link first";
        }
    }
    // The child side: each link this step makes.
    for (const auto* l : df.link_make) {
        const Table* p = sch->table(l->ref_table);
        const std::string to = curly(l->ref_table);
        if (!p || p->kind != Table::Kind::Table) return "there is no table " + to + " to link to";
        const auto pk = p->primary_key();
        if (pk.empty()) return to + " has no key -- a link points at a table's key";
        if (pk != l->ref_columns) return "a link points at " + to + "'s key (" + field_list_of(pk) + ")";
        if (pk.size() != l->columns.size())
            return to + "'s key has " + std::to_string(pk.size()) + " field" + (pk.size() == 1 ? "" : "s") +
                   " -- the link needs as many";
        for (std::size_t i = 0; i < pk.size(); ++i) {
            const ColumnDef* mine = want.column(l->columns[i]);
            const Column* theirs = p->column(pk[i]);
            if (!mine || !theirs) return "the link to " + to + " names a field that isn't there";
            const FieldType a = map_type(d, mine->decl_type), b = theirs->type;
            if (a != b)
                return curly(mine->name) + " is " + kind_words(a) + ", " + to + "'s " + curly(theirs->name) + " is " +
                       kind_words(b) + " -- a link joins fields of the same kind";
            if (l->deleting() == OnDelete::SetEmpty && !mine->nullable)
                return curly(mine->name) + " is required -- Set Empty needs it optional (or choose Restrict or Cascade)";
        }
        if (d == Dialect::SQLite && l->ref_table == live.name)
            return curly(live.name) + " would link to itself -- SQLite can't make such a table again inside a step";
        auto n = c.query("SELECT COUNT(*)" + unmatched_where(c, live.name, *l));
        if (!n || n->rows.empty()) return "could not look for records that don't match " + to;
        const auto& v = n->rows[0][0];
        const std::int64_t k = v.as_int() ? *v.as_int() : (v.as_text() ? std::stoll(*v.as_text()) : 0);
        if (k > 0)
            return k == 1 ? "1 record in " + curly(live.name) + " points at a " + to + " record that isn't there -- fix it first"
                          : std::to_string(k) + " records in " + curly(live.name) + " point at " + to +
                                " records that aren't there -- fix them first";
    }
    if (d == Dialect::SQLite && df.links())
        if (auto why = rebuild_blocker(c, live.name); !why.empty()) return why;
    return {};
}

}  // namespace

std::expected<std::vector<std::string>, std::string>
plan_alter(Dialect d, char q, const TableDef& live, const TableDef& want, bool in_place) {
    auto steps = plan_steps(d, q, live, want, in_place);
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

// Key parts, each self-delimiting: <len>:<field>= then the value --
// i<int>;  r<real>;  s<bytes>:<text>
Store::Id Store::row_id(const std::string& table, const std::vector<std::string>& key_fields,
                        const std::vector<db::Value>& key) {
    if (key.empty() || key_fields.size() != key.size()) return {};
    std::string k;
    for (std::size_t n = 0; n < key.size(); ++n) {
        const auto& v = key[n];
        if (key_fields[n].empty()) return {};
        k += std::to_string(key_fields[n].size()) + ":" + key_fields[n] + "=";
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
    std::vector<std::string> fields;
    return parse_row_id(id, table, fields, key);
}

bool Store::parse_row_id(const Id& id, std::string& table, std::vector<std::string>& key_fields,
                         std::vector<db::Value>& key) {
    if (id.rfind("row:", 0) != 0) return false;
    std::size_t i = 4;
    auto num = [&](char end, std::size_t& out) {
        std::size_t j = id.find(end, i);
        if (j == std::string::npos || j == i || j - i > 9) return false;
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
    key_fields.clear();
    while (i < id.size()) {
        std::size_t fl = 0;
        if (!num(':', fl) || fl == 0 || i + fl + 1 > id.size() || id[i + fl] != '=') return false;
        key_fields.push_back(id.substr(i, fl));
        i += fl + 1;
        if (i >= id.size()) return false;
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

Store::Id Store::layout_id(const std::string& table, const std::string& name) {
    return "layout:" + std::to_string(table.size()) + ":" + table + "/" + name;
}

bool Store::parse_layout_id(const Id& id, std::string& table, std::string& name) {
    if (!is_layout(id)) return false;
    const auto c = id.find(':', 7);
    if (c == std::string::npos || c == 7) return false;
    std::size_t n = 0;
    for (std::size_t k = 7; k < c; ++k) {
        if (!std::isdigit(static_cast<unsigned char>(id[k]))) return false;
        n = n * 10 + static_cast<std::size_t>(id[k] - '0');
        if (n > id.size()) return false;
    }
    if (c + 1 + n >= id.size() || id[c + 1 + n] != '/') return false;
    table = id.substr(c + 1, n);
    name  = id.substr(c + 2 + n);
    return !table.empty() && !name.empty();
}

Store::Id Store::calc_id(const std::string& table, const std::string& name) {
    return "calc:" + std::to_string(table.size()) + ":" + table + "/" + name;
}

bool Store::parse_calc_id(const Id& id, std::string& table, std::string& name) {
    if (!is_calc(id)) return false;
    // The same shape as a layout's id after its prefix.
    return parse_layout_id("layout:" + id.substr(5), table, name);
}

Store::Id Store::summary_id(const std::string& table, const std::string& name) {
    return "summary:" + std::to_string(table.size()) + ":" + table + "/" + name;
}

bool Store::parse_summary_id(const Id& id, std::string& table, std::string& name) {
    if (!is_summary(id)) return false;
    return parse_layout_id("layout:" + id.substr(8), table, name);
}

Store::Id Store::list_id(const std::string& table, const std::string& name) {
    return "vlist:" + std::to_string(table.size()) + ":" + table + "/" + name;
}

bool Store::parse_list_id(const Id& id, std::string& table, std::string& name) {
    if (!is_list(id)) return false;
    return parse_layout_id("layout:" + id.substr(6), table, name);
}

Store::Id Store::script_id(const std::string& table, const std::string& name) {
    return "script:" + std::to_string(table.size()) + ":" + table + "/" + name;
}

bool Store::parse_script_id(const Id& id, std::string& table, std::string& name) {
    if (!is_script(id)) return false;
    return parse_layout_id("layout:" + id.substr(7), table, name);
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
    if (is_box(id)) return curly(id.substr(4)) + "'s place in the Graph";
    std::string name;
    if (parse_layout_id(id, table, name)) return "the layout " + curly(name) + " of " + curly(table);
    if (parse_calc_id(id, table, name)) return "the calculation " + curly(name) + " of " + curly(table);
    if (parse_summary_id(id, table, name)) return "the summary " + curly(name) + " of " + curly(table);
    if (parse_list_id(id, table, name)) return "the value list " + curly(name) + " of " + curly(table);
    if (parse_script_id(id, table, name)) return "the script " + curly(name) + " of " + curly(table);
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
    if (is_box(id)) {   // s009: from the solution, not the database
        if (!m_solution) return std::nullopt;
        const std::string t = id.substr(4);
        auto it = m_solution->boxes.find(t);
        if (it == m_solution->boxes.end()) return std::nullopt;
        return BoxDef{t, it->second.x, it->second.y};
    }
    if (is_layout(id)) {   // s010: from the solution
        std::string t, n;
        if (!m_solution || !parse_layout_id(id, t, n)) return std::nullopt;
        if (const auto* l = m_solution->layout(t, n)) return *l;
        return std::nullopt;
    }
    if (is_calc(id)) {   // s014a: from the solution
        std::string t, n;
        if (!m_solution || !parse_calc_id(id, t, n)) return std::nullopt;
        if (const auto* c = m_solution->calc(t, n)) return *c;
        return std::nullopt;
    }
    if (is_summary(id)) {   // s014b: from the solution
        std::string t, n;
        if (!m_solution || !parse_summary_id(id, t, n)) return std::nullopt;
        if (const auto* x = m_solution->summary(t, n)) return *x;
        return std::nullopt;
    }
    if (is_script(id)) {   // s017a: from the solution
        std::string t, n;
        if (!m_solution || !parse_script_id(id, t, n)) return std::nullopt;
        if (const auto* v = m_solution->script(t, n)) return *v;
        return std::nullopt;
    }
    if (is_list(id)) {   // s014c: from the solution
        std::string t, n;
        if (!m_solution || !parse_list_id(id, t, n)) return std::nullopt;
        if (const auto* v = m_solution->list(t, n)) return *v;
        return std::nullopt;
    }
    return std::nullopt;   // "new:..." -- a declaration, never a record
}

std::optional<Record> Store::get_table(const std::string& name) const {
    m_schema.reset();   // a table's shape is always read fresh
    const Table* t = table_info(name);
    if (failed() || !t || t->kind != Table::Kind::Table) return std::nullopt;
    TableDef d = TableDef::from(*t, &*m_schema);   // s008: links read as their parent's key fields
    auto n = m_conn.query("SELECT COUNT(*) FROM " + m_conn.quote_ident(name));
    if (!n || n->rows.empty()) { fail(Error::Code::Internal, "could not count the rows of " + curly(name)); return d; }
    const auto& v = n->rows[0][0];
    if (auto i = v.as_int()) d.rows = *i;
    else if (auto txt = v.as_text()) d.rows = std::stoll(*txt);
    return d;
}

namespace {
std::string field_list(const std::vector<std::string>& f) {
    std::string s;
    for (std::size_t i = 0; i < f.size(); ++i)
        s += (i == 0 ? "" : i + 1 == f.size() ? " and " : ", ") + curly(f[i]);
    return s;
}
}  // namespace

// s007d: a record named under one key, read when the table is found by
// another -- refused in words, never matched to whatever record has those
// values in the new key fields.
bool Store::same_key(const std::string& table, const Table& t, const std::vector<std::string>& named) const {
    const auto pk = t.primary_key();
    if (pk == named) return true;
    fail(Error::Code::Changed, curly(table) + "'s records are found by " +
         (pk.empty() ? std::string("no key") : field_list(pk)) + " now, not " + field_list(named) +
         " -- a step taken under the old key can't be undone or redone");
    return false;
}

std::optional<Record> Store::get_row(const Id& id) const {
    std::string table;
    std::vector<std::string> fields;
    std::vector<db::Value> key;
    if (!parse_row_id(id, table, fields, key)) { fail(Error::Code::Internal, "not a record: " + id); return std::nullopt; }
    const Table* t = table_info(table);
    if (failed() || !t || t->kind != Table::Kind::Table) return std::nullopt;
    if (!same_key(table, *t, fields)) return std::nullopt;
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
    FieldData d{table, column, pk, {}, {}};
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
    if (!v->values.empty() && !same_key(table, *t, v->key_fields)) return;
    const auto got = get_field(id);
    if (failed()) return;
    // get_field re-reads the schema: `t` pointed into the old one (s009 fix --
    // a use-after-free the MariaDB suite hit now and then).
    t = table_info(table);
    if (failed()) return;
    if (!t || !t->column(column)) { fail(Error::Code::Changed, curly(table) + "." + curly(column) + " is not there"); return; }
    const FieldData live = got ? std::get<FieldData>(*got) : FieldData{table, column, pk, {}, {}};
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
            fail(Error::Code::Changed, describe(row_id(table, pk, key)) + " is not there any more -- " +
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
    t = table_info(table);   // get_field re-read the schema; `t` pointed into the old one (s009 fix)
    if (failed()) return false;
    if (!t || !t->column(to.name)) { fail(Error::Code::Changed, curly(table) + "." + curly(to.name) + " is not there"); return false; }
    const FieldData live = now ? std::get<FieldData>(*now) : FieldData{table, to.name, t->primary_key(), {}, {}};
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
            fail(Error::Code::Changed, describe(row_id(table, t->primary_key(), key)) + " matches more than one record");
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
    if (is_local(id)) { put_local(id, v); return; }
    fail(Error::Code::Internal, "unknown record " + id);
}

// s009 / s010: the solution, saved whole; the database is never touched. A
// save that fails puts the solution back as it was -- no step.
void Store::put_local(const Id& id, const std::optional<Record>& v) {
    if (!m_solution) {
        fail(Error::Code::Unsupported, is_box(id)    ? "there is no solution to keep the Graph's boxes in"
                                       : is_calc(id) || is_summary(id) ? "there is no solution to keep calculations in"
                                       : is_list(id) ? "there is no solution to keep value lists in"
                                       : is_script(id) ? "there is no solution to keep scripts in"
                                                     : "there is no solution to keep layouts in");
        return;
    }
    const core::Solution before = *m_solution;
    if (is_box(id)) {
        if (v && !std::holds_alternative<BoxDef>(*v)) { fail(Error::Code::Internal, id + " given a table or record"); return; }
        const std::string t = id.substr(4);
        if (v) m_solution->boxes[t] = {std::get<BoxDef>(*v).x, std::get<BoxDef>(*v).y};
        else   m_solution->boxes.erase(t);
    } else if (is_script(id)) {   // s017a
        std::string t, n;
        if (!parse_script_id(id, t, n)) { fail(Error::Code::Internal, "not a script: " + id); return; }
        if (v && !std::holds_alternative<core::Script>(*v)) { fail(Error::Code::Internal, id + " given something else"); return; }
        if (v) {
            const auto& x = std::get<core::Script>(*v);
            if (x.table != t || x.name != n) { fail(Error::Code::Internal, id + " given another script"); return; }
            m_solution->scripts[t][n] = x;
        } else if (auto it = m_solution->scripts.find(t); it != m_solution->scripts.end()) {
            it->second.erase(n);
            if (it->second.empty()) m_solution->scripts.erase(it);
        }
    } else if (is_list(id)) {   // s014c
        std::string t, n;
        if (!parse_list_id(id, t, n)) { fail(Error::Code::Internal, "not a value list: " + id); return; }
        if (v && !std::holds_alternative<core::ValueList>(*v)) { fail(Error::Code::Internal, id + " given something else"); return; }
        if (v) {
            const auto& x = std::get<core::ValueList>(*v);
            if (x.table != t || x.name != n) { fail(Error::Code::Internal, id + " given another value list"); return; }
            m_solution->lists[t][n] = x;
        } else if (auto it = m_solution->lists.find(t); it != m_solution->lists.end()) {
            it->second.erase(n);
            if (it->second.empty()) m_solution->lists.erase(it);
        }
    } else if (is_summary(id)) {   // s014b
        std::string t, n;
        if (!parse_summary_id(id, t, n)) { fail(Error::Code::Internal, "not a summary: " + id); return; }
        if (v && !std::holds_alternative<core::Summary>(*v)) { fail(Error::Code::Internal, id + " given something else"); return; }
        if (v) {
            const auto& x = std::get<core::Summary>(*v);
            if (x.table != t || x.name != n) { fail(Error::Code::Internal, id + " given another summary"); return; }
            m_solution->summaries[t][n] = x;
        } else if (auto it = m_solution->summaries.find(t); it != m_solution->summaries.end()) {
            it->second.erase(n);
            if (it->second.empty()) m_solution->summaries.erase(it);
        }
    } else if (is_calc(id)) {   // s014a
        std::string t, n;
        if (!parse_calc_id(id, t, n)) { fail(Error::Code::Internal, "not a calculation: " + id); return; }
        if (v && !std::holds_alternative<core::Calc>(*v)) { fail(Error::Code::Internal, id + " given something else"); return; }
        if (v) {
            const auto& c = std::get<core::Calc>(*v);
            if (c.table != t || c.name != n) { fail(Error::Code::Internal, id + " given another calculation"); return; }
            m_solution->calcs[t][n] = c;
        } else if (auto it = m_solution->calcs.find(t); it != m_solution->calcs.end()) {
            it->second.erase(n);
            if (it->second.empty()) m_solution->calcs.erase(it);
        }
    } else {
        std::string t, n;
        if (!parse_layout_id(id, t, n)) { fail(Error::Code::Internal, "not a layout: " + id); return; }
        if (v && !std::holds_alternative<core::Layout>(*v)) { fail(Error::Code::Internal, id + " given something else"); return; }
        if (v) {
            const auto& l = std::get<core::Layout>(*v);
            if (l.table != t || l.name != n) { fail(Error::Code::Internal, id + " given another layout"); return; }
            m_solution->layouts[t][n] = l;
        } else if (auto it = m_solution->layouts.find(t); it != m_solution->layouts.end()) {
            it->second.erase(n);
            if (it->second.empty()) m_solution->layouts.erase(it);
        }
    }
    if (*m_solution != before && m_save_solution && !m_save_solution()) {
        *m_solution = before;
        fail(Error::Code::Internal, "could not save the solution file");
    }
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
        if (auto sch = m_conn.introspect(); sch)   // s008: a table others point at stays
            for (const auto& in : sch->links_to(name))
                if (in.from->name != name) {
                    fail(Error::Code::Constraint, curly(in.from->name) + " links to " + curly(name) + " -- remove the link first");
                    return;
                }
        exec(drop_table_sql(q, name));
        return;
    }
    if (v->name != name) { fail(Error::Code::Internal, "record " + id + " does not hold table " + v->name); return; }
    if (live) {   // present -> another shape: ALTER (s007)
        auto df = diff_tables(*live, *v);
        if (!df) { fail(Error::Code::Unsupported, df.error()); return; }
        if (df->key)
            if (auto why = key_blocker(m_conn, m_dialect, *live, *v); !why.empty()) { fail(Error::Code::Constraint, why); return; }
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
                before.push_back({&ch, got ? std::get<FieldData>(*got) : FieldData{name, cn, primary_key_of(name), {}, {}}});
                if (!before.back().second.keys.empty() && primary_key_of(name).empty()) {
                    fail(Error::Code::Unsupported, curly(name) + " has no primary key -- Acorn can't write " +
                         curly(cn) + "'s values back");
                    return;
                }
            }
        }
        if (auto why = link_blocker(m_conn, m_dialect, *live, *v, *df); !why.empty()) {   // s008: first, in its own words
            fail(Error::Code::Constraint, why);
            return;
        }
        if (!df->changed.empty() && m_dialect == Dialect::SQLite)
            if (auto why = rebuild_blocker(m_conn, name); !why.empty()) { fail(Error::Code::Unsupported, why); return; }
        auto plan = plan_steps(m_dialect, q, *live, *v, can_place(m_conn, m_dialect, *live, *v));
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
    std::vector<std::string> fields;
    std::vector<db::Value> key;
    if (!parse_row_id(id, table, fields, key)) { fail(Error::Code::Internal, "not a record: " + id); return; }
    const Table* t = table_info(table);
    if (failed()) return;
    if (!t) { fail(Error::Code::Changed, curly(table) + " is not there any more"); return; }
    if (auto why = why_not_writable(*t); !why.empty()) { fail(Error::Code::Unsupported, why); return; }
    if (!same_key(table, *t, fields)) return;
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
    const Id id = row_id(table, pk, key);
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
    // s009 / s010: a step of box places / layouts touches only the solution --
    // no transaction, and a read-only database doesn't stop it.
    const bool local = !ids.empty() && std::all_of(ids.begin(), ids.end(), [](const auto& id) { return Store::is_local(id); });
    if (local) {
        m_store.clear_failed();
        m_txn = true;
        return {};
    }
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

std::expected<void, Error> Journal::cancel_batch() {
    if (!m_core.in_group()) return {};
    if (m_txn) return std::unexpected(Error{Error::Code::Internal, "a step is already running"});
    const auto ids = m_core.group_ids();
    if (ids.empty()) { (void)m_core.cancel_group(); return {}; }
    if (auto b = begin(ids); !b) { (void)m_core.end_group(); return b; }   // can't put back: keep it as a step
    m_store.set_allowed(ids);
    std::expected<void, Error> out;
    if (!m_core.cancel_group()) {
        out = std::unexpected(m_store.failed() ? m_store.error()
                                               : Error{Error::Code::Internal, "the script's writes could not be put back"});
        m_store.clear_failed();
    }
    rollback();
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

namespace {
// s008: what deleting `row` does to the records pointing at it, through every
// link into its table -- read before the step, so the step can declare them.
// Restrict: refused in words. Cascade: those records go too (and theirs, in
// turn). Set Empty: their link fields are emptied. The journal does each one
// itself, so undo puts every record back.
struct Fallout {
    std::vector<Store::Id> gone;      // deepest first; the record itself last
    std::vector<std::pair<Store::Id, std::vector<std::string>>> emptied;   // record, fields
};
std::expected<void, Error> fallout(Connection& c, const Schema& sch, const Store::Id& row, int depth, Fallout& out) {
    if (depth > 32) return std::unexpected(Error{Error::Code::Unsupported, "the links go deeper than Acorn follows"});
    std::string table;
    std::vector<std::string> kf;
    std::vector<Value> key;
    if (!Store::parse_row_id(row, table, kf, key)) return std::unexpected(Error{Error::Code::Internal, "not a record: " + row});
    if (std::find(out.gone.begin(), out.gone.end(), row) != out.gone.end()) return {};
    const Table* t = sch.table(table);
    if (!t) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(table)});
    std::vector<Store::Id> below;
    for (const auto& in : sch.links_to(table)) {
        const auto refs = sch.ref_columns(*in.fk);
        // the values this record holds in the fields pointed at
        std::string sel;
        for (std::size_t i = 0; i < refs.size(); ++i) sel += (i ? ", " : "") + c.quote_ident(refs[i]);
        std::string wk;
        for (std::size_t i = 0; i < kf.size(); ++i) wk += (i ? " AND " : " WHERE ") + c.quote_ident(kf[i]) + " = ?";
        auto mine = c.query("SELECT " + sel + " FROM " + c.quote_ident(table) + wk, key);
        if (!mine) return std::unexpected(mine.error());
        if (mine->rows.empty()) continue;
        const auto& vals = mine->rows[0];
        bool null = false;
        for (const auto& v : vals) if (v.is_null()) null = true;
        if (null) continue;
        const Table& ch = *in.from;
        const auto cpk = ch.primary_key();
        std::string sel2, w2;
        for (std::size_t i = 0; i < cpk.size(); ++i) sel2 += (i ? ", " : "") + c.quote_ident(cpk[i]);
        for (std::size_t i = 0; i < in.fk->columns.size(); ++i)
            w2 += (i ? " AND " : " WHERE ") + c.quote_ident(in.fk->columns[i]) + " = ?";
        auto kids = c.query("SELECT " + (cpk.empty() ? std::string("COUNT(*)") : sel2) + " FROM " + c.quote_ident(ch.name) + w2, vals);
        if (!kids) return std::unexpected(kids.error());
        if (cpk.empty()) {
            const auto& v = kids->rows[0][0];
            const std::int64_t n = v.as_int() ? *v.as_int() : (v.as_text() ? std::stoll(*v.as_text()) : 0);
            if (n > 0)
                return std::unexpected(Error{Error::Code::Unsupported, curly(ch.name) + " has no key and " +
                    std::to_string(n) + (n == 1 ? " of its records points" : " of its records point") +
                    " at this record -- Acorn couldn't bring " + (n == 1 ? "it" : "them") + " back, so it won't delete it"});
            continue;
        }
        if (kids->rows.empty()) continue;
        const std::int64_t n = static_cast<std::int64_t>(kids->rows.size());
        LinkDef l{in.fk->name, in.fk->columns, table, refs, in.fk->on_delete, in.fk->on_update};
        switch (l.deleting()) {
        case OnDelete::Restrict:
            return std::unexpected(Error{Error::Code::Constraint,
                std::to_string(n) + (n == 1 ? " record in " : " records in ") + curly(ch.name) +
                (n == 1 ? " points" : " point") + " at this record -- delete " + (n == 1 ? "it" : "them") +
                " first, or change the link to Cascade"});
        case OnDelete::Cascade:
            for (const auto& k : kids->rows) {
                const auto id = Store::row_id(ch.name, cpk, k);
                if (id.empty()) return std::unexpected(Error{Error::Code::Unsupported, curly(ch.name) + " has a key Acorn can't write by"});
                below.push_back(id);
            }
            break;
        case OnDelete::SetEmpty:
            for (const auto& k : kids->rows) {
                const auto id = Store::row_id(ch.name, cpk, k);
                if (id.empty()) return std::unexpected(Error{Error::Code::Unsupported, curly(ch.name) + " has a key Acorn can't write by"});
                out.emptied.push_back({id, in.fk->columns});
            }
            break;
        }
    }
    for (const auto& b : below)
        if (auto r = fallout(c, sch, b, depth + 1, out); !r) return r;
    out.gone.push_back(row);
    return {};
}
}  // namespace

std::expected<void, Error> delete_record(Journal& j, const Store::Id& row) {
    std::string table;
    std::vector<Value> key;
    if (!Store::parse_row_id(row, table, key))
        return std::unexpected(Error{Error::Code::Internal, "not a record: " + row});
    Connection& c = j.store().connection();
    auto sch = c.introspect();
    if (!sch) return std::unexpected(sch.error());
    Fallout fo;
    if (auto r = fallout(c, *sch, row, 0, fo); !r) return std::unexpected(r.error());
    // A record that goes isn't also emptied; each emptied record once, all its fields.
    std::vector<std::pair<Store::Id, std::vector<std::string>>> emptied;
    for (auto& [id, cols] : fo.emptied) {
        if (std::find(fo.gone.begin(), fo.gone.end(), id) != fo.gone.end()) continue;
        auto it = std::find_if(emptied.begin(), emptied.end(), [&](auto& e) { return e.first == id; });
        if (it == emptied.end()) emptied.push_back({id, cols});
        else it->second.insert(it->second.end(), cols.begin(), cols.end());
    }
    // Declared emptied first, then the gone deepest-first, the record last --
    // undo puts them back in reverse: the record, the ones below it, then the
    // emptied fields filled again.
    std::vector<Store::Id> ids;
    for (const auto& e : emptied) ids.push_back(e.first);
    ids.insert(ids.end(), fo.gone.begin(), fo.gone.end());
    return j.run("Delete Record from " + curly(table), ids, [&](Store& s) {
        if (!s.get(row) && !s.failed()) {
            s.refuse(Error{Error::Code::Changed, Store::describe(row) + " is not there any more"});
            return;
        }
        for (const auto& [id, cols] : emptied) {
            auto got = s.get(id);
            if (s.failed()) return;
            if (!got) continue;
            RowDef d = std::get<RowDef>(*got);
            for (std::size_t i = 0; i < d.columns.size(); ++i)
                if (std::find(cols.begin(), cols.end(), d.columns[i]) != cols.end()) d.values[i] = Value();
            s.put(id, d);
        }
        for (const auto& id : fo.gone) s.put(id, std::nullopt);
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
        for (auto& l : t->links)   // s008: its link goes with it
            for (auto& col : l.columns) if (col == from) col = to;
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
    if (d.key) {
        const auto lk = key_fields(live), wk = key_fields(want);
        if (lk == wk) {
            const auto wc = counting_fields(want), lc = counting_fields(live);
            if (!wc.empty()) return "Make " + curly(wc[0]) + " Count Up";
            if (!lc.empty()) return "Stop " + curly(lc[0]) + " Counting Up";
        }
        std::string f;
        for (std::size_t i = 0; i < wk.size(); ++i) f += (i ? ", " : "") + curly(wk[i]);
        return "Change Key of " + t + " to " + f;
    }
    if (d.links()) {   // s008
        if (d.link_make.size() == 1 && d.link_drop.empty()) return "Link " + t + " to " + curly(d.link_make[0]->ref_table);
        if (d.link_drop.size() == 1 && d.link_make.empty()) return "Unlink " + t + " from " + curly(d.link_drop[0]->ref_table);
        if (d.link_drop.size() == 1 && d.link_make.size() == 1 && d.link_drop[0]->columns == d.link_make[0]->columns)
            return "Change Link " + t + " to " + curly(d.link_make[0]->ref_table);
        return "Change Links of " + t;
    }
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
    {   // s008: what other tables point at, and the links this step changes
        if (auto why = link_blocker(cn, dl, out.live, want, *df); !why.empty())
            return std::unexpected(Error{Error::Code::Constraint, why});
        for (const auto* l : df->link_make) {
            if (l->deleting() == OnDelete::Cascade)
                out.words.push_back("Deleting a " + curly(l->ref_table) + " record deletes the " + curly(want.name) +
                                    " records that point at it (Ctrl+Z brings them back)");
            if (l->deleting() == OnDelete::SetEmpty)
                out.words.push_back("Deleting a " + curly(l->ref_table) + " record empties " + field_list_of(l->columns) +
                                    " in the " + curly(want.name) + " records that point at it");
        }
        if (df->links()) return out;
    }
    if (df->key) {   // s007d: the shape first, then the records
        const char q = cn.quote_ident("x").front();
        if (auto plan = plan_alter(dl, q, out.live, want); !plan)
            return std::unexpected(Error{Error::Code::Unsupported, plan.error()});
        if (auto why = key_blocker(cn, dl, out.live, want); !why.empty())
            return std::unexpected(Error{Error::Code::Constraint, why});
        const auto lk = key_fields(out.live), wk = key_fields(want);
        if (lk != wk) {
            std::string f;
            for (std::size_t i = 0; i < wk.size(); ++i)
                f += (i == 0 ? "" : i + 1 == wk.size() ? " and " : ", ") + curly(wk[i]);
            out.words.push_back("Records are found by " + f + " from now on");
            for (const auto& ch : df->changed) {
                const bool was = ch.live->pk_position > 0, is = ch.want->pk_position > 0;
                if (is && !was && ch.live->nullable && !ch.want->nullable)
                    out.words.push_back(curly(ch.want->name) + " becomes required");
                if (was && !is)
                    out.words.push_back(curly(ch.want->name) + " stays, as " +
                                        (ch.want->nullable ? "an ordinary field" : "a required field") +
                                        " -- its values are kept");
            }
        }
        for (const auto& ch : df->changed) {
            if (ch.want->auto_increment && !ch.live->auto_increment)
                out.words.push_back(curly(ch.want->name) + " counts up -- a new record gets the next number by itself");
            if (ch.live->auto_increment && !ch.want->auto_increment)
                out.words.push_back(curly(ch.want->name) + " stops counting up -- a new record needs " +
                                    (ch.want->pk_position > 0 ? "its key typed in" : "a value typed in"));
        }
        return out;
    }
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

std::expected<TableDef, std::string> keyed(const TableDef& live, const std::vector<std::string>& fields, bool counting) {
    TableDef w = live;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (!live.column(fields[i])) return std::unexpected(curly(live.name) + " has no field " + curly(fields[i]));
        for (std::size_t k = 0; k < i; ++k)
            if (fields[k] == fields[i]) return std::unexpected("the key names " + curly(fields[i]) + " twice");
    }
    for (auto& c : w.columns) {
        const auto it = std::find(fields.begin(), fields.end(), c.name);
        const int pos = it == fields.end() ? 0 : static_cast<int>(it - fields.begin()) + 1;
        const bool joins = pos > 0 && c.pk_position == 0, leaves = pos == 0 && c.pk_position > 0;
        const bool counts = counting && pos == 1 && fields.size() == 1;
        if (joins || leaves || counts != c.auto_increment) c.nullable = false;
        c.pk_position = pos;
        c.auto_increment = counts;
    }
    return w;
}

std::expected<void, Error> change_key(Journal& j, const std::string& table, const std::vector<std::string>& fields,
                                      bool counting) {
    Store& st = j.store();
    st.forget_schema();
    auto got = st.get(Store::table_id(table));
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!got) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(table)});
    auto w = keyed(std::get<TableDef>(*got), fields, counting);
    if (!w) return std::unexpected(Error{Error::Code::NotFound, w.error()});
    return design_table(j, *w);
}

// ── s008: links ─────────────────────────────────────────────────────────────
std::expected<TableDef, std::string> linked(const TableDef& live, const Table& parent,
                                            const std::vector<std::string>& fields, OnDelete on_delete) {
    if (fields.empty()) return std::unexpected("a link needs at least one field");
    for (const auto& f : fields)
        if (!live.column(f)) return std::unexpected(curly(live.name) + " has no field " + curly(f));
    if (parent.kind != Table::Kind::Table) return std::unexpected(curly(parent.name) + " is a view -- a link points at a table");
    const auto pk = parent.primary_key();
    if (pk.empty()) return std::unexpected(curly(parent.name) + " has no key -- a link points at a table's key");
    if (pk.size() != fields.size())
        return std::unexpected(curly(parent.name) + "'s key is " + field_list_of(pk) + " -- the link needs " +
                               std::to_string(pk.size()) + " field" + (pk.size() == 1 ? "" : "s") + " to match it");
    if (live.link(fields)) return std::unexpected(field_list_of(fields) + " already link" + (fields.size() == 1 ? "s" : "") +
                                                  " to " + curly(live.link(fields)->ref_table));
    TableDef w = live;
    w.links.push_back({"", fields, parent.name, pk, on_delete_sql(on_delete), "NO ACTION"});
    std::sort(w.links.begin(), w.links.end(), [](auto& a, auto& b) { return a.columns < b.columns; });
    return w;
}

namespace {
std::expected<TableDef, Error> live_def(Journal& j, const std::string& table) {
    Store& st = j.store();
    st.forget_schema();
    auto got = st.get(Store::table_id(table));
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!got) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(table)});
    return std::get<TableDef>(*got);
}
}  // namespace

std::expected<void, Error> new_link(Journal& j, const std::string& table, const std::vector<std::string>& fields,
                                    const std::string& parent, OnDelete on_delete) {
    auto live = live_def(j, table);
    if (!live) return std::unexpected(live.error());
    const Table* p = j.store().table_info(parent);
    if (j.store().failed()) { Error e = j.store().error(); j.store().clear_failed(); return std::unexpected(e); }
    if (!p) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(parent)});
    auto w = linked(*live, *p, fields, on_delete);
    if (!w) return std::unexpected(Error{Error::Code::Unsupported, w.error()});
    return design_table(j, *w);
}

std::expected<void, Error> delete_link(Journal& j, const std::string& table, const std::vector<std::string>& fields) {
    auto live = live_def(j, table);
    if (!live) return std::unexpected(live.error());
    TableDef w = *live;
    const auto n = w.links.size();
    std::erase_if(w.links, [&](const LinkDef& l) { return l.columns == fields; });
    if (w.links.size() == n)
        return std::unexpected(Error{Error::Code::NotFound, field_list_of(fields) + " in " + curly(table) + " link to nothing"});
    return design_table(j, w);
}

std::expected<void, Error> change_link(Journal& j, const std::string& table, const std::vector<std::string>& fields,
                                       OnDelete on_delete) {
    auto live = live_def(j, table);
    if (!live) return std::unexpected(live.error());
    TableDef w = *live;
    LinkDef* l = nullptr;
    for (auto& x : w.links) if (x.columns == fields) l = &x;
    if (!l) return std::unexpected(Error{Error::Code::NotFound, field_list_of(fields) + " in " + curly(table) + " link to nothing"});
    if (l->deleting() == on_delete) return {};   // no change: no step
    l->on_delete = on_delete_sql(on_delete);
    l->name.clear();
    return design_table(j, w);
}

std::expected<std::vector<Store::Id>, Error> unmatched(Journal& j, const std::string& table,
                                                       const std::vector<std::string>& fields,
                                                       const std::string& parent, std::size_t limit) {
    Connection& c = j.store().connection();
    auto sch = c.introspect();
    if (!sch) return std::unexpected(sch.error());
    const Table* t = sch->table(table);
    const Table* p = sch->table(parent);
    if (!t || !p) return std::unexpected(Error{Error::Code::NotFound, "there is no table " + curly(t ? parent : table)});
    const auto pk = t->primary_key(), ppk = p->primary_key();
    if (pk.empty()) return std::unexpected(Error{Error::Code::Unsupported, curly(table) + " has no key -- its records can't be named"});
    if (ppk.size() != fields.size())
        return std::unexpected(Error{Error::Code::Unsupported, curly(parent) + "'s key is " + field_list_of(ppk)});
    const LinkDef l{"", fields, parent, ppk};
    std::string sel, order;
    const std::string a = c.quote_ident("acorn_c");
    for (std::size_t i = 0; i < pk.size(); ++i) {
        sel += (i ? ", " : "") + a + "." + c.quote_ident(pk[i]);
        order += (i ? ", " : "") + std::to_string(i + 1);
    }
    auto r = c.query("SELECT " + sel + unmatched_where(c, table, l) + " ORDER BY " + order);
    if (!r) return std::unexpected(r.error());
    std::vector<Store::Id> out;
    for (const auto& row : r->rows) {
        if (out.size() >= limit) break;
        out.push_back(Store::row_id(table, pk, row));
    }
    return out;
}

std::expected<void, Error> move_box(Journal& j, const std::string& table, double x, double y) {
    const auto id = Store::box_id(table);
    return j.run("Move " + curly(table), {id}, [&](Store& s) { s.put(id, BoxDef{table, x, y}); });
}

std::expected<void, Error> save_layout(Journal& j, const core::Layout& l) {
    if (auto why = core::check_layout(l); !why.empty()) return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto id = Store::layout_id(l.table, l.name);
    const auto now = j.store().get(id);
    if (now && std::get<core::Layout>(*now) == l) return {};   // already kept: no step
    return j.run((now ? "Change Layout " : "New Layout ") + curly(l.name), {id}, [&](Store& s) { s.put(id, l); });
}

std::expected<void, Error> delete_layout(Journal& j, const std::string& table, const std::string& name) {
    const auto id = Store::layout_id(table, name);
    if (!j.store().get(id))
        return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no layout " + curly(name)});
    return j.run("Delete Layout " + curly(name), {id}, [&](Store& s) { s.put(id, std::nullopt); });
}

std::expected<void, Error> rename_layout(Journal& j, const std::string& table, const std::string& from,
                                         const std::string& to) {
    const auto a = Store::layout_id(table, from);
    const auto now = j.store().get(a);
    if (!now) return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no layout " + curly(from)});
    if (from == to) return {};   // no change: no step
    core::Layout l = std::get<core::Layout>(*now);
    l.name = to;
    if (auto why = core::check_layout(l); !why.empty()) return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto b = Store::layout_id(table, to);
    if (j.store().get(b))
        return std::unexpected(Error{Error::Code::Exists, curly(table) + " already has a layout " + curly(to)});
    return j.run("Rename Layout " + curly(from) + " to " + curly(to), {a, b}, [&](Store& s) {
        s.put(a, std::nullopt);
        s.put(b, l);
    });
}

std::expected<void, Error> save_calc(Journal& j, const core::Calc& c, const std::string& was) {
    Store& st = j.store();
    if (!st.solution()) return std::unexpected(Error{Error::Code::Unsupported, "there is no solution to keep calculations in"});
    st.forget_schema();
    const Table* t = st.table_info(c.table);
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!t) return std::unexpected(Error{Error::Code::NotFound, "there's no table " + curly(c.table)});
    std::vector<std::string> fields;
    for (const auto& col : t->columns) fields.push_back(col.name);
    for (const auto& [n, x] : st.solution()->summaries_of(c.table)) {   // s014b: read like fields
        if (n == c.name) return std::unexpected(Error{Error::Code::Exists, curly(c.table) + " already has a summary " + curly(n)});
        fields.push_back(n);
    }
    auto calcs = st.solution()->calcs_of(c.table);
    const bool renaming = !was.empty() && was != c.name;
    if (!was.empty() && !calcs.contains(was))
        return std::unexpected(Error{Error::Code::NotFound, curly(c.table) + " has no calculation " + curly(was)});
    if (renaming) {
        if (calcs.contains(c.name))
            return std::unexpected(Error{Error::Code::Exists, curly(c.table) + " already has a calculation " + curly(c.name)});
        calcs.erase(was);
    }
    if (auto why = core::check_calc(c, fields, calcs); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto id = Store::calc_id(c.table, c.name);
    if (!renaming) {
        const auto now = st.get(id);
        if (now && std::get<core::Calc>(*now) == c) return {};   // already kept: no step
        return j.run((now ? "Change Calculation " : "New Calculation ") + curly(c.name), {id},
                     [&](Store& s) { s.put(id, c); });
    }
    const auto old = Store::calc_id(c.table, was);
    return j.run("Change Calculation " + curly(was), {old, id}, [&](Store& s) {
        s.put(old, std::nullopt);
        s.put(id, c);
    });
}

std::expected<void, Error> save_summary(Journal& j, const core::Summary& x, const std::string& was) {
    Store& st = j.store();
    if (!st.solution()) return std::unexpected(Error{Error::Code::Unsupported, "there is no solution to keep summaries in"});
    st.forget_schema();
    const Table* t = st.table_info(x.table);
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!t) return std::unexpected(Error{Error::Code::NotFound, "there's no table " + curly(x.table)});
    std::vector<std::string> fields, taken;
    for (const auto& col : t->columns) fields.push_back(col.name);
    for (const auto& [n, c] : st.solution()->calcs_of(x.table)) taken.push_back(n);
    const auto& sums = st.solution()->summaries_of(x.table);
    if (!was.empty() && !sums.contains(was))
        return std::unexpected(Error{Error::Code::NotFound, curly(x.table) + " has no summary " + curly(was)});
    const bool renaming = !was.empty() && was != x.name;
    for (const auto& [n, s] : sums) if (n != (was.empty() ? x.name : was)) taken.push_back(n);
    if (auto why = core::check_summary(x, fields, taken); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    const Column* col = t->column(x.field);
    if (col->type == FieldType::Container)
        return std::unexpected(Error{Error::Code::Unsupported, curly(x.field) + " holds binary data -- it can't be summarised"});
    if ((x.op == core::SummaryOp::Total || x.op == core::SummaryOp::Average) &&
        col->type != FieldType::Integer && col->type != FieldType::Number)
        return std::unexpected(Error{Error::Code::Unsupported, std::string(core::summary_op_label(x.op)) + " needs a number -- " +
                                                               curly(x.field) + " is " + to_string(col->type)});
    const auto id = Store::summary_id(x.table, x.name);
    if (!renaming) {
        const auto now = st.get(id);
        if (now && std::get<core::Summary>(*now) == x) return {};
        return j.run((now ? "Change Summary " : "New Summary ") + curly(x.name), {id}, [&](Store& s) { s.put(id, x); });
    }
    const auto old = Store::summary_id(x.table, was);
    return j.run("Change Summary " + curly(was), {old, id}, [&](Store& s) {
        s.put(old, std::nullopt);
        s.put(id, x);
    });
}

std::expected<void, Error> save_value_list(Journal& j, const core::ValueList& v, const std::string& was) {
    Store& st = j.store();
    if (!st.solution()) return std::unexpected(Error{Error::Code::Unsupported, "there is no solution to keep value lists in"});
    st.forget_schema();
    const Table* t = st.table_info(v.table);
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!t) return std::unexpected(Error{Error::Code::NotFound, "there's no table " + curly(v.table)});
    const auto& lists = st.solution()->lists_of(v.table);
    if (!was.empty() && !lists.contains(was))
        return std::unexpected(Error{Error::Code::NotFound, curly(v.table) + " has no value list " + curly(was)});
    const bool renaming = !was.empty() && was != v.name;
    std::vector<std::string> taken;
    for (const auto& [n, x] : lists) if (n != (was.empty() ? v.name : was)) taken.push_back(n);
    std::vector<std::string> from_fields;
    const Table* from = nullptr;
    if (!v.typed() && !v.from.empty()) {
        from = st.table_info(v.from);
        if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
        if (from) for (const auto& col : from->columns) from_fields.push_back(col.name);
    }
    if (auto why = core::check_value_list(v, taken, from ? &from_fields : nullptr); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    if (from)
        for (const auto* f : {&v.field, &v.shows})
            if (!f->empty() && from->column(*f)->type == FieldType::Container)
                return std::unexpected(Error{Error::Code::Unsupported, curly(*f) + " holds binary data -- it can't be offered in a list"});
    const auto id = Store::list_id(v.table, v.name);
    if (!renaming) {
        const auto now = st.get(id);
        if (now && std::get<core::ValueList>(*now) == v) return {};   // already kept: no step
        return j.run((now ? "Change Value List " : "New Value List ") + curly(v.name), {id}, [&](Store& s) { s.put(id, v); });
    }
    const auto old = Store::list_id(v.table, was);
    return j.run("Change Value List " + curly(was), {old, id}, [&](Store& s) {
        s.put(old, std::nullopt);
        s.put(id, v);
    });
}

std::expected<void, Error> save_script(Journal& j, const core::Script& sc, const std::string& was) {
    Store& st = j.store();
    if (!st.solution()) return std::unexpected(Error{Error::Code::Unsupported, "there is no solution to keep scripts in"});
    st.forget_schema();
    const Table* t = st.table_info(sc.table);
    if (st.failed()) { Error e = st.error(); st.clear_failed(); return std::unexpected(e); }
    if (!t) return std::unexpected(Error{Error::Code::NotFound, "there's no table " + curly(sc.table)});
    const auto& all = st.solution()->scripts_of(sc.table);
    if (!was.empty() && !all.contains(was))
        return std::unexpected(Error{Error::Code::NotFound, curly(sc.table) + " has no script " + curly(was)});
    const bool renaming = !was.empty() && was != sc.name;
    std::vector<std::string> taken, fields, layouts;
    for (const auto& [n, x] : all) if (n != (was.empty() ? sc.name : was)) taken.push_back(n);
    for (const auto& c : t->columns) fields.push_back(c.name);
    if (auto ls = st.solution()->layouts.find(sc.table); ls != st.solution()->layouts.end())
        for (const auto& [n, l] : ls->second) layouts.push_back(n);
    if (auto why = core::check_script(sc, taken, fields, layouts); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    const auto id = Store::script_id(sc.table, sc.name);
    if (!renaming) {
        const auto now = st.get(id);
        if (now && std::get<core::Script>(*now) == sc) return {};   // already kept: no step
        return j.run((now ? "Change Script " : "New Script ") + curly(sc.name), {id}, [&](Store& s) { s.put(id, sc); });
    }
    const auto old = Store::script_id(sc.table, was);
    // s017b: the table's buttons that ran `was` run the new name -- in the
    // same step, so Ctrl+Z puts the name and the buttons back together.
    std::vector<Store::Id> ids{old, id};
    std::vector<core::Layout> moved;
    if (auto ls = st.solution()->layouts.find(sc.table); ls != st.solution()->layouts.end())
        for (const auto& [n, l] : ls->second) {
            core::Layout c = l;
            if (core::rename_script_on(c, was, sc.name) == 0) continue;
            ids.push_back(Store::layout_id(sc.table, n));
            moved.push_back(std::move(c));
        }
    return j.run("Change Script " + curly(was), ids, [&](Store& s) {
        s.put(old, std::nullopt);
        s.put(id, sc);
        for (const auto& l : moved) s.put(Store::layout_id(l.table, l.name), l);
    });
}

std::expected<void, Error> delete_script(Journal& j, const std::string& table, const std::string& name) {
    const auto id = Store::script_id(table, name);
    if (!j.store().get(id))
        return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no script " + curly(name)});
    return j.run("Delete Script " + curly(name), {id}, [&](Store& s) { s.put(id, std::nullopt); });
}

std::expected<void, Error> delete_value_list(Journal& j, const std::string& table, const std::string& name) {
    const auto id = Store::list_id(table, name);
    if (!j.store().get(id))
        return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no value list " + curly(name)});
    return j.run("Delete Value List " + curly(name), {id}, [&](Store& s) { s.put(id, std::nullopt); });
}

std::expected<void, Error> delete_summary(Journal& j, const std::string& table, const std::string& name) {
    const auto id = Store::summary_id(table, name);
    if (!j.store().get(id))
        return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no summary " + curly(name)});
    return j.run("Delete Summary " + curly(name), {id}, [&](Store& s) { s.put(id, std::nullopt); });
}

std::expected<void, Error> delete_calc(Journal& j, const std::string& table, const std::string& name) {
    const auto id = Store::calc_id(table, name);
    if (!j.store().get(id))
        return std::unexpected(Error{Error::Code::NotFound, curly(table) + " has no calculation " + curly(name)});
    return j.run("Delete Calculation " + curly(name), {id}, [&](Store& s) { s.put(id, std::nullopt); });
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
    const TableDef& live = std::get<TableDef>(*got);
    auto plan = plan_alter(st.dialect(), q, live, want, can_place(st.connection(), st.dialect(), live, want));
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
// s007d -- the walk's fixture for the key: a counting key, a text code that
// is filled and different in every record, and a number. Made through the door.
constexpr const char* kKey = "acorn_verb_key";
std::expected<void, Error> key_fixture(Journal& j) {
    const Dialect d = j.store().dialect();
    j.store().forget_schema();
    if (!j.store().table_info(kKey)) {
        j.store().clear_failed();
        TableDef t;
        t.name = kKey;
        t.columns = {
            {"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
            {"code", decl_for(d, FieldType::Text), true, std::nullopt, 0, false},
            {"n", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false},
        };
        if (auto r = new_table(j, t); !r) return r;
    }
    Connection& c = j.store().connection();
    auto n = c.query("SELECT COUNT(*) FROM " + c.quote_ident(kKey));
    if (!n || n->rows.empty()) return std::unexpected(Error{Error::Code::Internal, "could not count the key fixture"});
    const auto& v = n->rows[0][0];
    const std::int64_t k = v.as_int() ? *v.as_int() : (v.as_text() ? std::stoll(*v.as_text()) : 0);
    if (auto r = new_record(j, kKey, {"code", "n"}, {Value("c" + std::to_string(k + 1)), Value(k + 1)}); !r)
        return std::unexpected(r.error());
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
    return Store::row_id(kRows, {"id"}, {Value(*id)});
}
}  // namespace

namespace {
// s008 -- the walk's fixture for links: a parent with a counting key and a
// child whose "parent_id" points at it (or not yet), each with records --
// made through the door. `link`: whether the child should be linked, and how.
constexpr const char* kParent = "acorn_verb_parent";
constexpr const char* kChild = "acorn_verb_child";
std::expected<void, Error> link_fixture(Journal& j, std::optional<OnDelete> link) {
    const Dialect d = j.store().dialect();
    j.store().forget_schema();
    if (!j.store().table_info(kParent)) {
        j.store().clear_failed();
        TableDef p;
        p.name = kParent;
        p.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                     {"name", decl_for(d, FieldType::Text), true, std::nullopt, 0, false}};
        if (auto r = new_table(j, p); !r) return r;
        TableDef c;
        c.name = kChild;
        c.columns = {{"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true},
                     {"parent_id", decl_for(d, FieldType::Integer), true, std::nullopt, 0, false},
                     {"note", decl_for(d, FieldType::Text), true, std::nullopt, 0, false}};
        if (auto r = new_table(j, c); !r) return r;
    }
    // One more parent with two children pointing at it (steps, like the others).
    auto pid = new_record(j, kParent, {"name"}, {Value("parent")});
    if (!pid) return std::unexpected(pid.error());
    std::string t;
    std::vector<Value> key;
    (void)Store::parse_row_id(*pid, t, key);
    for (const char* n : {"first", "second"})
        if (auto r = new_record(j, kChild, {"parent_id", "note"}, {key[0], Value(n)}); !r) return std::unexpected(r.error());
    auto live = live_def(j, kChild);
    if (!live) return std::unexpected(live.error());
    const LinkDef* l = live->link({"parent_id"});
    if (l && (!link || l->deleting() != *link))
        if (auto r = delete_link(j, kChild, {"parent_id"}); !r) return r;
    if (link && (!l || l->deleting() != *link))
        if (auto r = new_link(j, kChild, {"parent_id"}, kParent, *link); !r) return r;
    return {};
}
// The newest parent record (the one the fixture just made).
std::expected<Store::Id, Error> newest_parent(Journal& j) {
    Connection& c = j.store().connection();
    auto r = c.query("SELECT MAX(" + c.quote_ident("id") + ") FROM " + c.quote_ident(kParent));
    if (!r) return std::unexpected(r.error());
    if (r->rows.empty() || r->rows[0][0].is_null()) return std::unexpected(Error{Error::Code::NotFound, "no record"});
    return Store::row_id(kParent, {"id"}, {r->rows[0][0]});
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
        // s007d: the key moves to a text field (counting stops); then a key
        // of two fields (the counting field joins another, and stops counting).
        {"Change Key", [](Journal& j) { return change_key(j, kKey, {"code"}, false); }, key_fixture},
        {"Change Key (two fields)", [](Journal& j) { return change_key(j, kKey, {"code", "id"}, false); }, key_fixture},
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
        // s008 -- links
        {"New Link", [](Journal& j) { return new_link(j, kChild, {"parent_id"}, kParent); },
         [](Journal& j) { return link_fixture(j, std::nullopt); }},
        {"Change Link", [](Journal& j) { return change_link(j, kChild, {"parent_id"}, OnDelete::Cascade); },
         [](Journal& j) { return link_fixture(j, OnDelete::Restrict); }},
        {"Delete Link", [](Journal& j) { return delete_link(j, kChild, {"parent_id"}); },
         [](Journal& j) { return link_fixture(j, OnDelete::Restrict); }},
        {"Delete Record (Cascade)", [](Journal& j) -> std::expected<void, Error> {
             auto row = newest_parent(j);
             if (!row) return std::unexpected(row.error());
             return delete_record(j, *row);
         }, [](Journal& j) { return link_fixture(j, OnDelete::Cascade); }},
        // s009 -- the solution: a box moved in the Graph (no database write).
        {"Move Box", [](Journal& j) { return move_box(j, kDesign, 40, 80); }, design_fixture, true},
        // s010 -- layouts, in the solution too.
        {"Save Layout", [](Journal& j) {
             return save_layout(j, core::default_layout(kDesign, {"id", "title", "n"}));
         }, design_fixture, true},
        {"Delete Layout", [](Journal& j) { return delete_layout(j, kDesign, "walk"); },
         [](Journal& j) -> std::expected<void, Error> {
             auto l = core::default_layout(kDesign, {"title"});
             l.name = "walk";
             return save_layout(j, l);
         }, true},
        {"Rename Layout", [](Journal& j) { return rename_layout(j, kDesign, "walk to rename", "walk renamed"); },
         [](Journal& j) -> std::expected<void, Error> {
             auto l = core::default_layout(kDesign, {"title"});
             l.name = "walk to rename";
             return save_layout(j, l);
         }, true},
        // s014a -- calculation fields, in the solution too.
        {"New Calculation", [](Journal& j) { return save_calc(j, core::Calc{kDesign, "twice", "n * 2"}); },
         design_fixture, true},
        {"Change Calculation", [](Journal& j) { return save_calc(j, core::Calc{kDesign, "loud", "Upper(title) & \"!\""}); },
         [](Journal& j) { return design_fixture(j).and_then([&] { return save_calc(j, core::Calc{kDesign, "loud", "Upper(title)"}); }); }, true},
        {"Rename Calculation", [](Journal& j) { return save_calc(j, core::Calc{kDesign, "renamed", "n + 1"}, "to_rename"); },
         [](Journal& j) { return design_fixture(j).and_then([&] { return save_calc(j, core::Calc{kDesign, "to_rename", "n + 1"}); }); }, true},
        {"Delete Calculation", [](Journal& j) { return delete_calc(j, kDesign, "gone"); },
         [](Journal& j) { return design_fixture(j).and_then([&] { return save_calc(j, core::Calc{kDesign, "gone", "Length(title)"}); }); }, true},
        // s014b -- summaries, in the solution too.
        {"New Summary", [](Journal& j) { return save_summary(j, core::Summary{kDesign, "total_n", core::SummaryOp::Total, "n"}); },
         design_fixture, true},
        {"Change Summary", [](Journal& j) {
             return save_summary(j, core::Summary{kDesign, "most", core::SummaryOp::Max, "title"});
         }, [](Journal& j) { return design_fixture(j).and_then([&] {
             return save_summary(j, core::Summary{kDesign, "most", core::SummaryOp::Max, "n"}); }); }, true},
        {"Delete Summary", [](Journal& j) { return delete_summary(j, kDesign, "how_many"); },
         [](Journal& j) { return design_fixture(j).and_then([&] {
             return save_summary(j, core::Summary{kDesign, "how_many", core::SummaryOp::Count, "title"}); }); }, true},
        // s014c -- value lists, in the solution too.
        {"New Value List", [](Journal& j) { return save_value_list(j, core::ValueList{kDesign, "kinds", {"a", "b"}}); },
         design_fixture, true},
        {"Change Value List", [](Journal& j) {
             return save_value_list(j, core::ValueList{kDesign, "titles", {}, kDesign, "title"});
         }, [](Journal& j) { return design_fixture(j).and_then([&] {
             return save_value_list(j, core::ValueList{kDesign, "titles", {"x"}}); }); }, true},
        {"Delete Value List", [](Journal& j) { return delete_value_list(j, kDesign, "gone"); },
         [](Journal& j) { return design_fixture(j).and_then([&] {
             return save_value_list(j, core::ValueList{kDesign, "gone", {"one"}}); }); }, true},
        // s017a -- scripts, in the solution too.
        {"New Script", [](Journal& j) {
             return save_script(j, core::Script{kDesign, "tidy", {{core::StepKind::Sort, "title", "", true},
                                                                 {core::StepKind::ShowAll, "", "", false}}});
         }, design_fixture, true},
        {"Change Script", [](Journal& j) {
             return save_script(j, core::Script{kDesign, "tidy", {{core::StepKind::Find, "title", "=", false}}});
         }, [](Journal& j) { return design_fixture(j).and_then([&] {
             return save_script(j, core::Script{kDesign, "tidy", {{core::StepKind::ShowAll, "", "", false}}}); }); }, true},
        {"Delete Script", [](Journal& j) { return delete_script(j, kDesign, "gone"); },
         [](Journal& j) { return design_fixture(j).and_then([&] {
             return save_script(j, core::Script{kDesign, "gone", {{core::StepKind::Preview, "", "", false}}}); }); }, true},
        {"Delete Record (Set Empty)", [](Journal& j) -> std::expected<void, Error> {
             auto row = newest_parent(j);
             if (!row) return std::unexpected(row.error());
             return delete_record(j, *row);
         }, [](Journal& j) { return link_fixture(j, OnDelete::SetEmpty); }},
    };
    return kVerbs;
}

}  // namespace verbs

}  // namespace acorn::db
