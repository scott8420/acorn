// db/Value.cpp -- Value, Result, Error, Schema helpers.
#include "db/Engine.hpp"
#include "db/Schema.hpp"
#include "db/Value.hpp"

#include <cstdio>

namespace acorn::db {

std::optional<std::int64_t> Value::as_int() const {
    if (auto p = std::get_if<std::int64_t>(&v_)) return *p;
    return std::nullopt;
}
std::optional<double> Value::as_real() const {
    if (auto p = std::get_if<double>(&v_)) return *p;
    if (auto p = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*p);
    return std::nullopt;
}
const std::string* Value::as_text() const { return std::get_if<std::string>(&v_); }
const Blob*        Value::as_blob() const { return std::get_if<Blob>(&v_); }

std::string Value::to_display() const {
    switch (kind()) {
    case Kind::Null:    return "NULL";
    case Kind::Integer: return std::to_string(*as_int());
    case Kind::Real: {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", std::get<double>(v_));
        return buf;
    }
    case Kind::Text:    return "'" + *as_text() + "'";
    case Kind::Blob: {
        static const char* hex = "0123456789abcdef";
        std::string s = "x'";
        for (auto b : *as_blob()) { s += hex[b >> 4]; s += hex[b & 15]; }
        return s + "'";
    }
    }
    return "?";
}

int Result::column_index(const std::string& name) const {
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (columns[i].name == name) return static_cast<int>(i);
    return -1;
}

const char* to_string(Error::Code c) {
    switch (c) {
    case Error::Code::NotFound:    return "not found";
    case Error::Code::Open:        return "open";
    case Error::Code::ReadOnly:    return "read-only";
    case Error::Code::Syntax:      return "syntax";
    case Error::Code::Constraint:  return "constraint";
    case Error::Code::Busy:        return "busy";
    case Error::Code::Abi:         return "abi";
    case Error::Code::Unsupported: return "unsupported";
    case Error::Code::Internal:    return "internal";
    case Error::Code::Exists:      return "exists";
    }
    return "?";
}

std::string quote_ident_with(char q, const std::string& name) {
    std::string s(1, q);
    for (char c : name) { s += c; if (c == q) s += q; }
    s += q;
    return s;
}

const Column* Table::column(const std::string& n) const {
    for (auto& c : columns) if (c.name == n) return &c;
    return nullptr;
}

std::vector<std::string> Table::primary_key() const {
    std::vector<const Column*> pk;
    for (auto& c : columns) if (c.pk_position > 0) pk.push_back(&c);
    std::vector<std::string> out(pk.size());
    for (auto* c : pk)
        if (static_cast<std::size_t>(c->pk_position) <= out.size())
            out[static_cast<std::size_t>(c->pk_position - 1)] = c->name;
    return out;
}

const Table* Schema::table(const std::string& n) const {
    for (auto& t : tables) if (t.name == n) return &t;
    return nullptr;
}

std::vector<LinkIn> Schema::links_to(const std::string& n) const {
    std::vector<LinkIn> out;
    for (auto& t : tables)
        for (auto& fk : t.foreign_keys)
            if (fk.ref_table == n) out.push_back({&t, &fk});
    return out;
}

std::vector<std::string> Schema::ref_columns(const ForeignKey& fk) const {
    if (!fk.ref_columns.empty()) return fk.ref_columns;
    if (auto* t = table(fk.ref_table)) return t->primary_key();
    return {};
}

} // namespace acorn::db
