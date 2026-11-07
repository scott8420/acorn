// db/Browse.hpp -- reading a table's records a page at a time (s006b).
// GTK-free: the Browse area paints what read_page returns, and the selftest
// drives the same call on every engine.
//
// A page is records [offset, offset + limit) in a stable order: the sort
// column if one is asked for, then the primary key (so equal sort values never
// shuffle between pages); no key -> the engine's own order. Each record comes
// with its journal id (Store::row_id) when the table is writable -- that id is
// what an edit names (s006c) -- else "".
//
// Reads only: everything here is a SELECT, so it passes the Gate with the door
// shut.
#pragma once

#include "db/Engine.hpp"
#include "db/Journal.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace acorn::db {

struct PageRequest {
    std::int64_t offset = 0;
    std::int64_t limit  = 50;
    std::string  sort;            // a column name; "" = the key order
    bool         descending = false;
};

struct Page {
    std::vector<std::string> columns;   // the table's columns, in order
    std::vector<Row>         rows;
    std::vector<Store::Id>   ids;       // per row; "" when the table isn't writable by key
    std::int64_t             total = 0; // every record in the table
    std::int64_t             offset = 0;
};

// The SELECT a page runs (exposed so the selftest can read it).
std::string page_sql(const Connection& c, const Table& t, const PageRequest& r);

std::expected<Page, Error> read_page(Connection& c, const Table& t, const PageRequest& r);

// One cell as Browse shows it: NULL -> "", text as-is, a Yes-No field 0/1 ->
// "No" / "Yes", a real without a trailing ".0" mess, a blob -> "(n bytes)".
std::string cell_text(const Value& v, FieldType t);

// The words a card leads with: the first non-key Text field that holds
// something, else the key, else "(empty)".
std::string card_title(const Table& t, const Row& row);

// s006c -- what a typed field becomes. The inverse of cell_text where it can
// be: "" -> NULL when the column allows it (else "" for text, refused for
// the rest); Integer / Yes-No -> a whole number (Yes-No also takes yes / no /
// true / false); Number -> a number; text, dates, times -> the text as typed
// (the engine judges a date). A blob is not typed here. Refused -> the
// reason in words ("“age” needs a whole number").
std::expected<Value, std::string> parse_field(const std::string& typed, const Column& c);

// "51–100 of 312" (en dash); "No records" when empty.
std::string page_range(const Page& p);

}  // namespace acorn::db
