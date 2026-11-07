// db/NewFile.hpp -- make a new, empty database file (s005a).
//
// The one place Acorn creates a database. It NEVER touches an existing file:
// if anything is at the path -- a database, an empty file, a directory -- it
// refuses with Error::Code::Exists and leaves it alone. On success the file
// is a valid, empty database stamped with Acorn's application id (so `file`
// and other tools see a real SQLite database, not a zero-byte file), and the
// connection that made it is closed again: the caller opens it the normal
// way. If the create fails part-way, a file it made is removed.
#pragma once

#include "db/Engine.hpp"

#include <expected>
#include <filesystem>

namespace acorn::db {

// SQLite's PRAGMA application_id for files Acorn made: 'A' 'c' 'o' 'r'.
inline constexpr int kAcornApplicationId = 0x41636F72;

std::expected<void, Error> new_file(Engine& engine, const std::filesystem::path& path);

} // namespace acorn::db
