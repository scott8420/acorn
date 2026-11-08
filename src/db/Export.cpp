#include "db/Export.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

// db/Export.cpp -- records out as CSV / tab-separated text (s018a).

namespace acorn::db {

const char* export_format_label(ExportFormat f) {
    return f == ExportFormat::Tab ? "Tab-separated (.tsv)" : "Comma-separated (.csv)";
}
const char* export_ending(ExportFormat f) { return f == ExportFormat::Tab ? ".tsv" : ".csv"; }

std::string export_cell(const std::string& v, char sep) {
    bool quote = !v.empty() && (v.front() == ' ' || v.back() == ' ');
    for (char ch : v) if (ch == sep || ch == '"' || ch == '\n' || ch == '\r') { quote = true; break; }
    if (!quote) return v;
    std::string out = "\"";
    for (char ch : v) { if (ch == '"') out += '"'; out += ch; }
    return out + "\"";
}

std::string export_line(const std::vector<std::string>& values, char sep) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += sep;
        out += export_cell(values[i], sep);
    }
    return out;
}

namespace {
bool usable(const Table& t, const std::map<std::string, core::Calc>& calcs, const std::string& f) {
    if (const Column* c = t.column(f)) return c->type != FieldType::Container;
    return calcs.contains(f);
}
}  // namespace

std::vector<std::string> layout_export_fields(const core::Layout& l, const Table& t,
                                              const std::map<std::string, core::Calc>& calcs) {
    std::vector<const core::LayoutObject*> fs;
    for (const auto& o : l.objects)
        if (o.kind == core::ObjectKind::Field && usable(t, calcs, o.field)) fs.push_back(&o);
    std::stable_sort(fs.begin(), fs.end(), [](const auto* a, const auto* b) { return a->y != b->y ? a->y < b->y : a->x < b->x; });
    std::vector<std::string> out;
    for (const auto* o : fs)
        if (std::find(out.begin(), out.end(), o->field) == out.end()) out.push_back(o->field);
    return out;
}

std::vector<std::string> table_export_fields(const Table& t, const std::map<std::string, core::Calc>& calcs) {
    std::vector<std::string> out;
    for (const auto& c : t.columns) if (c.type != FieldType::Container) out.push_back(c.name);
    for (const auto& [n, c] : calcs) if (!t.column(n)) out.push_back(n);
    return out;
}

std::expected<ExportResult, Error> export_records(Connection& c, const Schema& s, const Table& t,
                                                  const ExportRequest& r, std::ostream& out) {
    const char sep = r.format == ExportFormat::Tab ? '\t' : ',';
    if (r.fields.empty()) return std::unexpected(Error{Error::Code::Unsupported, "there are no fields to export"});
    // Where each field comes from: a column (its index) or a calculation (-1).
    std::vector<int> at;
    bool any_calc = false;
    for (const auto& f : r.fields) {
        int k = -1;
        for (std::size_t i = 0; i < t.columns.size(); ++i) if (t.columns[i].name == f) k = static_cast<int>(i);
        if (k >= 0 && t.columns[static_cast<std::size_t>(k)].type == FieldType::Container)
            return std::unexpected(Error{Error::Code::Unsupported, "“" + f + "” is binary -- it can't go in a text file"});
        if (k < 0 && !r.calcs.contains(f))
            return std::unexpected(Error{Error::Code::NotFound, "“" + t.name + "” has no field “" + f + "”"});
        any_calc = any_calc || k < 0;
        at.push_back(k);
    }
    out << export_line(r.fields, sep) << "\r\n";
    ExportResult res;
    res.fields = r.fields.size();
    PageRequest pr{0, std::max<std::int64_t>(1, r.chunk), r.sort, r.descending};
    for (;;) {
        auto page = r.found.empty() ? read_page(c, t, pr) : read_found(c, t, r.found, pr, &s);
        if (!page) return std::unexpected(page.error());
        for (const auto& row : page->rows) {
            std::map<std::string, core::CalcOutcome> worked;
            if (any_calc) worked = row_calcs(t, row, r.calcs, r.today, &r.sums);
            std::vector<std::string> vals;
            vals.reserve(at.size());
            for (std::size_t i = 0; i < at.size(); ++i) {
                if (at[i] >= 0) {
                    const auto k = static_cast<std::size_t>(at[i]);
                    vals.push_back(k < row.size() ? cell_text(row[k], t.columns[k].type) : std::string{});
                } else {
                    const auto& oc = worked[r.fields[i]];
                    vals.push_back(oc.ok() ? core::calc_text(oc) : std::string{});
                }
            }
            out << export_line(vals, sep) << "\r\n";
            ++res.records;
        }
        if (!out) return std::unexpected(Error{Error::Code::Internal, "the file couldn't be written"});
        if (r.keep_going && !r.keep_going(res.records))   // s020b: Stop
            return std::unexpected(Error{Error::Code::Internal, "stopped"});
        if (page->rows.empty() || static_cast<std::int64_t>(page->rows.size()) < pr.limit) break;
        pr.offset += pr.limit;
    }
    return res;
}

std::expected<ExportResult, Error> export_file(Connection& c, const Schema& s, const Table& t,
                                               const ExportRequest& r, const std::string& path) {
    namespace fs = std::filesystem;
    const std::string part = path + ".part";
    std::expected<ExportResult, Error> res = std::unexpected(Error{Error::Code::Internal, "not written"});
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(Error{Error::Code::Internal, "can't write " + fs::path(path).filename().string() + " there"});
        res = export_records(c, s, t, r, out);
        out.flush();
        if (res && !out) res = std::unexpected(Error{Error::Code::Internal, "the file couldn't be written"});
    }
    std::error_code ec;
    if (!res) { fs::remove(part, ec); return res; }
    fs::rename(part, path, ec);
    if (ec) {
        fs::remove(part, ec);
        return std::unexpected(Error{Error::Code::Internal, "couldn't put the file in place: " + ec.message()});
    }
    return res;
}

std::string export_says(const ExportResult& r, const std::string& file) {
    return "Exported " + std::to_string(r.records) + (r.records == 1 ? " record" : " records") + " (" +
           std::to_string(r.fields) + (r.fields == 1 ? " field" : " fields") + ") to " + file;
}

}  // namespace acorn::db
