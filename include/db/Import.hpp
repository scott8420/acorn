// db/Import.hpp -- records in from delimited text (s018b). The other half of
// db/Export: what s018a writes, this reads back exactly.
// GTK-free: Browse -> Import… reads the file and shows a preview (ImportWindow);
// Import calls import_records; the selftest drives the same calls on every
// engine.
//
// Reading (RFC 4180, leniently): UTF-8, a byte-order mark skipped; CRLF, LF or
// a lone CR end a record; a field in quotes may hold the separator, a doubled
// quote (one quote) and line breaks. A quote inside an unquoted field is kept
// as it is. A quote never closed is the one thing refused -- "line 12: a
// quote is never closed" -- since everything after it would be one value.
// Comma or tab: a .tsv / .tab file is tab-separated, a .csv comma; otherwise
// the first line decides (more tabs than commas outside quotes -> tabs).
//
// Matching: each column of the file goes to a field or is skipped. The first
// line's names are matched to the table's fields case-blind, each field once;
// a counting key (auto increment) is left to count up, a binary field is
// never offered. The person can change any match.
//
// Writing: every record through the door (verbs::new_record) inside ONE
// journal batch -- "Import 40 records into “contacts”" on Ctrl+Z. A row is
// never half written: each row is one write of its own, so a row a field or
// the database refuses ("line 7: “birthday” needs a date") is listed with its
// line number and the rest go in. An empty cell in a field with a default
// leaves the default to the database. A line with nothing on it is passed by.
#pragma once

#include "db/Engine.hpp"
#include "db/Journal.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace acorn::db {

// The file, read: its records (each a list of cells) and the line each starts on.
struct Delimited {
    char                                  sep = ',';
    std::vector<std::vector<std::string>> rows;
    std::vector<std::size_t>              lines;   // 1-based, per row
    std::size_t width() const;                     // the most cells in any row
};

// ',' or '\t': by the file's ending (.tsv / .tab, .csv), else by its first line.
char guess_separator(std::string_view text, const std::string& file_name);
// The whole text read; refused in words only for a quote never closed.
std::expected<Delimited, std::string> read_delimited(std::string_view text, char sep);

// The fields a column may go to: every field that isn't binary, in order.
std::vector<std::string> import_fields(const Table& t);
// Per column of `first`: the field its name matches (case-blind, each field
// once; a counting key and a binary field never), else "" (skipped).
std::vector<std::string> match_fields(const std::vector<std::string>& first, const Table& t);
// Does the first line name fields? (any cell matches one, case-blind)
bool looks_like_header(const std::vector<std::string>& first, const Table& t);

// One cell for one field: parse_field's rules (s006c), and a date, a time or a
// timestamp must look like one -- ISO, as Export writes it ("“birthday” needs a
// date like 1990-07-14"), whatever the engine would let by.
std::expected<Value, std::string> import_value(const std::string& cell, const Column& c);

// What the person chose: a field (or "" to skip) per column of the file, and
// whether the first line is names (not a record).
struct ImportPlan {
    std::vector<std::string> fields;
    bool                     header = true;
};
// "" when the plan can go; else why not, in words ("“email” is picked for two
// columns", "Pick a field for at least one column").
std::string check_import(const Table& t, const ImportPlan& p);

struct ImportRefusal {
    std::size_t line = 0;
    std::string why;
};
struct ImportResult {
    std::int64_t               records = 0;   // made
    std::int64_t               rows = 0;      // the file's records looked at (not counting the names, nor empty lines)
    std::vector<ImportRefusal> refused;
};

// The rows that would go in, and the ones a field refuses (the database isn't
// asked -- a unique field or a link can still refuse a row on Import).
ImportResult import_preview(const Table& t, const Delimited& d, const ImportPlan& p);

// Every row through the door in one batch, labelled by import_label. A plan
// check_import refuses, or a table that isn't writable by key, is refused
// before anything is written. Nothing made: no step.
std::expected<ImportResult, Error> import_records(Journal& j, const Table& t, const Delimited& d, const ImportPlan& p);

// "Import 40 records into “contacts”"
std::string import_label(std::int64_t n, const std::string& table);
// "Imported 38 records into “contacts” -- 2 refused" (no refusals: no tail)
std::string import_says(const ImportResult& r, const std::string& table);
// "Line 7: “birthday” needs a date like 1990-07-14"
std::string refusal_says(const ImportRefusal& r);

}  // namespace acorn::db
