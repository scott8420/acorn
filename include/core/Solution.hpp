#pragma once
#include "core/Calc.hpp"
#include "core/Connections.hpp"
#include "core/Layout.hpp"
#include "core/Script.hpp"
#include "core/ValueList.hpp"

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
    // s014a: calculation fields, by table then name (core/Calc). On disk:
    // "calcs": { "<table>": { "<name>": {"expr": ".."} } } -- absent reads as none.
    std::map<std::string, std::map<std::string, Calc>> calcs;
    const Calc* calc(const std::string& table, const std::string& name) const;
    const std::map<std::string, Calc>& calcs_of(const std::string& table) const;
    // s014b: summaries, by table then name. On disk: "summaries": { "<table>":
    // { "<name>": {"op": "total", "of": "<field>"} } } -- absent reads as none.
    std::map<std::string, std::map<std::string, Summary>> summaries;
    const Summary* summary(const std::string& table, const std::string& name) const;
    const std::map<std::string, Summary>& summaries_of(const std::string& table) const;
    // s014c: value lists, by table then name. On disk: "value_lists": { "<table>":
    // { "<name>": {"values": [..]} | {"from": t, "field": f, "shows": s?} } }.
    std::map<std::string, std::map<std::string, ValueList>> lists;
    // s025b: a table's list by name -- its own, else a SHARED one (kept under
    // the table "", offered by every table's layouts).
    const ValueList* list(const std::string& table, const std::string& name) const;
    const ValueList* list_exact(const std::string& table, const std::string& name) const;   // no falling back
    const std::map<std::string, ValueList>& lists_of(const std::string& table) const;       // its own ("" : the shared)
    std::map<std::string, ValueList> lists_for(const std::string& table) const;             // its own + the shared
    // The names a new / renamed list of `table` may not take: a table's -- its
    // own and the shared ones; a shared one's ("") -- every list's.
    std::vector<std::string> list_names_taken(const std::string& table) const;
    // s017a: scripts, by table then name. On disk: "scripts": { "<table>":
    // { "<name>": [ {"step": "find", "field": f, "text": t}, ... ] } } --
    // a step's keys only when they say something.
    std::map<std::string, std::map<std::string, Script>> scripts;
    const Script* script(const std::string& table, const std::string& name) const;
    const std::map<std::string, Script>& scripts_of(const std::string& table) const;
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
