// db/Starter.hpp -- starter databases (s009b): a new file that arrives with
// tables, links and a few records, made through the door like everything
// else (the caller clears the journal after: the starter is not the user's
// step to undo).
#pragma once

#include "db/Journal.hpp"

#include <expected>

namespace acorn::db {

// The Address Book: contacts; addresses and phones, each pointing at a
// contact (Cascade -- a contact's addresses and phones go with it, and
// Ctrl+Z brings them all back); three made-up people. Refused if any of the
// three tables is already there.
std::expected<void, Error> make_address_book(Journal& j);

}  // namespace acorn::db
