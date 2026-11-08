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

#include "core/Calc.hpp"
#include "core/ValueList.hpp"
#include "db/Engine.hpp"
#include "db/Journal.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

namespace acorn::db {

struct PageRequest {
    std::int64_t offset = 0;
    std::int64_t limit  = 50;
    std::string  sort;            // a column name; "" = the key order
    bool         descending = false;
    // s015b: a report's groups -- ordered by these first (each ascending),
    // then by `sort`, then the key.
    std::vector<std::string> group = {};
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

// s011a -- a portal's rows: the records of `t` whose `link` fields hold
// `values` (a contact's phones: phones where contact_id = 7), in key order,
// a page of them; `total` counts every match. The values are bound, never
// written into the SQL. A NULL value matches nothing (no record points at a
// record that has no key).
std::string related_sql(const Connection& c, const Table& t, const std::vector<std::string>& link, const PageRequest& r);
std::expected<Page, Error> read_related(Connection& c, const Table& t, const std::vector<std::string>& link,
                                        const std::vector<Value>& values, const PageRequest& r);

// s013a -- FIND: query by form. A criterion is what was typed in one box of
// the Find layout; the records that match EVERY criterion are the found set.
// Blank boxes ask nothing. What a box asks, by what it starts with:
//   "=" alone        the field is empty (NULL; for text, also "")
//   "=x"             exactly x
//   "<x" "<=x" ">x" ">=x"   less / greater, as the engine orders the field
//   "a..b" ("a...b") a RANGE, a to b inclusive (s022); text any case
//   anything else    text: CONTAINS x, any case (ASCII letters); the rest: = x
// Values are bound (never written into the SQL); a value the field can't
// hold is refused in words ("“id” needs a whole number"), as is a field the
// table hasn't, a binary field, and no criterion at all.
// s013b: a criterion can reach THROUGH a link -- `table` names another table
// and the record matches when `outer` (a field of this table) is IN the
// `inner` values of that table's records whose `field` matches:
//   a portal's column (contacts with a phone "0177"):
//       {field "number", table "phones", outer "id", inner "contact_id"}
//   a related field (addresses whose contact's last name has "th"):
//       {field "last_name", table "contacts", outer "contact_id", inner "id"}
// `table` empty: `field` is this table's own.
struct Criterion {
    std::string field;
    std::string typed;
    std::string table = {};
    std::string outer = {};
    std::string inner = {};
    // s022: which REQUEST of the find the box belongs to, and whether that
    // request OMITS. Boxes of one request must all match (AND); a record found
    // by ANY non-omit request is in (OR) -- unless any omit request matches it.
    // Only omit requests: every record but those. The script / export /
    // summary paths pass these along untouched.
    int  request = 0;
    bool omit = false;
    bool operator==(const Criterion&) const = default;
};
struct FindQuery {
    std::string        where;    // " WHERE ..." (with the leading space)
    std::vector<Value> values;
};
// `s` resolves a criterion's other table; without it such a criterion is refused.
std::expected<FindQuery, std::string> find_query(const Connection& c, const Table& t, const std::vector<Criterion>& crit,
                                                 const Schema* s = nullptr);
// A page of the found set; `total` counts every match. A refusal is an Error
// (Code::Unsupported) carrying the words.
std::expected<Page, Error> read_found(Connection& c, const Table& t, const std::vector<Criterion>& crit,
                                      const PageRequest& r, const Schema* s = nullptr);
// "last_name contains “a” · company is empty" -- the non-blank criteria, in
// words, for the line under Browse.
std::string find_summary(const std::vector<Criterion>& crit);

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

// s014a -- calculation fields over a record. A field's value as the
// calculation language sees it: NULL -> empty; a whole number / real -> a
// number; a number field the engine gave as text ("12.50") -> a number; a
// Yes-No field -> 1 / 0 (also "t" / "f"); dates, times, text -> text; a blob ->
// empty.
core::CalcValue calc_value(const Value& v, FieldType t);
// Every calculation of `t` for one of its records (`row` in the table's column
// order), by name.
// s014b: `extra` -- the table's summaries' values, read like fields.
std::map<std::string, core::CalcOutcome> row_calcs(const Table& t, const Row& row,
                                                   const std::map<std::string, core::Calc>& calcs,
                                                   const std::string& today,
                                                   const std::map<std::string, core::CalcValue>* extra = nullptr);

// s014b -- SUMMARIES, worked out by the database: ONE SELECT of every
// summary of `t` (COUNT(f) / SUM / AVG / MIN / MAX) over the records the
// criteria find (none: every record) -- the same WHERE as the found set,
// values bound. A summary whose field the table no longer has is left out
// (its value: "?" with the words, by the caller). Count counts the records
// whose field holds something; an empty set's Total / Average / Min / Max is
// empty. The values as the language sees them: Count / Total / Average a
// number; Min / Max as the field is (a date stays ISO text).
std::string summary_sql(const Connection& c, const Table& t, const std::vector<core::Summary>& sums,
                        const std::string& where);
std::expected<std::map<std::string, core::CalcValue>, Error>
read_summaries(Connection& c, const Table& t, const std::map<std::string, core::Summary>& sums,
               const std::vector<Criterion>& crit = {}, const Schema* s = nullptr);

// s015b -- SUMMARIES BY GROUP, for a report's sub-summaries: ONE SELECT per
// group level -- `by` (the group's fields, outermost first), then each
// summary, "GROUP BY" `by` -- over the records the criteria find. Each group
// keyed by its fields' values as Browse shows them (cell_text), in `by`'s
// order. A summary whose field is gone is left out (as read_summaries).
std::string group_summary_sql(const Connection& c, const Table& t, const std::vector<core::Summary>& sums,
                              const std::vector<std::string>& by, const std::string& where);
using GroupSums = std::map<std::vector<std::string>, std::map<std::string, core::CalcValue>>;
std::expected<GroupSums, Error>
read_group_summaries(Connection& c, const Table& t, const std::map<std::string, core::Summary>& sums,
                     const std::vector<std::string>& by, const std::vector<Criterion>& crit = {},
                     const Schema* s = nullptr);

// s014c -- what a value list offers. Typed: its values. From a field: ONE
// SELECT DISTINCT of the field (and the shown field) of `from`, empties left
// out, in the database's order of what is shown, at most core::kValueListMax
// (`more`: there were others). Each item's value is the field's as Browse
// shows it (cell_text) -- what a pick types into the box.
struct ListItems {
    std::vector<core::ListItem> items;
    bool more = false;
};
std::string value_list_sql(const Connection& c, const core::ValueList& v);
std::expected<ListItems, Error> read_value_list(Connection& c, const Schema& s, const core::ValueList& v);

// "51–100 of 312" (en dash); "No records" when empty.
std::string page_range(const Page& p);

}  // namespace acorn::db
