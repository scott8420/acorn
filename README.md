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

> **Status:** s001 -- the spine. Acorn stands on the Cairn starter; nothing
> database-specific yet.

## Build

```bash
# Fedora
sudo dnf install gtkmm4.0-devel spdlog-devel cmake gcc-c++ ninja-build \
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
