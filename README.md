# Acorn

**Databases you design by drawing them.** A FileMaker-style database designer
for GNOME, written in C++23 with gtkmm 4.

You draw tables and the relationships between them, lay out forms by drag and
drop, find records by filling in the form itself, and print reports and labels.
Underneath sit real SQL engines -- SQLite, MariaDB / MySQL and PostgreSQL --
so your data is never locked in Acorn's format. The *solution* (layouts,
scripts, reports, labels) lives in its own document, beside the data, and can
point at any engine.

Acorns grow into trees. So do schemas.

> **Status:** s004 -- connect to SQLite, MariaDB or PostgreSQL **read-only**
> and browse the schema: every table's columns, types, keys, indexes and links.
> File -> New Database File (Ctrl+N) makes an empty SQLite file -- it never
> overwrites anything. Recent connections are remembered; server passwords go
> to the GNOME keyring (libsecret), never to a file. Nothing writes yet.

## Engines

Each engine is a plugin, `acorn-engine-<id>.so`, built over that engine's own
C client library and installed to `<prefix>/lib/acorn/engines/`. An engine
whose client library isn't installed simply isn't offered.

| Engine | Plugin | Status |
|---|---|---|
| SQLite | `acorn-engine-sqlite.so` (libsqlite3) | working |
| MariaDB / MySQL | `acorn-engine-mariadb.so` (MariaDB Connector/C) | working |
| PostgreSQL | `acorn-engine-postgres.so` (libpq) | working |

Acorn SQL uses `?` placeholders on every engine (rewritten to `$n` for
PostgreSQL), and values are always bound, never spliced into the SQL.

### Passwords

A server password is used for the connect and then dropped. With
"Remember the password in the GNOME keyring" ticked, it is stored in the
Secret Service under `io.github.scott8420.Acorn.Connection` -- visible and
deletable in Seahorse ("Passwords and Keys"). The recent-connections list
(`~/.local/share/acorn/connections.json`) holds engine, host, port, database
and user only. Forgetting a recent (its x) also removes its stored password.
`ACORN_TEST_KEYRING=1 ./build/acorn_selftest` adds a keyring round-trip (one
test entry, user `acorn_selftest`, removed again).

### Testing against servers

`./build/acorn_selftest` runs the db suite on SQLite always, and on a server
only when you name one:

```bash
ACORN_TEST_MARIADB="host=127.0.0.1 port=3306 user=me password=..." \
ACORN_TEST_POSTGRES="host=127.0.0.1 port=5432 user=me password=..." \
./build/acorn_selftest
```

The user needs the right to create databases. The test creates one database
named `acorn_selftest_<pid>`, works only inside it, and drops it. Without a
variable that engine is reported *skipped*.

## Build

```bash
# Fedora
sudo dnf install gtkmm4.0-devel spdlog-devel libsecret-devel cmake gcc-c++ ninja-build \
                 sqlite-devel mariadb-connector-c-devel libpq-devel
./build.sh
./build/acorn_selftest
```

## Install

```bash
./install.sh            # to ~/.local, no sudo
./install.sh --system   # to /usr/local
./uninstall.sh          # removes exactly what install.sh wrote
```

## License

MIT. See `LICENSE`.
