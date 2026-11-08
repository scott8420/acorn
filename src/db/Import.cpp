#include "db/Import.hpp"
#include "db/Browse.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>

// db/Import.cpp -- records in from CSV / tab-separated text (s018b).

namespace acorn::db {

namespace {
std::string curly(const std::string& s) { return "“" + s + "”"; }

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
bool ends_with(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && lower(s.substr(s.size() - e.size())) == e;
}

// Digits at [at, at + n) -> their value, else -1.
int digits(std::string_view s, std::size_t at, std::size_t n) {
    if (at + n > s.size()) return -1;
    int v = 0;
    for (std::size_t i = at; i < at + n; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}
// YYYY-MM-DD, a real day of a real month (Feb 29 only in a leap year).
bool is_date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    const int y = digits(s, 0, 4), m = digits(s, 5, 2), d = digits(s, 8, 2);
    if (y < 0 || m < 1 || m > 12 || d < 1) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= days[m - 1] + (m == 2 && leap ? 1 : 0);
}
// HH:MM[:SS[.f...]] -- how much of `s` it used (0: not a time).
std::size_t time_len(std::string_view s) {
    const int h = digits(s, 0, 2), mi = digits(s, 3, 2);
    if (s.size() < 5 || s[2] != ':' || h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
    std::size_t at = 5;
    if (at < s.size() && s[at] == ':') {
        const int sec = digits(s, at + 1, 2);
        if (sec < 0 || sec > 60) return 0;
        at += 3;
        if (at < s.size() && s[at] == '.') {
            std::size_t k = at + 1;
            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
            if (k == at + 1) return 0;
            at = k;
        }
    }
    return at;
}
// A zone after a time: Z, +hh, +hhmm, +hh:mm (or -).
bool is_zone(std::string_view z) {
    if (z.empty()) return true;
    if (z == "Z") return true;
    if (z[0] != '+' && z[0] != '-') return false;
    z.remove_prefix(1);
    if (digits(z, 0, 2) < 0) return false;
    if (z.size() == 2) return true;
    if (z.size() == 4) return digits(z, 2, 2) >= 0;
    return z.size() == 5 && z[2] == ':' && digits(z, 3, 2) >= 0;
}
bool is_time(std::string_view s) {
    const std::size_t n = time_len(s);
    return n > 0 && is_zone(s.substr(n));
}
bool is_timestamp(std::string_view s) {
    if (s.size() == 10) return is_date(s);
    if (s.size() < 16 || !is_date(s.substr(0, 10)) || (s[10] != ' ' && s[10] != 'T')) return false;
    return is_time(s.substr(11));
}
}  // namespace

bool is_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t n;
        unsigned least;
        if ((c & 0xE0) == 0xC0) { n = 1; least = 0x80; }
        else if ((c & 0xF0) == 0xE0) { n = 2; least = 0x800; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) { n = 3; least = 0x10000; }
        else return false;
        if (i + n >= s.size()) return false;
        unsigned cp = c & (0x3Fu >> n);
        for (std::size_t k = 1; k <= n; ++k) {
            const unsigned b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;   // overlong, too big, a surrogate
        i += n + 1;
    }
    return true;
}

std::string to_utf8(std::string_view bytes, bool& converted) {
    converted = false;
    if (is_utf8(bytes)) return std::string(bytes);
    converted = true;
    // Windows-1252's 0x80..0x9F; the rest of the high half is Latin-1 (= U+00A0..U+00FF).
    static const char16_t cp[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                    0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 8);
    for (unsigned char c : bytes) {
        const unsigned u = c < 0x80 ? c : c < 0xA0 ? cp[c - 0x80] : c;
        if (u < 0x80) out += static_cast<char>(u);
        else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 0x3F)); }
        else { out += static_cast<char>(0xE0 | (u >> 12)); out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
               out += static_cast<char>(0x80 | (u & 0x3F)); }
    }
    return out;
}

std::optional<std::string> iso_date(std::string_view s, DateOrder o) {
    if (is_date(s)) return std::string(s);
    if (o == DateOrder::ISO) return std::nullopt;
    // a<sep>b<sep>yyyy -- one or two digits each, the same separator twice
    std::size_t i = 0;
    auto num = [&](std::size_t most) -> int {
        std::size_t k = i;
        int v = 0;
        while (k < s.size() && k - i < most && std::isdigit(static_cast<unsigned char>(s[k]))) v = v * 10 + (s[k++] - '0');
        if (k == i) return -1;
        i = k;
        return v;
    };
    const int a = num(2);
    if (a < 0 || i >= s.size()) return std::nullopt;
    const char sep = s[i];
    if (sep != '/' && sep != '-' && sep != '.') return std::nullopt;
    ++i;
    const int b = num(2);
    if (b < 0 || i >= s.size() || s[i] != sep) return std::nullopt;
    ++i;
    const std::size_t ys = i;
    const int y = num(4);
    if (y < 0 || i != s.size() || i - ys != 4) return std::nullopt;
    const int d = o == DateOrder::DayFirst ? a : b, m = o == DateOrder::DayFirst ? b : a;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    if (!is_date(buf)) return std::nullopt;
    return std::string(buf);
}

const char* date_order_says(DateOrder o) {
    return o == DateOrder::DayFirst ? "14/07/1990 (day first)" : o == DateOrder::MonthFirst ? "07/14/1990 (month first)" : "1990-07-14";
}

std::size_t Delimited::width() const {
    std::size_t w = 0;
    for (const auto& r : rows) w = std::max(w, r.size());
    return w;
}

char guess_separator(std::string_view text, const std::string& file_name) {
    if (ends_with(file_name, ".tsv") || ends_with(file_name, ".tab")) return '\t';
    if (ends_with(file_name, ".csv")) return ',';
    std::size_t tabs = 0, commas = 0;
    bool q = false;
    for (char ch : text) {
        if (ch == '"') q = !q;
        else if (!q && (ch == '\n' || ch == '\r')) break;
        else if (!q && ch == '\t') ++tabs;
        else if (!q && ch == ',') ++commas;
    }
    return tabs > commas ? '\t' : ',';
}

std::expected<Delimited, std::string> read_delimited(std::string_view text, char sep) {
    Delimited d;
    d.sep = sep;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::vector<std::string> row;
    std::string cell;
    std::size_t line = 1, row_line = 1, quote_line = 0;
    bool quoted = false;      // inside quotes
    bool any = false;         // this record has begun (a cell or a separator seen)
    auto end_cell = [&] { row.push_back(std::move(cell)); cell.clear(); };
    auto end_row = [&] {
        end_cell();
        d.rows.push_back(std::move(row));
        d.lines.push_back(row_line);
        row.clear();
        any = false;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (!any) { row_line = line; }
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else quoted = false;
            } else {
                if (ch == '\n' || (ch == '\r' && !(i + 1 < text.size() && text[i + 1] == '\n'))) ++line;
                cell += ch;
            }
            continue;
        }
        if (ch == '"' && cell.empty()) { quoted = true; any = true; quote_line = line; continue; }
        if (ch == sep) { end_cell(); any = true; continue; }
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !cell.empty()) end_row();
            else { d.rows.emplace_back(); d.lines.push_back(line); }   // an empty line: a record of nothing
            ++line;
            continue;
        }
        cell += ch;
        any = true;
    }
    if (quoted) return std::unexpected("line " + std::to_string(quote_line) + ": a quote is never closed");
    if (any || !cell.empty()) end_row();
    return d;
}

std::vector<std::string> import_fields(const Table& t) {
    std::vector<std::string> out;
    for (const auto& c : t.columns) if (c.type != FieldType::Container) out.push_back(c.name);
    return out;
}

std::vector<std::string> match_fields(const std::vector<std::string>& first, const Table& t) {
    std::vector<std::string> out(first.size());
    std::vector<std::string> used;
    for (std::size_t i = 0; i < first.size(); ++i) {
        const std::string want = lower(trim(first[i]));
        for (const auto& c : t.columns) {
            if (c.type == FieldType::Container || c.auto_increment) continue;
            if (lower(c.name) != want || std::find(used.begin(), used.end(), c.name) != used.end()) continue;
            out[i] = c.name;
            used.push_back(c.name);
            break;
        }
    }
    return out;
}

bool looks_like_header(const std::vector<std::string>& first, const Table& t) {
    for (const auto& cell : first) {
        const std::string want = lower(trim(cell));
        for (const auto& c : t.columns) if (!want.empty() && lower(c.name) == want) return true;
    }
    return false;
}

std::expected<Value, std::string> import_value(const std::string& cell, const Column& c, DateOrder o) {
    const bool when = c.type == FieldType::Date || c.type == FieldType::Time || c.type == FieldType::Timestamp;
    if (!when) return parse_field(cell, c);
    std::string t = trim(cell);
    if (t.empty()) return parse_field(t, c);
    const std::string q = curly(c.name);
    // s024b: the date written the file's way -> ISO.
    if (c.type == FieldType::Date) {
        auto iso = iso_date(t, o);
        if (!iso) return std::unexpected(q + " needs a date like " + date_order_says(o));
        return Value(*iso);
    }
    if (c.type == FieldType::Timestamp && o != DateOrder::ISO) {
        const auto sp = t.find_first_of(" T");
        auto iso = iso_date(std::string_view(t).substr(0, sp), o);
        if (!iso) return std::unexpected(q + " needs a date and time like " + date_order_says(o) + " 09:30:00");
        t = *iso + (sp == std::string::npos ? std::string{} : " " + trim(t.substr(sp + 1)));
    }
    if (c.type == FieldType::Time && !is_time(t)) return std::unexpected(q + " needs a time like 09:30 or 09:30:15");
    if (c.type == FieldType::Timestamp && !is_timestamp(t))
        return std::unexpected(q + " needs a date and time like 1990-07-14 09:30:00");
    return Value(t);
}

std::string check_import(const Table& t, const ImportPlan& p) {
    std::vector<std::string> seen;
    for (const auto& f : p.fields) {
        if (f.empty()) continue;
        const Column* c = t.column(f);
        if (!c) return curly(t.name) + " has no field " + curly(f);
        if (c->type == FieldType::Container) return curly(f) + " is binary -- it can't come from a text file";
        if (std::find(seen.begin(), seen.end(), f) != seen.end()) return curly(f) + " is picked for two columns";
        seen.push_back(f);
    }
    if (seen.empty()) return "Pick a field for at least one column";
    if (!p.match.empty() && std::find(seen.begin(), seen.end(), p.match) == seen.end())   // s024a
        return "Matching by " + curly(p.match) + " needs a column going to it";
    return {};
}

namespace {
bool blank(const std::vector<std::string>& r) {
    return std::all_of(r.begin(), r.end(), [](const std::string& s) { return s.empty(); });
}
// One row -> the columns and values to insert, or why not.
std::expected<std::pair<std::vector<std::string>, std::vector<Value>>, std::string>
row_values(const Table& t, const std::vector<std::string>& row, const ImportPlan& p) {
    for (std::size_t i = p.fields.size(); i < row.size(); ++i)
        if (!row[i].empty())
            return std::unexpected("it has " + std::to_string(row.size()) + " values -- the file's columns are " +
                                   std::to_string(p.fields.size()));
    std::pair<std::vector<std::string>, std::vector<Value>> out;
    for (std::size_t i = 0; i < p.fields.size(); ++i) {
        if (p.fields[i].empty()) continue;
        const Column* c = t.column(p.fields[i]);
        if (!c) return std::unexpected(curly(t.name) + " has no field " + curly(p.fields[i]));
        const std::string cell = i < row.size() ? row[i] : std::string{};
        if (trim(cell).empty() && c->default_sql) continue;   // the database's default
        auto v = import_value(cell, *c, p.dates);
        if (!v) return std::unexpected(v.error());
        out.first.push_back(c->name);
        out.second.push_back(std::move(*v));
    }
    return out;
}
}  // namespace

DateOrder guess_date_order(const Table& t, const Delimited& d, const ImportPlan& p) {
    bool slashed = false;
    for (std::size_t col = 0; col < p.fields.size(); ++col) {
        const Column* c = p.fields[col].empty() ? nullptr : t.column(p.fields[col]);
        if (!c || (c->type != FieldType::Date && c->type != FieldType::Timestamp)) continue;
        for (std::size_t i = p.header ? 1 : 0; i < d.rows.size(); ++i) {
            if (col >= d.rows[i].size()) continue;
            const std::string cell = trim(d.rows[i][col]);
            const std::string day = cell.substr(0, cell.find_first_of(" T"));
            if (day.empty() || is_date(day)) continue;
            if (!iso_date(day, DateOrder::DayFirst) && !iso_date(day, DateOrder::MonthFirst)) continue;
            slashed = true;
            if (!iso_date(day, DateOrder::MonthFirst)) return DateOrder::DayFirst;    // 14/07: no 14th month
            if (!iso_date(day, DateOrder::DayFirst)) return DateOrder::MonthFirst;   // 07/14
        }
    }
    return slashed ? DateOrder::MonthFirst : DateOrder::ISO;
}

ImportResult import_preview(const Table& t, const Delimited& d, const ImportPlan& p) {
    ImportResult r;
    for (std::size_t i = p.header ? 1 : 0; i < d.rows.size(); ++i) {
        if (blank(d.rows[i])) continue;
        ++r.rows;
        auto v = row_values(t, d.rows[i], p);
        if (v) ++r.records;
        else r.refused.push_back({d.lines[i], v.error()});
    }
    return r;
}

std::expected<ImportResult, Error> import_records(Journal& j, const Table& t, const Delimited& d, const ImportPlan& p,
                                                  const std::function<bool(std::int64_t)>& keep_going) {
    if (const std::string why = check_import(t, p); !why.empty()) return std::unexpected(Error{Error::Code::Unsupported, why});
    if (const std::string why = Store::why_not_writable(t); !why.empty())
        return std::unexpected(Error{Error::Code::Unsupported, why});
    if (j.in_batch()) return std::unexpected(Error{Error::Code::Internal, "another batch is running"});
    ImportResult r;
    r.matching = !p.match.empty();
    std::size_t mcol = 0;   // s024a: the file's column the match field comes from
    for (std::size_t i = 0; i < p.fields.size(); ++i) if (r.matching && p.fields[i] == p.match) mcol = i;
    j.begin_batch(import_label(0, t.name));
    for (std::size_t i = p.header ? 1 : 0; i < d.rows.size(); ++i) {
        if (blank(d.rows[i])) continue;
        if (keep_going && !keep_going(r.rows)) { r.stopped_at = d.lines[i]; break; }
        ++r.rows;
        auto v = row_values(t, d.rows[i], p);
        if (!v) { r.refused.push_back({d.lines[i], v.error()}); continue; }
        if (r.matching) {   // s024a: the records whose match field holds this row's value
            const std::string cell = trim(mcol < d.rows[i].size() ? d.rows[i][mcol] : std::string{});
            std::vector<Store::Id> ids;
            if (!cell.empty()) {
                auto found = read_found(j.store().connection(), t, {Criterion{p.match, "=" + cell}}, PageRequest{0, 100000});
                if (!found) { r.refused.push_back({d.lines[i], found.error().message}); continue; }
                ids = found->ids;
            }
            if (!ids.empty()) {
                std::vector<std::string> cols;
                std::vector<Value> vals;
                for (std::size_t k = 0; k < v->first.size(); ++k)
                    if (v->first[k] != p.match) { cols.push_back(v->first[k]); vals.push_back(v->second[k]); }
                bool ok = true;
                for (const auto& id : ids) {
                    if (cols.empty()) { ++r.updated; continue; }
                    auto u = verbs::set_fields(j, id, cols, vals);
                    if (!u) { r.refused.push_back({d.lines[i], u.error().message}); ok = false; break; }
                    ++r.updated;
                }
                (void)ok;
                continue;
            }
            if (!p.add_rest) { ++r.unmatched; continue; }
        }
        auto made = verbs::new_record(j, t.name, v->first, v->second);
        if (!made) { r.refused.push_back({d.lines[i], made.error().message}); continue; }
        ++r.records;
    }
    j.name_batch(r.matching ? import_label(r, t.name) : import_label(r.records, t.name));
    (void)j.end_batch();
    return r;
}

std::string import_label(std::int64_t n, const std::string& table) {
    return "Import " + std::to_string(n) + (n == 1 ? " record" : " records") + " into " + curly(table);
}

std::string import_label(const ImportResult& r, const std::string& table) {
    return "Import into " + curly(table) + " -- " + std::to_string(r.updated) + " updated, " + std::to_string(r.records) + " added";
}

std::string import_says(const ImportResult& r, const std::string& table) {
    std::string s = r.matching
        ? "Updated " + std::to_string(r.updated) + (r.updated == 1 ? " record" : " records") + " and added " +
              std::to_string(r.records) + " in " + curly(table)
        : "Imported " + std::to_string(r.records) + (r.records == 1 ? " record" : " records") + " into " + curly(table);
    if (r.unmatched) s += " -- " + std::to_string(r.unmatched) + (r.unmatched == 1 ? " row matched" : " rows matched") + " none (not added)";
    if (!r.refused.empty()) s += " -- " + std::to_string(r.refused.size()) + " refused";
    if (r.stopped_at) s += " -- stopped before line " + std::to_string(r.stopped_at) + ", the rest weren't read";
    return s;
}

std::string refusal_says(const ImportRefusal& r) { return "Line " + std::to_string(r.line) + ": " + r.why; }

}  // namespace acorn::db
