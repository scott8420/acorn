// db/Schema.hpp -- what introspection reads out of a database Acorn may not
// have created (REQUIREMENTS §1.4). Plain data; engines fill it, the UI and the
// solution document read it. Names are the engine's own, unquoted.
#pragma once

#include "db/Types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acorn::db {

struct Column {
    std::string name;
    std::string decl_type;                 // verbatim from the engine
    FieldType   type = FieldType::Unknown; // map_type(dialect, decl_type)
    bool        nullable = true;
    std::optional<std::string> default_sql;  // the default as SQL text, if any
    int         pk_position = 0;           // 1-based within the primary key; 0 = not in it
    bool        auto_increment = false;    // rowid alias / AUTO_INCREMENT / IDENTITY / serial
};

struct ForeignKey {
    std::string              name;         // "" where the engine doesn't name them (SQLite)
    std::vector<std::string> columns;
    std::string              ref_table;
    std::vector<std::string> ref_columns;  // may be empty in SQLite = the ref table's PK
    std::string              on_update;    // "NO ACTION", "CASCADE", ...
    std::string              on_delete;
};

struct Index {
    std::string              name;
    std::vector<std::string> columns;      // expression parts appear as "<expr>"
    bool                     unique = false;
    bool                     primary = false;  // the index backing the primary key
    bool                     partial = false;  // has a WHERE
};

struct Table {
    enum class Kind { Table, View };
    std::string              name;
    Kind                     kind = Kind::Table;
    std::vector<Column>      columns;
    std::vector<ForeignKey>  foreign_keys;
    std::vector<Index>       indexes;

    const Column* column(const std::string& n) const;
    std::vector<std::string> primary_key() const;   // in key order
};

struct Schema {
    std::vector<Table> tables;               // tables then views, each by name
    const Table* table(const std::string& n) const;
};

} // namespace acorn::db
