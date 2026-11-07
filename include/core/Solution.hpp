#pragma once
#include "core/Connections.hpp"
#include "core/Layout.hpp"

#include <map>
#include <optional>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// core/Solution -- the .acorn solution document (REQUIREMENTS §1.3, s009).
// Acorn's own side of a database: it POINTS AT a connection and keeps what the
// database itself can't -- in s009, where each table's box sits in the Graph.
// Layouts, finds, reports ... join it as they come.
//
// Where it lives (Scott, s009): a database FILE's solution sits beside it
// (shop.sqlite -> shop.acorn); a server's (and a file whose folder can't be
// written) in Acorn's own folder, ~/.local/share/acorn/solutions/. Nothing is
// written until something is placed.
//
// On disk: { "acorn": 1, "points_at": {engine, path | host, port, database,
// user}, "boxes": { "<table>": {"x": .., "y": ..}, ... }, "layouts": { "<table>":
// { "<name>": {"width", "height", "objects": [{"kind": "field" | "text",
// "field" | "text", "x", "y", "w", "h"}, ...]}}} } -- never a password. (s010:
// layouts added; a file without them reads as none.)
// Written whole or not at all (tmp + rename). GTK-free, headless-testable.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::core {

inline constexpr int kSolutionVersion = 1;

struct BoxPlace {
    double x = 0, y = 0;
    bool operator==(const BoxPlace&) const = default;
};

struct Solution {
    SavedConnection                 points_at;
    std::map<std::string, BoxPlace> boxes;     // by table name
    // s010: by table, then by name. A layout's own table / name match its keys.
    std::map<std::string, std::map<std::string, Layout>> layouts;
    const Layout* layout(const std::string& table, const std::string& name) const;
    bool operator==(const Solution&) const = default;
};

// Where `c`'s solution lives. `app_dir` is Acorn's own data folder
// (~/.local/share/acorn); `beside_ok` says whether the file's folder may be
// written (the UI asks the filesystem).
std::string solution_path(const SavedConnection& c, const std::string& app_dir, bool beside_ok);
// Whether a new file may be made in `dir` (the folder exists and is writable).
bool folder_writable(const std::string& dir);

// nullopt: no file, or not an Acorn solution (garbage, a newer version).
std::optional<Solution> load_solution(const std::string& file);
bool save_solution(const std::string& file, const Solution& s);

}  // namespace acorn::core
