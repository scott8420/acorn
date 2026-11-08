// db/Starter.hpp -- starter databases (s009b): a new file that arrives with
// tables, links and a few records, made through the door like everything
// else (the caller clears the journal after: the starter is not the user's
// step to undo).
#pragma once

#include "core/Layout.hpp"
#include "db/Journal.hpp"

#include <expected>

namespace acorn::db {

// The Address Book: contacts; addresses and phones, each pointing at a
// contact (Cascade -- a contact's addresses and phones go with it, and
// Ctrl+Z brings them all back); three made-up people. Refused if any of the
// three tables is already there.
std::expected<void, Error> make_address_book(Journal& j);

// s010f: the SAMPLE Address Book Acorn opens at startup -- the same three
// tables and links, twelve made-up people (so Browse has pages of cards and
// the Graph has something to show), and the kept layout below. The caller
// clears the journal after, as for the starter.
std::expected<void, Error> make_sample(Journal& j);

// The sample's kept layout for contacts: "Contact Card" -- a header, a box
// framing the name rows, a rule, words and fields on the 8 px grid, a footer.
// Shows what the layout editor can do; passes core::check_layout.
core::Layout sample_layout();
inline constexpr const char* kSampleLayoutName = "Contact Card";
// s012b: and "Contact List" -- a list-shaped layout for Browse -> List: column
// titles (left-aligned words) in the header, one row of fields per person,
// a footer. Passes core::check_layout.
core::Layout sample_list_layout();
inline constexpr const char* kSampleListName = "Contact List";

}  // namespace acorn::db
