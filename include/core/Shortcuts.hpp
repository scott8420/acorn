#pragma once
// core::Shortcuts -- the SHORTCUT-REGISTRY paradigm: a key binding and the row
// that advertises it are ONE fact, declared once.
//
// The stone this carries: a shortcut has two consumers that must never disagree
// -- the wiring (set_accels_for_action) and the documentation (the shortcuts
// window). Declare them separately and they drift the first time a key changes;
// the app then lies to its user about its own keys. So the registry is the
// single source, and both consumers READ it: App wires from it, ShortcutsDialog
// renders from it. Add a shortcut in one place or it doesn't exist.
//
// It lives in core/ because it is GTK-FREE -- pure data plus string helpers
// (the two-layer seam: core knows nothing of widgets). That's what lets the
// selftest guard it headless: a pure test proves no two chords collide, which is
// a class of bug that otherwise only shows up as a mysteriously dead key.
//
// DOC-ONLY ROWS are half the point. Mouse gestures (drag, Ctrl+scroll) and keys
// a widget handles itself are not GActions and cannot be accelerators -- but a
// reader needs them just as much. A row with an empty `action` documents without
// binding, so the reference stays complete without lying to the accel table.
//
// (Adapted from Sudoku's registry, itself from Folio's s98. Cairn carries the
// shape, not the content -- these bindings are the seed's own.)
#include <string>
#include <vector>

namespace acorn::core {

struct ShortcutSpec {
    std::string              section;      // group heading, e.g. "General" (authored A-Z)
    std::string              action;       // "win.about" -- empty => doc-only (not a GAction)
    std::vector<std::string> accels;       // GTK accel form(s); wired iff action set
    std::string              keys;         // explicit display; empty => derived from accels
    std::string              description;

    std::string display_keys() const;      // `keys`, else accels formatted + joined
};

// "<Ctrl><Shift>n" -> "Ctrl+Shift+N". Modifiers in canonical order; named keys
// map to glyphs where one reads better.
std::string format_accel(const std::string& accel);

// The registry -- authored section (A-Z) -> row, so a consumer walks it linearly
// and starts a heading whenever the section changes.
const std::vector<ShortcutSpec>& shortcut_registry();

// An accel collides when two+ specs claim it and at least one is a wired
// GAction (a GAction accel fires regardless of focus, so anything else on that
// chord is shadowed). Sorted, unique; empty => clean.
std::vector<std::string> find_accel_collisions();

}  // namespace acorn::core
