#include "db/Browse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// db/Browse.cpp -- read include/db/Browse.hpp first.

namespace acorn::db {

namespace {

// "WHERE a = ? AND b = ?" over `link` (empty: "").
std::string where_of(const Connection& c, const std::vector<std::string>& link) {
    std::string w;
    for (std::size_t i = 0; i < link.size(); ++i) w += (i ? " AND " : " WHERE ") + c.quote_ident(link[i]) + " = ?";
    return w;
}

std::string select_sql(const Connection& c, const Table& t, const PageRequest& r, const std::string& where) {
    std::string s = "SELECT ";
    for (std::size_t i = 0; i < t.columns.size(); ++i) s += (i ? ", " : "") + c.quote_ident(t.columns[i].name);
    s += " FROM " + c.quote_ident(t.name) + where;
    std::vector<std::string> order;
    const auto pk = t.primary_key();
    if (!r.sort.empty() && t.column(r.sort))
        order.push_back(c.quote_ident(r.sort) + (r.descending ? " DESC" : " ASC"));
    for (const auto& k : pk)
        if (k != r.sort) order.push_back(c.quote_ident(k) + (r.descending && r.sort.empty() ? " DESC" : " ASC"));
    if (!order.empty()) {
        s += " ORDER BY ";
        for (std::size_t i = 0; i < order.size(); ++i) s += (i ? ", " : "") + order[i];
    }
    // Integers Acorn computed, not user text: safe to write in.
    s += " LIMIT " + std::to_string(std::max<std::int64_t>(r.limit, 1)) +
         " OFFSET " + std::to_string(std::max<std::int64_t>(r.offset, 0));
    return s;
}

std::expected<Page, Error> read_sql_where(Connection& c, const Table& t, const PageRequest& r,
                                          const std::string& where, const std::vector<Value>& values) {
    Page p;
    p.offset = std::max<std::int64_t>(r.offset, 0);
    for (const auto& col : t.columns) p.columns.push_back(col.name);
    if (t.columns.empty()) return p;

    auto n = c.query("SELECT COUNT(*) FROM " + c.quote_ident(t.name) + where, values);
    if (!n) return std::unexpected(n.error());
    if (!n->rows.empty()) {
        const auto& v = n->rows[0][0];
        if (auto i = v.as_int()) p.total = *i;
        else if (auto txt = v.as_text()) p.total = std::stoll(*txt);
    }
    auto q = c.query(select_sql(c, t, r, where), values);
    if (!q) return std::unexpected(q.error());
    p.rows = std::move(q->rows);

    const bool keyed = Store::why_not_writable(t).empty();
    const auto pk = t.primary_key();
    std::vector<std::size_t> at;
    for (const auto& k : pk)
        for (std::size_t i = 0; i < t.columns.size(); ++i)
            if (t.columns[i].name == k) at.push_back(i);
    for (const auto& row : p.rows) {
        if (!keyed || at.size() != pk.size()) { p.ids.emplace_back(); continue; }
        std::vector<Value> key;
        for (auto i : at) key.push_back(i < row.size() ? row[i] : Value());
        p.ids.push_back(Store::row_id(t.name, pk, key));
    }
    return p;
}

std::expected<Page, Error> read_where(Connection& c, const Table& t, const PageRequest& r,
                                      const std::vector<std::string>& link, const std::vector<Value>& values) {
    return read_sql_where(c, t, r, where_of(c, link), values);
}

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// The operator a box starts with ("" = none) and the rest, trimmed.
std::pair<std::string, std::string> split_op(const std::string& typed) {
    const std::string s = trimmed(typed);
    for (const char* op : {"<=", ">=", "=", "<", ">"}) {
        const std::string o = op;
        if (s.rfind(o, 0) == 0) return {o, trimmed(s.substr(o.size()))};
    }
    return {"", s};
}

}  // namespace

std::expected<FindQuery, std::string> find_query(const Connection& c, const Table& t, const std::vector<Criterion>& crit,
                                                 const Schema* sch) {
    FindQuery q;
    for (const auto& k : crit) {
        if (trimmed(k.typed).empty()) continue;
        // s013b: whose field -- this table's, or another's through a link.
        const Table* in = &t;
        if (!k.table.empty()) {
            in = sch ? sch->table(k.table) : nullptr;
            if (!in) return std::unexpected("there is no table “" + k.table + "”");
            if (!t.column(k.outer)) return std::unexpected("“" + t.name + "” has no field “" + k.outer + "”");
            if (!in->column(k.inner)) return std::unexpected("“" + in->name + "” has no field “" + k.inner + "”");
        }
        const Column* col = in->column(k.field);
        if (!col) return std::unexpected("“" + in->name + "” has no field “" + k.field + "”");
        const std::string name = "\u201C" + (in == &t ? col->name : in->name + "." + col->name) + "\u201D";
        if (col->type == FieldType::Container) return std::unexpected(name + " holds binary data -- it can't be searched");
        const std::string f = c.quote_ident(col->name);
        auto [op, rest] = split_op(k.typed);
        std::string cond;
        if (op == "=" && rest.empty()) {
            cond = col->type == FieldType::Text ? "(" + f + " IS NULL OR " + f + " = '')" : f + " IS NULL";
        } else if (op.empty() && col->type == FieldType::Text) {
            // Contains, any case: the field lowered by the engine, the words here
            // (ASCII). '!' escapes LIKE's own % and _ so they are only themselves.
            std::string pat = "%";
            for (unsigned char ch : rest) {
                if (ch == '%' || ch == '_' || ch == '!') pat += '!';
                pat += static_cast<char>(std::tolower(ch));
            }
            pat += "%";
            cond = "LOWER(" + f + ") LIKE ? ESCAPE '!'";
            q.values.emplace_back(pat);
        } else {
            if (rest.empty()) return std::unexpected(name + ": “" + op + "” needs something after it");
            auto v = parse_field(rest, *col);
            if (!v) return std::unexpected(v.error());
            cond = f + " " + (op.empty() ? std::string("=") : op) + " ?";
            q.values.push_back(std::move(*v));
        }
        if (in != &t)   // the link: this record's outer among the matching records' inner
            cond = c.quote_ident(k.outer) + " IN (SELECT " + c.quote_ident(k.inner) + " FROM " + c.quote_ident(in->name) +
                   " WHERE " + cond + ")";
        q.where += (q.where.empty() ? " WHERE " : " AND ") + cond;
    }
    if (q.where.empty()) return std::unexpected("Type what to look for in at least one box");
    return q;
}

std::expected<Page, Error> read_found(Connection& c, const Table& t, const std::vector<Criterion>& crit,
                                      const PageRequest& r, const Schema* sch) {
    auto q = find_query(c, t, crit, sch);
    if (!q) return std::unexpected(Error{Error::Code::Unsupported, q.error()});
    return read_sql_where(c, t, r, q->where, q->values);
}

std::string find_summary(const std::vector<Criterion>& crit) {
    std::string out;
    for (const auto& k : crit) {
        if (trimmed(k.typed).empty()) continue;
        auto [op, rest] = split_op(k.typed);
        const std::string who = k.table.empty() ? k.field : k.table + "." + k.field;   // s013b
        std::string said;
        if (op == "=" && rest.empty()) said = who + " is empty";
        else if (op.empty())           said = who + " has “" + rest + "”";
        else                           said = who + " " + op + " " + rest;
        out += (out.empty() ? "" : " · ") + said;
    }
    return out;
}

std::string page_sql(const Connection& c, const Table& t, const PageRequest& r) { return select_sql(c, t, r, {}); }

std::expected<Page, Error> read_page(Connection& c, const Table& t, const PageRequest& r) {
    return read_where(c, t, r, {}, {});
}

std::string related_sql(const Connection& c, const Table& t, const std::vector<std::string>& link, const PageRequest& r) {
    return select_sql(c, t, r, where_of(c, link));
}

std::expected<Page, Error> read_related(Connection& c, const Table& t, const std::vector<std::string>& link,
                                        const std::vector<Value>& values, const PageRequest& r) {
    if (link.empty() || link.size() != values.size())
        return std::unexpected(Error{Error::Code::Internal, "a portal's link and its values don't match"});
    for (const auto& v : link)
        if (!t.column(v)) return std::unexpected(Error{Error::Code::NotFound, "“" + t.name + "” has no field “" + v + "”"});
    for (const auto& v : values)
        if (v.is_null()) {   // nothing points at a record with no key
            Page p;
            for (const auto& col : t.columns) p.columns.push_back(col.name);
            return p;
        }
    return read_where(c, t, r, link, values);
}

core::CalcValue calc_value(const Value& v, FieldType t) {
    switch (v.kind()) {
    case Value::Kind::Null:    return {};
    case Value::Kind::Integer: return static_cast<double>(*v.as_int());
    case Value::Kind::Real:    return *v.as_real();
    case Value::Kind::Blob:    return {};
    case Value::Kind::Text: {
        const std::string& s = *v.as_text();
        if (t == FieldType::Boolean) {
            if (s == "t" || s == "true" || s == "1") return 1.0;
            if (s == "f" || s == "false" || s == "0") return 0.0;
        }
        if (t == FieldType::Integer || t == FieldType::Number) {
            char* end = nullptr;
            const double d = std::strtod(s.c_str(), &end);
            if (!s.empty() && end && *end == '\0' && std::isfinite(d)) return d;
        }
        return s;
    }
    }
    return {};
}

std::map<std::string, core::CalcOutcome> row_calcs(const Table& t, const Row& row,
                                                   const std::map<std::string, core::Calc>& calcs,
                                                   const std::string& today,
                                                   const std::map<std::string, core::CalcValue>* extra) {
    if (calcs.empty()) return {};
    core::CalcLookup look = [&](const std::string& name) -> std::optional<core::CalcValue> {
        for (std::size_t i = 0; i < t.columns.size() && i < row.size(); ++i)
            if (t.columns[i].name == name) return calc_value(row[i], t.columns[i].type);
        if (extra)
            if (auto it = extra->find(name); it != extra->end()) return it->second;
        return std::nullopt;
    };
    return core::evaluate_all(calcs, look, today);
}

std::string summary_sql(const Connection& c, const Table& t, const std::vector<core::Summary>& sums,
                        const std::string& where) {
    std::string s = "SELECT ";
    for (std::size_t i = 0; i < sums.size(); ++i) {
        const auto f = c.quote_ident(sums[i].field);
        const char* fn = "COUNT";
        switch (sums[i].op) {
            case core::SummaryOp::Count:   fn = "COUNT"; break;
            case core::SummaryOp::Total:   fn = "SUM"; break;
            case core::SummaryOp::Average: fn = "AVG"; break;
            case core::SummaryOp::Min:     fn = "MIN"; break;
            case core::SummaryOp::Max:     fn = "MAX"; break;
        }
        s += (i ? ", " : "") + std::string(fn) + "(" + f + ")";
    }
    return s + " FROM " + c.quote_ident(t.name) + where;
}

std::expected<std::map<std::string, core::CalcValue>, Error>
read_summaries(Connection& c, const Table& t, const std::map<std::string, core::Summary>& sums,
               const std::vector<Criterion>& crit, const Schema* sch) {
    std::map<std::string, core::CalcValue> out;
    std::vector<core::Summary> asked;
    for (const auto& [n, x] : sums) {
        const Column* col = t.column(x.field);
        if (col && col->type != FieldType::Container) asked.push_back(x);
    }
    if (asked.empty()) return out;
    FindQuery q;
    if (!crit.empty()) {
        auto fq = find_query(c, t, crit, sch);
        if (!fq) return std::unexpected(Error{Error::Code::Unsupported, fq.error()});
        q = std::move(*fq);
    }
    auto r = c.query(summary_sql(c, t, asked, q.where), q.values);
    if (!r) return std::unexpected(r.error());
    if (r->rows.empty()) return out;
    const auto& row = r->rows.front();
    for (std::size_t i = 0; i < asked.size() && i < row.size(); ++i) {
        const auto& x = asked[i];
        const bool counted = x.op == core::SummaryOp::Count || x.op == core::SummaryOp::Total ||
                             x.op == core::SummaryOp::Average;
        // A server's SUM / AVG of an exact number comes as text ("12.50"): a number.
        out[x.name] = calc_value(row[i], counted ? FieldType::Number : t.column(x.field)->type);
    }
    return out;
}

std::string cell_text(const Value& v, FieldType t) {
    switch (v.kind()) {
    case Value::Kind::Null: return {};
    case Value::Kind::Integer:
        if (t == FieldType::Boolean) return *v.as_int() ? "Yes" : "No";
        return std::to_string(*v.as_int());
    case Value::Kind::Real: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.15g", *v.as_real());
        return buf;
    }
    case Value::Kind::Text: {
        const std::string& s = *v.as_text();
        if (t == FieldType::Boolean) {
            if (s == "t" || s == "true" || s == "1") return "Yes";
            if (s == "f" || s == "false" || s == "0") return "No";
        }
        return s;
    }
    case Value::Kind::Blob: {
        const auto n = v.as_blob()->size();
        return "(" + std::to_string(n) + (n == 1 ? " byte)" : " bytes)");
    }
    }
    return {};
}

std::expected<Value, std::string> parse_field(const std::string& typed, const Column& c) {
    const std::string q = "\u201C" + c.name + "\u201D";
    // Trim only for numbers: text keeps its spaces.
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    if (c.type == FieldType::Container) return std::unexpected(q + " holds binary data -- not typed here");
    const bool text_like = c.type != FieldType::Integer && c.type != FieldType::Number && c.type != FieldType::Boolean;
    const std::string t = text_like ? typed : trim(typed);
    if (t.empty()) {
        if (c.nullable) return Value();
        if (text_like) return Value(std::string{});
        return std::unexpected(q + " can't be empty");
    }
    if (c.type == FieldType::Boolean) {
        std::string l;
        for (unsigned char ch : t) l += static_cast<char>(std::tolower(ch));
        if (l == "yes" || l == "true" || l == "1") return Value(std::int64_t{1});
        if (l == "no" || l == "false" || l == "0") return Value(std::int64_t{0});
        return std::unexpected(q + " is Yes or No");
    }
    if (c.type == FieldType::Integer) {
        std::size_t used = 0;
        try {
            const long long v = std::stoll(t, &used, 10);
            if (used == t.size()) return Value(static_cast<std::int64_t>(v));
        } catch (...) {}
        return std::unexpected(q + " needs a whole number");
    }
    if (c.type == FieldType::Number) {
        std::size_t used = 0;
        try {
            const double v = std::stod(t, &used);
            if (used == t.size() && std::isfinite(v)) return Value(v);
        } catch (...) {}
        return std::unexpected(q + " needs a number");
    }
    return Value(typed);
}

std::string card_title(const Table& t, const Row& row) {
    for (std::size_t i = 0; i < t.columns.size() && i < row.size(); ++i) {
        const auto& c = t.columns[i];
        if (c.pk_position > 0 || c.type != FieldType::Text) continue;
        if (const auto* s = row[i].as_text(); s && !s->empty()) return *s;
    }
    std::string k;
    for (std::size_t i = 0; i < t.columns.size() && i < row.size(); ++i)
        if (t.columns[i].pk_position > 0) {
            if (!k.empty()) k += " · ";
            k += cell_text(row[i], t.columns[i].type);
        }
    return k.empty() ? "(empty)" : "#" + k;
}

std::string page_range(const Page& p) {
    if (p.total == 0 || p.rows.empty()) return p.total == 0 ? "No records" : "Past the last record";
    return std::to_string(p.offset + 1) + "–" + std::to_string(p.offset + static_cast<std::int64_t>(p.rows.size())) +
           " of " + std::to_string(p.total);
}

}  // namespace acorn::db
