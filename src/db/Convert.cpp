#include "db/Convert.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>

// db/Convert.cpp -- see the header. One set of rules for every engine.

namespace acorn::db {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::optional<std::int64_t> read_int(const std::string& typed) {
    const std::string t = trim(typed);
    if (t.empty()) return std::nullopt;
    std::int64_t v = 0;
    const char* b = t.data() + (t[0] == '+' ? 1 : 0);
    auto [p, ec] = std::from_chars(b, t.data() + t.size(), v);
    if (ec != std::errc{} || p != t.data() + t.size()) return std::nullopt;
    return v;
}

std::optional<double> read_real(const std::string& typed) {
    const std::string t = trim(typed);
    if (t.empty()) return std::nullopt;
    double v = 0;
    const char* b = t.data() + (t[0] == '+' ? 1 : 0);
    auto [p, ec] = std::from_chars(b, t.data() + t.size(), v);
    if (ec != std::errc{} || p != t.data() + t.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::string real_text(double d) {   // shortest form that reads back the same
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof buf, d);
    return ec == std::errc{} ? std::string(buf, p) : std::to_string(d);
}

// A whole double -> the int64 it is (nullopt: not whole, or out of range).
std::optional<std::int64_t> whole(double d) {
    if (!std::isfinite(d) || std::floor(d) != d) return std::nullopt;
    if (d < -9.2e18 || d > 9.2e18) return std::nullopt;
    return static_cast<std::int64_t>(d);
}

bool digits(const std::string& s, std::size_t at, std::size_t n) {
    if (at + n > s.size()) return false;
    for (std::size_t i = at; i < at + n; ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

// YYYY-MM-DD
bool is_date(const std::string& s) {
    if (s.size() != 10 || !digits(s, 0, 4) || s[4] != '-' || !digits(s, 5, 2) || s[7] != '-' || !digits(s, 8, 2))
        return false;
    const int m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// HH:MM[:SS[.fff...]]
bool is_time(const std::string& s) {
    if (s.size() < 5 || !digits(s, 0, 2) || s[2] != ':' || !digits(s, 3, 2)) return false;
    if (std::stoi(s.substr(0, 2)) > 23 || std::stoi(s.substr(3, 2)) > 59) return false;
    if (s.size() == 5) return true;
    if (s.size() < 8 || s[5] != ':' || !digits(s, 6, 2) || std::stoi(s.substr(6, 2)) > 59) return false;
    if (s.size() == 8) return true;
    return s[8] == '.' && s.size() > 9 && digits(s, 9, s.size() - 9);
}

bool is_timestamp(const std::string& s) {
    return s.size() >= 16 && is_date(s.substr(0, 10)) && (s[10] == ' ' || s[10] == 'T') && is_time(s.substr(11));
}

}  // namespace

std::optional<Value> convert_value(const Value& v, FieldType from, FieldType to) {
    if (v.is_null() || to == FieldType::Unknown) return v;
    const auto* text = v.as_text();
    const auto ival = v.kind() == Value::Kind::Integer ? v.as_int() : std::nullopt;
    const auto rval = v.kind() == Value::Kind::Real ? v.as_real() : std::nullopt;
    switch (to) {
    case FieldType::Text:
        if (text) return v;
        if (ival) {
            if (from == FieldType::Boolean && (*ival == 0 || *ival == 1)) return Value(std::string(*ival ? "Yes" : "No"));
            return Value(std::to_string(*ival));
        }
        if (rval) return Value(real_text(*rval));
        return std::nullopt;   // a blob
    case FieldType::Integer:
        if (ival) return v;
        if (rval) { if (auto w = whole(*rval)) return Value(*w); return std::nullopt; }
        if (text) { if (auto i = read_int(*text)) return Value(*i); return std::nullopt; }
        return std::nullopt;
    case FieldType::Number:
        if (rval) return v;
        if (ival) return Value(static_cast<double>(*ival));
        if (text) { if (auto r = read_real(*text)) return Value(*r); return std::nullopt; }
        return std::nullopt;
    case FieldType::Boolean:
        if (ival) { if (*ival == 0 || *ival == 1) return v; return std::nullopt; }
        if (rval) { if (*rval == 0.0 || *rval == 1.0) return Value(static_cast<std::int64_t>(*rval)); return std::nullopt; }
        if (text) {
            const std::string l = lower(trim(*text));
            if (l == "yes" || l == "true" || l == "1") return Value(std::int64_t{1});
            if (l == "no" || l == "false" || l == "0") return Value(std::int64_t{0});
        }
        return std::nullopt;
    case FieldType::Date:
        if (text && is_date(*text)) return v;
        return std::nullopt;
    case FieldType::Time:
        if (text && is_time(*text)) return v;
        return std::nullopt;
    case FieldType::Timestamp:
        if (text && is_timestamp(*text)) return v;
        if (text && is_date(*text)) return Value(*text + " 00:00:00");
        return std::nullopt;
    case FieldType::Container:
        if (v.kind() == Value::Kind::Blob) return v;
        return std::nullopt;
    case FieldType::Unknown:
        return v;
    }
    return std::nullopt;
}

bool native_to(const Value& v, FieldType t) {
    switch (v.kind()) {
    case Value::Kind::Null:    return true;
    case Value::Kind::Integer: return t == FieldType::Integer || t == FieldType::Boolean || t == FieldType::Number ||
                                      t == FieldType::Unknown;
    case Value::Kind::Real:    return t == FieldType::Number || t == FieldType::Unknown;
    case Value::Kind::Text:    return t == FieldType::Text || t == FieldType::Date || t == FieldType::Time ||
                                      t == FieldType::Timestamp || t == FieldType::Unknown;
    case Value::Kind::Blob:    return t == FieldType::Container || t == FieldType::Unknown;
    }
    return false;
}

std::string kind_words(FieldType t) {
    switch (t) {
    case FieldType::Text:      return "text";
    case FieldType::Integer:   return "a whole number";
    case FieldType::Number:    return "a number";
    case FieldType::Date:      return "a date (YYYY-MM-DD)";
    case FieldType::Time:      return "a time (HH:MM)";
    case FieldType::Timestamp: return "a date and time";
    case FieldType::Boolean:   return "Yes or No";
    case FieldType::Container: return "binary data";
    case FieldType::Unknown:   return "this field";
    }
    return "this field";
}

std::string default_text(const std::optional<std::string>& sql) {
    if (!sql) return {};
    std::string s = trim(*sql);
    // PostgreSQL adds a cast: 'x'::text, 'x'::character varying, (0)::bigint.
    if (auto at = s.rfind("::"); at != std::string::npos && at > 0 && (s[at - 1] == '\'' || s[at - 1] == ')')) {
        bool cast = true;   // only a trailing type name after it
        for (std::size_t i = at + 2; i < s.size(); ++i) {
            const char ch = s[i];
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '_' || ch == '(' || ch == ')'))
                cast = false;
        }
        if (cast) s = s.substr(0, at);
    }
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')') s = s.substr(1, s.size() - 2);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        std::string out;
        for (std::size_t i = 1; i + 1 < s.size(); ++i) {
            out += s[i];
            if (s[i] == '\'' && i + 2 < s.size() && s[i + 1] == '\'') ++i;   // '' -> '
        }
        return out;
    }
    return s;
}

std::expected<std::optional<std::string>, std::string>
default_sql_for(Dialect d, FieldType t, const std::string& field, const std::string& typed) {
    const std::string q = "“" + field + "”";
    if (typed.empty()) return std::optional<std::string>{};
    auto quoted = [&](const std::string& s) {
        std::string out = "'";
        for (char ch : s) { out += ch; if (ch == '\'') out += '\''; }
        return out + "'";
    };
    auto v = convert_value(Value(typed), FieldType::Text, t);
    switch (t) {
    case FieldType::Integer:
        if (!v) return std::unexpected("the default for " + q + " needs a whole number");
        return std::optional<std::string>(std::to_string(*v->as_int()));
    case FieldType::Number:
        if (!v) return std::unexpected("the default for " + q + " needs a number");
        return std::optional<std::string>(real_text(*v->as_real()));
    case FieldType::Boolean:
        if (!v) return std::unexpected("the default for " + q + " is Yes or No");
        if (d == Dialect::PostgreSQL) return std::optional<std::string>(*v->as_int() ? "TRUE" : "FALSE");
        return std::optional<std::string>(*v->as_int() ? "1" : "0");
    case FieldType::Date:
    case FieldType::Time:
    case FieldType::Timestamp:
        if (!v) return std::unexpected("the default for " + q + " needs " + kind_words(t));
        return std::optional<std::string>(quoted(*v->as_text()));
    case FieldType::Container:
        return std::unexpected(q + " holds binary data -- it takes no default here");
    default:
        return std::optional<std::string>(quoted(typed));
    }
}

}  // namespace acorn::db
