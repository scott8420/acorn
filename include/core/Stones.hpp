#pragma once
// core::Stones -- the smallest model that exercises the undo stone: an ordered
// list of named stones, each with a size. GTK-free. It is FIXTURE, not
// paradigm: what to copy from it is the shape --
//
//   * the store meets the journal's contract (get / put by id, by value);
//   * the record's PLACE is in the value (`pos`), so putting an old value back
//     puts the stone back where it was -- delete + undo lands in the same slot;
//   * every user verb is a function that goes through Journal::run (or a
//     gesture) -- the ONE DOOR -- and is listed in verbs(), which the
//     selftest walks;
//   * put() counts any write made with the door shut (`strays()`), so "forgot
//     to wrap it" is a failing selftest, not a bug report. Loading a document
//     is the one honest exception and says so (`Loading` scope).
//
// A project seeded from Cairn deletes this file and keeps the shape: its own
// store, its own verbs, the same journal, the same gate.
#include "core/Undo.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acorn::core {

struct Stone {
    std::string name;
    int         size = 3;    // 1..10
    double      pos  = 0;    // the place in the list (sorts ascending)
    bool operator==(const Stone&) const = default;
};

class Stones {
public:
    using Id    = std::string;
    using Value = Stone;

    // ── the journal's contract ──────────────────────────────────────────────
    std::optional<Stone> get(const Id& id) const;
    void put(const Id& id, const std::optional<Stone>& v);

    // ── reading ─────────────────────────────────────────────────────────────
    std::vector<Id> ids() const;                 // in list order
    std::size_t     count() const { return m_items.size(); }
    Id              fresh_id();                  // "s1", "s2", ... never reused
    double          pos_after(const Id& id) const;  // a place just after `id` (end if empty)

    // ── the one-door guard ──────────────────────────────────────────────────
    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    // A load is not a user choice and is not undoable: say so, by scope.
    struct Loading {
        explicit Loading(Stones& s) : m_s(s) { ++m_s.m_loading; }
        ~Loading() { --m_s.m_loading; }
        Stones& m_s;
    };

    // Repaint hook: called after every put (the UI queues a repaint here).
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

private:
    std::map<Id, Stone>    m_items;
    int                    m_next = 1;
    int                    m_strays = 0;
    int                    m_loading = 0;
    std::function<bool()>  m_door;
    std::function<void()>  m_changed;
};

using StoneJournal = Journal<Stones>;

// ── the verbs: every user choice the demo page offers ──────────────────────
// Each goes through the journal. The UI calls these and nothing else writes.
namespace stones {
Stones::Id add(StoneJournal& j, Stones& s, const std::string& name, const Stones::Id& after = {});
bool       remove(StoneJournal& j, Stones& s, const Stones::Id& id);
bool       rename(StoneJournal& j, Stones& s, const Stones::Id& id, const std::string& name);
bool       move_up(StoneJournal& j, Stones& s, const Stones::Id& id);
bool       move_down(StoneJournal& j, Stones& s, const Stones::Id& id);
// Keys / scroll on the size slider: each press is a write; presses inside the
// merge window fold into ONE step (merge key "size:<id>").
bool       resize(StoneJournal& j, Stones& s, const Stones::Id& id, int size);
// Dragging the slider: begin at press, set_size during the drag (no step of its
// own -- the gesture is open), commit at release. ONE step for the whole drag.
void       drag_begin(StoneJournal& j, Stones& s, const Stones::Id& id);
void       drag_to(StoneJournal& j, Stones& s, const Stones::Id& id, int size);
bool       drag_end(StoneJournal& j);
// The neighbour that takes the selection when `id` goes (next, else previous).
Stones::Id neighbour(const Stones& s, const Stones::Id& id);

// The verb table: every verb, by name, runnable on a fixture. The selftest
// walks it (one door, undo restores, redo re-applies). A verb the page offers
// and this table lacks is the gap the gate cannot see -- add it here in the
// same change that adds the button.
struct Verb {
    std::string name;
    std::function<void(StoneJournal&, Stones&, const Stones::Id& first)> run;
};
const std::vector<Verb>& verbs();
}  // namespace stones

}  // namespace acorn::core
