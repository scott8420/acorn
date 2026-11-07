// db/Types.hpp -- Acorn's field types and the map from each engine's own type
// names onto them (REQUIREMENTS §3).
//
// The map is shared, not per plugin, so the three engines can't drift: s003's
// MariaDB and PostgreSQL plugins call the same function with their dialect. The
// engine's declared type is always kept verbatim beside the mapped one (Column
// ::decl_type) -- Acorn never forgets what the database actually said.
#pragma once

#include <string>
#include <string_view>

namespace acorn::db {

enum class FieldType {
    Text, Integer, Number, Date, Time, Timestamp, Boolean, Container, Unknown,
};

enum class Dialect { SQLite, MariaDB, PostgreSQL };

const char* to_string(FieldType t);
const char* to_string(Dialect d);

// Map a declared column type ("VARCHAR(40)", "int unsigned", "timestamp with
// time zone", "" ...) to a FieldType. Case-insensitive; size/precision ignored.
// SQLite follows its affinity rules (sqlite.org/datatype3.html §3.1) with
// date/time/bool names recognised first, so "DATETIME" maps to Timestamp rather
// than to SQLite's NUMERIC affinity.
FieldType map_type(Dialect d, std::string_view decl_type);

} // namespace acorn::db
