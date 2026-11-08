// db/Accounts.hpp -- the engine's own accounts and what each may do (s019a).
// GTK-free: the Accounts area paints what read_accounts returns; the selftest
// drives the same call on every engine.
//
// What an account may do is said per table in four words -- read, add,
// change, delete (SELECT / INSERT / UPDATE / DELETE) -- never as SQL.
//   MariaDB: the server's accounts (user@host, from mysql.user), and their
//     grants from information_schema: server-wide (USER_PRIVILEGES), on this
//     database (SCHEMA_PRIVILEGES -- a grant on a pattern like `shop\_%`
//     counts when this database's name matches it), per table
//     (TABLE_PRIVILEGES). PUBLIC's grants count for everyone. Without the
//     right to read mysql.user only your own account is shown -- and said.
//   PostgreSQL: the roles (pg_roles, without the built-in pg_*), and per
//     table of this schema what has_table_privilege says (it folds in
//     ownership, PUBLIC and the roles a role is a member of); a superuser may
//     do everything; a role without CONNECT on this database reaches nothing.
//   SQLite: no accounts -- said in words.
//
// Reads only: every query is a SELECT, so it passes the Gate with the door shut.
#pragma once

#include "db/Engine.hpp"
#include "db/Schema.hpp"
#include "db/Types.hpp"

#include <expected>
#include <map>
#include <string>
#include <vector>

namespace acorn::db {

// What an account may do with a table's records: a set of these.
enum Priv : unsigned { kRead = 1, kAdd = 2, kChange = 4, kDelete = 8, kAllPrivs = 15 };
using Privs = unsigned;

// "read · add · change · delete" in that order; none -> "nothing".
std::string privs_says(Privs p);
// SELECT / INSERT / UPDATE / DELETE (any case) -> its bit; anything else 0.
Privs priv_of(const std::string& sql_word);

struct Account {
    std::string name;
    std::string host;            // MariaDB's "@host" part; "" elsewhere
    bool        you = false;     // the account this connection uses
    bool        login = true;    // PostgreSQL: a role that can't log in is a group
    bool        super = false;   // may do everything, everywhere
    bool        admin = false;   // may make accounts / give rights to others
    bool        reaches = true;  // PostgreSQL: CONNECT on this database
    Privs       every = 0;       // what it may do with every table here (server-wide + this database)
    std::map<std::string, Privs> tables;   // per table: everything it may do there (every folded in)
};

struct AccountList {
    bool                 supported = true;  // false: SQLite -- `why` says so
    std::string          why;
    bool                 only_you = false;  // the list couldn't be read: just this connection's account
    std::vector<Account> accounts;          // sorted by name (then host)
};

// "acorn@localhost" (MariaDB), "acorn" (PostgreSQL).
std::string account_name(const Account& a);
// "you · may do everything · makes accounts · can't log in · can't connect here" -- what applies.
std::string account_tags(const Account& a);
// What it may do with `tables` (in their order), in lines: one "Every table:
// read · add" when they're all alike, else "contacts: read" per table.
std::vector<std::string> account_lines(const Account& a, const std::vector<std::string>& tables);

// MariaDB's GRANTEE "'acorn'@'localhost'" -> {"acorn", "localhost"}; CURRENT_USER()'s "acorn@localhost" too.
std::pair<std::string, std::string> parse_grantee(const std::string& g);

// The accounts and their rights over `s`'s tables. SQLite: supported = false.
std::expected<AccountList, Error> read_accounts(Connection& c, Dialect d, const Schema& s);

}  // namespace acorn::db
