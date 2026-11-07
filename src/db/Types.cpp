// db/Types.cpp -- declared type name -> FieldType, per dialect.
#include "db/Types.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace acorn::db {

const char* to_string(FieldType t) {
    switch (t) {
    case FieldType::Text:      return "text";
    case FieldType::Integer:   return "integer";
    case FieldType::Number:    return "number";
    case FieldType::Date:      return "date";
    case FieldType::Time:      return "time";
    case FieldType::Timestamp: return "timestamp";
    case FieldType::Boolean:   return "boolean";
    case FieldType::Container: return "container";
    case FieldType::Unknown:   return "unknown";
    }
    return "?";
}

const char* to_string(Dialect d) {
    switch (d) {
    case Dialect::SQLite:     return "SQLite";
    case Dialect::MariaDB:    return "MariaDB";
    case Dialect::PostgreSQL: return "PostgreSQL";
    }
    return "?";
}

namespace {

std::string lower_trim(std::string_view in) {
    std::string s;
    for (char c : in) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

// "varchar(40) character set utf8" -> "varchar character set utf8";
// "numeric(10, 2)" -> "numeric". Collapses runs of spaces.
std::string strip_sizes(const std::string& s) {
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(') { ++depth; continue; }
        if (c == ')') { if (depth) --depth; continue; }
        if (depth) continue;
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool has(const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}
bool starts(const std::string& s, std::string_view p) {
    return s.rfind(p, 0) == 0;
}
std::string first_word(const std::string& s) {
    return s.substr(0, s.find(' '));
}

// Names that mean the same thing on every engine. nullopt-like: Unknown = "not
// one of these".
FieldType common(const std::string& s) {
    if (s == "bool" || s == "boolean") return FieldType::Boolean;
    if (s == "date") return FieldType::Date;
    if (s == "time" || starts(s, "time with") || s == "timetz") return FieldType::Time;
    if (s == "datetime" || starts(s, "timestamp")) return FieldType::Timestamp;
    return FieldType::Unknown;
}

FieldType sqlite_affinity(const std::string& s) {
    // sqlite.org/datatype3.html §3.1, in its order.
    if (has(s, "int")) return FieldType::Integer;
    if (has(s, "char") || has(s, "clob") || has(s, "text")) return FieldType::Text;
    if (has(s, "blob")) return FieldType::Container;
    if (has(s, "real") || has(s, "floa") || has(s, "doub")) return FieldType::Number;
    return FieldType::Number;   // NUMERIC affinity (DECIMAL, NUMERIC, MONEY, ...)
}

FieldType mariadb(const std::string& raw, const std::string& s) {
    // MariaDB reports BOOLEAN as tinyint(1); the size matters here only.
    if (starts(raw, "tinyint(1)")) return FieldType::Boolean;
    const std::string w = first_word(s);
    static const char* ints[] = {"tinyint", "smallint", "mediumint", "int", "integer",
                                 "bigint", "year", "bit", "serial"};
    for (auto i : ints) if (w == i) return FieldType::Integer;
    static const char* nums[] = {"decimal", "numeric", "dec", "fixed", "float", "double", "real"};
    for (auto n : nums) if (w == n) return FieldType::Number;
    if (has(w, "blob") || w == "binary" || w == "varbinary") return FieldType::Container;
    if (has(w, "char") || has(w, "text") || w == "enum" || w == "set" || w == "json" ||
        w == "uuid" || w == "inet4" || w == "inet6")
        return FieldType::Text;
    return FieldType::Unknown;
}

FieldType postgres(const std::string& s) {
    static const char* ints[] = {"smallint", "integer", "int", "bigint", "int2", "int4", "int8",
                                 "smallserial", "serial", "bigserial", "serial2", "serial4", "serial8"};
    for (auto i : ints) if (s == i) return FieldType::Integer;
    static const char* nums[] = {"numeric", "decimal", "real", "double precision",
                                 "float4", "float8", "float", "money"};
    for (auto n : nums) if (s == n) return FieldType::Number;
    if (s == "bytea") return FieldType::Container;
    if (s == "text" || starts(s, "character") || starts(s, "varchar") || s == "char" ||
        s == "bpchar" || s == "name" || s == "uuid" || s == "json" || s == "jsonb" ||
        s == "citext" || s == "xml" || s == "inet" || s == "cidr")
        return FieldType::Text;
    return FieldType::Unknown;
}

} // namespace

FieldType map_type(Dialect d, std::string_view decl_type) {
    const std::string raw = lower_trim(decl_type);
    const std::string s = strip_sizes(raw);
    if (s.empty()) return FieldType::Unknown;   // SQLite: no declared type at all
    if (auto c = common(s); c != FieldType::Unknown) return c;
    switch (d) {
    case Dialect::SQLite:     return sqlite_affinity(s);
    case Dialect::MariaDB:    return mariadb(raw, s);
    case Dialect::PostgreSQL: return postgres(s);
    }
    return FieldType::Unknown;
}

} // namespace acorn::db
