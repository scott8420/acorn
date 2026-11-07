// db/Value.hpp -- the one value type that crosses every engine.
//
// Five storage shapes, the common ground of SQLite, MariaDB and PostgreSQL:
// NULL, a 64-bit integer, a double, UTF-8 text, and a blob. Everything richer
// (dates, decimals, booleans) travels as one of these and is interpreted by the
// column's FieldType (db/Types.hpp) -- an engine never invents a sixth shape.
// GTK-free; lives in acorn_db, which both the app and the engine plugins link.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace acorn::db {

using Blob = std::vector<std::uint8_t>;

class Value {
public:
    enum class Kind { Null, Integer, Real, Text, Blob };

    Value() = default;                                  // NULL
    static Value null() { return {}; }
    Value(std::int64_t v) : v_(v) {}
    Value(int v) : v_(static_cast<std::int64_t>(v)) {}
    Value(double v) : v_(v) {}
    Value(std::string v) : v_(std::move(v)) {}
    Value(const char* v) : v_(std::string(v)) {}
    Value(Blob v) : v_(std::move(v)) {}

    Kind kind() const { return static_cast<Kind>(v_.index()); }
    bool is_null() const { return kind() == Kind::Null; }

    // Exact accessors: nullopt when the value is a different kind. No silent
    // coercion -- the caller asks for what it expects and learns when it isn't.
    std::optional<std::int64_t> as_int() const;
    std::optional<double>       as_real() const;     // Integer widens to double
    const std::string*          as_text() const;
    const Blob*                 as_blob() const;

    // A human/debug rendering: NULL, 42, 3.5, 'text', x'0aff'. Not SQL-safe --
    // values reach SQL only as bound parameters, never by string splicing.
    std::string to_display() const;

    bool operator==(const Value&) const = default;

private:
    std::variant<std::monostate, std::int64_t, double, std::string, Blob> v_;
};

using Row = std::vector<Value>;

struct ColumnInfo {
    std::string name;
    std::string decl_type;   // as the engine reported it ("" if unknown)
};

// The answer to one statement. Statements that return no rows still carry
// `affected` (and `last_insert_id` where the engine reports one).
struct Result {
    std::vector<ColumnInfo> columns;
    std::vector<Row>        rows;
    std::int64_t            affected = 0;
    std::optional<std::int64_t> last_insert_id;

    int column_index(const std::string& name) const;   // -1 if absent
};

// Every failure in the db layer is one of these, carried by std::expected.
struct Error {
    enum class Code {
        NotFound,        // file / database / engine absent
        Open,            // could not connect / open
        ReadOnly,        // a write on a read-only connection
        Syntax,          // the engine rejected the statement
        Constraint,      // a constraint stopped the write
        Busy,            // locked
        Abi,             // a plugin built against another ABI
        Unsupported,     // the engine lacks the capability
        Internal,
        Exists,          // s005a: refused -- the file is already there (appended: no renumbering)
        Changed,         // s005b: refused -- the database no longer looks the way the step left it
    };
    Code code = Code::Internal;
    std::string message;     // the engine's own words where it gave any
};

const char* to_string(Error::Code c);

} // namespace acorn::db
