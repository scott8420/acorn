// db/Export.hpp -- writing records out as delimited text (s018a).
// GTK-free: Browse -> Export asks for a file and calls export_records; the
// selftest drives the same call on every engine.
//
// What goes out: the records Browse shows -- every one, or the found set --
// in its sort (then the key), every one of them (read a page at a time, not
// only the page on the screen). The fields: the shown layout's, in reading
// order (top to bottom, then left to right, each once), or every field of
// the table in its order. Calculations are worked out per record; summaries
// (one value over the whole found set, not a record's) and binary fields are
// left out. A header row of the field names first.
//
// How it is written (RFC 4180): UTF-8, no byte-order mark, CRLF between
// records; a value holding the separator, a quote, a line break or a space at
// either end goes in quotes, quotes doubled. Comma-separated (.csv) or
// tab-separated (.tsv -- the same quoting, so a tab in a value survives).
// Values as Browse shows them: empty for NULL, Yes / No, dates ISO.
//
// Reads only: every query is a SELECT, so it passes the Gate with the door
// shut; the file is the only thing written, to "<path>.part" first and then
// renamed over the path, so a failed export never leaves half a file where
// the person asked for one.
#pragma once

#include "core/Calc.hpp"
#include "core/Layout.hpp"
#include "db/Browse.hpp"
#include "db/Engine.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace acorn::db {

enum class ExportFormat { Csv, Tab };
// "Comma-separated (.csv)" / "Tab-separated (.tsv)"; the file ending ".csv" / ".tsv".
const char* export_format_label(ExportFormat f);
const char* export_ending(ExportFormat f);

// One value made safe for a line: quoted when it must be (above).
std::string export_cell(const std::string& v, char sep);
// One record's line (without its CRLF).
std::string export_line(const std::vector<std::string>& values, char sep);

// The fields an export of `l` writes: its field objects in reading order
// (top, then left), each once -- the table's own fields that aren't binary,
// and `calcs`' names; a missing field, a summary, a related field, a portal
// are left out.
std::vector<std::string> layout_export_fields(const core::Layout& l, const Table& t,
                                              const std::map<std::string, core::Calc>& calcs);
// Every field of the table, in order, without binary ones; then its
// calculations by name.
std::vector<std::string> table_export_fields(const Table& t, const std::map<std::string, core::Calc>& calcs);

struct ExportRequest {
    std::vector<std::string> fields;   // what to write, in order (layout_ / table_export_fields)
    std::vector<Criterion>   found;    // the found set's criteria (none: every record)
    std::string              sort;
    bool                     descending = false;
    ExportFormat             format = ExportFormat::Csv;
    std::map<std::string, core::Calc>      calcs;   // the table's calculations
    std::map<std::string, core::CalcValue> sums;    // its summaries' values (a calculation may read one)
    std::string              today;    // what Today means for a calculation (ISO)
    std::int64_t             chunk = 500;   // records per read
};

struct ExportResult {
    std::int64_t records = 0;
    std::size_t  fields = 0;
};

// Writes the header and every record to `out`. A field the table doesn't
// have (and isn't a calculation) is refused before anything is written.
std::expected<ExportResult, Error> export_records(Connection& c, const Schema& s, const Table& t,
                                                  const ExportRequest& r, std::ostream& out);
// The same into `path`: written to "<path>.part", renamed over `path` when
// whole; a failure removes the .part and leaves `path` as it was.
std::expected<ExportResult, Error> export_file(Connection& c, const Schema& s, const Table& t,
                                               const ExportRequest& r, const std::string& path);
// "Exported 12 records (5 fields) to contacts.csv"
std::string export_says(const ExportResult& r, const std::string& file);

}  // namespace acorn::db
