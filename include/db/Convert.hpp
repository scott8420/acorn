// db/Convert.hpp -- what a value becomes when its field changes kind (s007c).
//
// Acorn converts the values itself, by ONE set of rules (the ones typing a
// value in the record panel uses -- db::parse_field), rather than trusting each
// engine's CAST. So the designer's words ("3 of “age”'s values won't fit a
// whole number -- they become empty") and what the step actually does are the
// same function, on every engine. GTK-free.
//
//   to Text       a number as written (12, 12.5), Yes-No as Yes / No, text as
//                 is; a blob won't fit
//   to Integer    a whole number; a real only when it is whole (12.0 -> 12,
//                 12.5 won't fit -- no silent rounding); text that reads as a
//                 whole number (spaces round it allowed)
//   to Number     any number; text that reads as one
//   to Yes / No   1 / 0 (any whole 1 or 0), yes / no / true / false
//   to Date       text shaped YYYY-MM-DD (a date and time won't fit -- a step
//                 never cuts a value)
//   to Time       text shaped HH:MM[:SS[.fff]]
//   to Timestamp  text shaped YYYY-MM-DD HH:MM[:SS[.fff]] (or a T between); a
//                 date alone gains 00:00:00
//   to Container  only a blob
// NULL is NULL everywhere. Unknown (a type Acorn doesn't map) takes any value
// as it is.
#pragma once

#include "db/Types.hpp"
#include "db/Value.hpp"

#include <expected>
#include <optional>
#include <string>

namespace acorn::db {

// `from` is the field's kind now (it decides Yes-No -> "Yes"); nullopt = the
// value won't fit `to`.
std::optional<Value> convert_value(const Value& v, FieldType from, FieldType to);

// Does `v` already have the shape a field of kind `t` stores? (An int in an
// Integer field, text in a Text / Date field ...) -- the store puts such a
// value back untouched.
bool native_to(const Value& v, FieldType t);

// "a whole number", "a number", "text", "a date", "a time", "a date and time",
// "Yes or No", "binary data".
std::string kind_words(FieldType t);

// ── defaults ────────────────────────────────────────────────────────────────
// What the designer shows for a field's default SQL: 'it''s'::text -> it's,
// 0 -> 0, CURRENT_TIMESTAMP -> CURRENT_TIMESTAMP. "" when there is none.
std::string default_text(const std::optional<std::string>& sql);
// The SQL a default typed in the designer becomes, for a field of kind t:
// "" -> none (nullopt); numbers bare, Yes / No -> 1 / 0 (TRUE / FALSE on
// PostgreSQL), everything else a quoted literal. Refused -> the reason in
// words ("the default for “age” needs a whole number").
std::expected<std::optional<std::string>, std::string>
default_sql_for(Dialect d, FieldType t, const std::string& field, const std::string& typed);

}  // namespace acorn::db
