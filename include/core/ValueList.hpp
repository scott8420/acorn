#pragma once
// core/ValueList -- value lists (s014c). A VALUE LIST lives in the SOLUTION,
// never the database: a named set of values a layout's field box offers as a
// drop-down, so a pick saves exactly what typing it would have.
//
// It belongs to a table -- the one whose layouts offer it (as calculations and
// summaries do) -- and is one of two kinds:
//   typed      -- the values, one per line, in the order typed
//                 ("home", "work", "mobile");
//   from field -- every distinct value of a field of a table (that table, or
//                 another: contacts.city, or contacts.id for phones.contact_id).
//                 With `shows`, another field of that table is shown in place
//                 of the value (contacts.id showing last_name): the pick saves
//                 the id, the person sees the name.
// GTK-free, headless-testable.
#include <string>
#include <vector>

namespace acorn::core {

struct ValueList {
    std::string table;                // the table whose layouts offer it
    std::string name;                 // unique among the table's value lists
    std::vector<std::string> values;  // typed: the values, in order
    std::string from;                 // from a field: the table it reads
    std::string field;                // ... the field whose values it offers (what a pick saves)
    std::string shows;                // ... optional: a field of `from` shown in place of the value
    // s025a: only its values -- a box offering it is a pick-only drop-down
    // (no typing), as one showing another field always was.
    bool        strict = false;
    bool typed() const { return field.empty(); }
    bool operator==(const ValueList&) const = default;
};

// One choice as offered: what a pick saves, and what the person reads.
struct ListItem {
    std::string value, shown;
    bool operator==(const ListItem&) const = default;
};

// Typed text -> values: one per line, each trimmed; blank lines and a value
// already there are left out (the first stays where it was).
std::vector<std::string> parse_list_values(const std::string& text);
// The values back as lines ("home\nwork").
std::string list_values_text(const std::vector<std::string>& values);

// A value list's name: "" = fine, else why. Any words but not empty, no
// line breaks, no spaces at either end, at most 64 characters.
std::string check_value_list_name(const std::string& name);

// The whole check before a list is kept: its name; not one of `taken` (the
// table's other lists); typed -- at least one value; from a field -- `fields_of`
// knows `from` (nullptr: no such table), `field` is one of them, `shows` (when
// given) is another one of them. "" = fine.
std::string check_value_list(const ValueList& v, const std::vector<std::string>& taken,
                             const std::vector<std::string>* from_fields);

// How the Tables area says what a list offers: "home · work · mobile" (the
// first few, then "+ 3 more"), "every “city” of “contacts”", "“id” of
// “contacts”, showing “last_name”".
std::string value_list_says(const ValueList& v);

// The items a typed list offers (value = shown).
std::vector<ListItem> typed_items(const ValueList& v);

// The most a list read from a field offers (the rest are left out, and the
// editor says so).
inline constexpr int kValueListMax = 500;

}  // namespace acorn::core
