#include "db/Browse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

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

std::expected<Page, Error> read_where(Connection& c, const Table& t, const PageRequest& r,
                                      const std::vector<std::string>& link, const std::vector<Value>& values) {
    Page p;
    p.offset = std::max<std::int64_t>(r.offset, 0);
    for (const auto& col : t.columns) p.columns.push_back(col.name);
    if (t.columns.empty()) return p;
    const std::string where = where_of(c, link);

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

}  // namespace

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
