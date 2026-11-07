#pragma once
#include <cstddef>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Connections -- the recent-connections list + its JSON pump (s004b).
// Replaces Cairn's folder recents (core/Recents): same discipline, new shape.
//
// GTK-free, headless-testable: pure list ops + encode/decode IN ONE FILE so a
// write can't skew from its read (CANON: pumps at conceptual seams). The UI
// resolves the XDG path and hands it in (docs/two-layer-seam.md).
//
// A saved connection is everything needed to connect again EXCEPT the
// password. Passwords never touch this file -- they live in the GNOME keyring
// (Keyring.hpp), keyed by the same engine/host/port/database/user.
//
// On disk: { "connections": [ {engine, path | host, port, database, user}, ... ] },
// first == most recent. load is first-run tolerant, drops file entries whose
// file has vanished (a server entry is never pruned -- the server may just be
// down), dedupes by key(). Round-trip fidelity is the bar (CANON).
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::core {

inline constexpr std::size_t kMaxConnections = 8;

struct SavedConnection {
    std::string engine;     // engine id: "sqlite", "mariadb", "postgres"
    std::string path;       // file engines
    std::string host;       // server engines ("" = localhost)
    int         port = 0;   // 0 = the engine's default
    std::string database;
    std::string user;

    bool is_file() const { return !path.empty(); }
    // Identity: two entries with the same key are the same connection.
    std::string key() const;
    // What a person reads: "shop.sqlite" / "shop on db.lan".
    std::string label() const;
    // The second line: the full path / "scott@db.lan:5433".
    std::string detail() const;

    bool operator==(const SavedConnection&) const = default;
};

// Pure list ops. add: move-to-front (by key) + trim to `max`.
void connections_add(std::vector<SavedConnection>& list, const SavedConnection& c,
                     std::size_t max = kMaxConnections);
void connections_remove(std::vector<SavedConnection>& list, const std::string& key);
const SavedConnection* connections_find(const std::vector<SavedConnection>& list,
                                        const std::string& key);

// Persistence pump -- encode + decode adjacent so they can't drift.
std::vector<SavedConnection> load_connections(const std::string& file);
bool save_connections(const std::string& file, const std::vector<SavedConnection>& list);

}  // namespace acorn::core
