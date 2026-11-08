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
#include <functional>
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
    // s024a -- FileMaker's "update matching records": `match` (one of the
    // picked fields; "" = add every row) finds the records whose field holds
    // exactly the row's value; each is UPDATED with the row's other fields.
    // A row matching none is added -- unless `add_rest` is false (then it's
    // counted, not added). An empty match value matches nothing.
    std::string              match;
    bool                     add_rest = true;
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
    std::size_t                stopped_at = 0;   // s020c: Stop -- the line it stopped before (0: it ran to the end)
    bool                       matching = false; // s024a: the plan matched by a field
    std::int64_t               updated = 0;      // s024a: records changed by a matching row
    std::int64_t               unmatched = 0;    // s024a: rows matching none, left out (add_rest off)
};

// The rows that would go in, and the ones a field refuses (the database isn't
// asked -- a unique field or a link can still refuse a row on Import).
ImportResult import_preview(const Table& t, const Delimited& d, const ImportPlan& p);

// Every row through the door in one batch, labelled by import_label. A plan
// check_import refuses, or a table that isn't writable by key, is refused
// before anything is written. Nothing made: no step.
//
// s020c: `keep_going(rows looked at so far)` is asked before each row (it
// runs on a worker); false stops there -- what went in is KEPT as the one
// step (Ctrl+Z takes it back), stopped_at says where.
std::expected<ImportResult, Error> import_records(Journal& j, const Table& t, const Delimited& d, const ImportPlan& p,
                                                  const std::function<bool(std::int64_t)>& keep_going = {});

// "Import 40 records into “contacts”"
std::string import_label(std::int64_t n, const std::string& table);
// s024a: "Import into “contacts” -- 3 updated, 2 added"
std::string import_label(const ImportResult& r, const std::string& table);
// "Imported 38 records into “contacts” -- 2 refused" (no refusals: no tail);
// stopped: "... -- stopped before line 4201, the rest weren't read"
std::string import_says(const ImportResult& r, const std::string& table);
// "Line 7: “birthday” needs a date like 1990-07-14"
std::string refusal_says(const ImportRefusal& r);

}  // namespace acorn::db
