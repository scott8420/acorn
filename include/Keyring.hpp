#pragma once
#include <functional>
#include <optional>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Keyring -- server passwords in the GNOME keyring, via libsecret (s004b, Q2).
//
// Acorn never writes a password to disk itself. A password lives in the
// Connect form only until the open call; if "Remember the password" is ticked
// it goes to the Secret Service (gnome-keyring on GNOME), keyed by the same
// fields the recent-connections list keeps -- engine, host, port, database,
// user -- under the schema "io.github.scott8420.Acorn.Connection". You can see
// and delete the entries in Seahorse ("Passwords and Keys").
//
// All three calls are ASYNC: the Secret Service is a D-Bus round trip, and a
// locked keyring pops an unlock prompt that may sit there for a while. `done`
// runs later on the thread's default main context (the GTK main loop in the
// app; the selftest spins the context itself). Never blocks the window.
//
// GTK-free (glib + libsecret only), so the selftest links it.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::keyring {

struct Account {
    std::string engine;     // "mariadb", "postgres"
    std::string host;       // "" is stored as "localhost"
    int         port = 0;   // 0 = the engine's default
    std::string database;
    std::string user;
    std::string label;      // what Seahorse shows: "Acorn -- MariaDB: shop on localhost (scott)"
};

// ok=false carries the keyring's own words in `error`.
void store(const Account& a, const std::string& password,
           std::function<void(bool ok, std::string error)> done);

// pw empty-optional = nothing stored (error "" ) or the keyring failed (error set).
void lookup(const Account& a,
            std::function<void(std::optional<std::string> pw, std::string error)> done);

// removed=true when an entry was there and is gone.
void clear(const Account& a, std::function<void(bool removed, std::string error)> done);

// The schema name, for the selftest and the docs.
const char* schema_name();

}  // namespace acorn::keyring
