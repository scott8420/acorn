#pragma once
// core::Undo -- the UNDO paradigm: every user choice is an undoable event, and
// there is ONE door it goes through. GTK-free and header-only (a template over
// the app's own store), so the selftest drives it headless.
//
// ── why this is a stone, and why it is early ───────────────────────────────
// Folio, Curvz and jot all reached the undo talk late, and each time the cost
// was not the undo system -- it was finding every write path built without it
// (Curvz: group / ungroup, layers, palettes, each a retrofit milestone; jot
// s046: Note details, card ticks, Inbox verbs, captures). Scott, s046: "we
// should integrate a system for undo early in a project and make any user
// choice an undoable event." So it ships with the model, in the first
// milestone that writes anything. docs/undo.md has the whole story.
//
// ── a step is two pictures, not an inverse ─────────────────────────────────
// Curvz wrote one command class per verb, each with its own undo() -- ~65
// classes, and every new verb needed one before it could be undone. Here a step
// records how the records it touched looked BEFORE and AFTER (a record absent
// on one side is a create or a delete). Undo puts BEFORE back, redo puts AFTER
// back. There is no per-verb inverse to write or get wrong: a verb added later
// is undoable the day it goes through run(). (Curvz's own BooleanOpCommand --
// clone the originals and the results, swap them whole -- was this shape, and
// its ARCHITECTURE called it "the template for future structural ops".)
//
// ── records by ID, never by pointer ────────────────────────────────────────
// Curvz commands held raw SceneNode* and needed a scrub pass when a node died.
// A step here holds ids and VALUES (copies), so nothing it holds can dangle.
//
// ── the store contract (all the journal asks of a model) ───────────────────
//     using Id    = ...;   // copyable, ==   (a string, an int)
//     using Value = ...;   // copyable, ==   (the whole record, by value)
//     std::optional<Value> get(const Id&) const;          // nullopt: absent
//     void put(const Id&, const std::optional<Value>&);   // nullopt: remove
// If the record has a place (an order, a parent), the place is IN the Value,
// so putting the old value back puts the record back where it was.
//
// ── what the journal adds beyond do / undo / redo ──────────────────────────
//   * a GESTURE is one step: begin() at press, any writes, commit() at release
//     (Curvz: "one EditPathCommand per leaf path, pushed on drag-end").
//   * MERGING: run(..., merge_key) folds into the last step when the key
//     matches inside merge_window -- arrows on a slider, typing a field
//     (Curvz: inspector edits coalesce per object, 1500 ms).
//   * a no-op is not a step (a refused delete, a value set to itself).
//   * SELECTION travels with the step: undo hands back what was selected
//     before, redo what was selected after (Curvz: preserves_selection()).
//   * open(): true inside run / a gesture / undo / redo. A store that wants the
//     ONE-DOOR guarantee checks it in put() and counts a write made outside --
//     the selftest walks every verb and fails on a stray (the gate).
// ─────────────────────────────────────────────────────────────────────────────
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace acorn::core {

template <class Store>
class Journal {
public:
    using Id    = typename Store::Id;
    using Value = typename Store::Value;
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Id                   id;
        std::optional<Value> before;
        std::optional<Value> after;
    };
    struct Step {
        std::string        label;       // "Delete “Ruby”" -- what the menu says
        std::vector<Entry> entries;     // in the order the ids were given
        std::vector<Id>    sel_before;  // selection when the step began
        std::vector<Id>    sel_after;   // selection when it ended
        std::string        merge_key;   // "" = never merges
        Clock::time_point  at{};        // last time it grew (for merging)
    };
    // What an undo / redo did. `selection` is what the UI should select now.
    struct Result {
        bool            done = false;
        std::string     label;
        std::vector<Id> selection;
    };

    static constexpr std::size_t kMax = 500;   // Curvz's limit
    std::chrono::milliseconds merge_window{1500};

    explicit Journal(Store& s) : m_store(s) {}

    // The UI tells the journal how to read the selection (optional).
    void set_selection_source(std::function<std::vector<Id>()> f) { m_sel = std::move(f); }
    // Called after every change to the journal itself (a step kept, undone,
    // redone, cleared) -- the UI re-labels Undo / Redo and greys them here.
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

    // ── THE ONE DOOR ────────────────────────────────────────────────────────
    // Run `op` as one step. `ids` are the records it will touch; `op` may
    // return the ids it CREATED (std::vector<Id>), or nothing. Returns true if
    // a step was kept (or merged into the last one).
    template <class Op>
    bool run(const std::string& label, const std::vector<Id>& ids, Op&& op,
             const std::string& merge_key = {}, Clock::time_point now = Clock::now()) {
        // Inside a gesture, undo or redo: just write. The gesture's commit (or
        // the step being applied) is the step.
        if (m_depth > 0) { call(op, ids); return false; }

        Step st;
        st.label     = label;
        st.merge_key = merge_key;
        st.at        = now;
        st.sel_before = selection();
        for (const auto& id : ids) st.entries.push_back({id, m_store.get(id), std::nullopt});

        ++m_depth;
        const std::vector<Id> made = call(op, ids);
        --m_depth;

        for (const auto& id : made)
            if (!has(st, id)) st.entries.push_back({id, std::nullopt, std::nullopt});
        for (auto& e : st.entries) e.after = m_store.get(e.id);
        st.sel_after = selection();
        if (unchanged(st)) return false;

        // Merge: same key, same records, inside the window, nothing undone in
        // between. The kept step keeps its BEFORE and takes the new AFTER.
        if (!merge_key.empty() && can_undo() && m_at == m_steps.size()) {
            Step& top = m_steps.back();
            if (top.merge_key == merge_key && now - top.at <= merge_window && same_ids(top, st)) {
                for (auto& e : top.entries) e.after = m_store.get(e.id);
                top.sel_after = st.sel_after;
                top.at        = now;
                top.label     = label;
                if (unchanged(top)) { m_steps.pop_back(); m_at = m_steps.size(); }
                notify();
                return true;
            }
        }
        keep(std::move(st));
        return true;
    }

    // ── a gesture: press .. drag .. release = ONE step ──────────────────────
    void begin(const std::string& label, const std::vector<Id>& ids) {
        if (m_gesture) return;   // a second press inside one gesture: same step
        m_gesture = true;
        ++m_depth;
        m_open.label      = label;
        m_open.merge_key.clear();
        m_open.sel_before = selection();
        m_open.entries.clear();
        for (const auto& id : ids) m_open.entries.push_back({id, m_store.get(id), std::nullopt});
    }
    bool in_gesture() const { return m_gesture; }
    // Release: keep the step if anything changed.
    bool commit(Clock::time_point now = Clock::now()) {
        if (!m_gesture) return false;
        m_gesture = false;
        --m_depth;
        for (auto& e : m_open.entries) e.after = m_store.get(e.id);
        m_open.sel_after = selection();
        m_open.at        = now;
        if (unchanged(m_open)) return false;
        keep(std::move(m_open));
        return true;
    }
    // Escape mid-drag: put everything back, keep nothing.
    void cancel() {
        if (!m_gesture) return;
        m_gesture = false;
        for (auto it = m_open.entries.rbegin(); it != m_open.entries.rend(); ++it)
            m_store.put(it->id, it->before);
        --m_depth;
    }

    // ── undo / redo ─────────────────────────────────────────────────────────
    // Undo walks the step BACKWARDS (a child put back after its parent was, a
    // created record removed before what it was made under); redo forwards.
    Result undo() {
        Result r;
        if (m_gesture) commit();   // Ctrl+Z mid-drag: the drag is a step first
        if (!can_undo()) return r;
        const Step& st = m_steps[--m_at];
        ++m_depth;
        for (auto it = st.entries.rbegin(); it != st.entries.rend(); ++it)
            m_store.put(it->id, it->before);
        --m_depth;
        r = {true, st.label, pick(st.sel_before, st, true)};
        notify();
        return r;
    }
    Result redo() {
        Result r;
        if (m_gesture || !can_redo()) return r;
        const Step& st = m_steps[m_at++];
        ++m_depth;
        for (const auto& e : st.entries) m_store.put(e.id, e.after);
        --m_depth;
        r = {true, st.label, pick(st.sel_after, st, false)};
        notify();
        return r;
    }

    // The UI moved the selection AFTER the verb (a delete selects the
    // neighbour): amend what redo of the last step will hand back.
    void reselect(const std::vector<Id>& sel) {
        if (can_undo() && m_at == m_steps.size()) m_steps.back().sel_after = sel;
    }

    bool        can_undo()   const { return m_at > 0; }
    bool        can_redo()   const { return m_at < m_steps.size(); }
    std::string undo_label() const { return can_undo() ? m_steps[m_at - 1].label : std::string{}; }
    std::string redo_label() const { return can_redo() ? m_steps[m_at].label : std::string{}; }
    std::size_t size()       const { return m_steps.size(); }

    // A different document / folder: no step carries over.
    void clear() { m_steps.clear(); m_at = 0; notify(); }

    // True while a write is coming through the door (run, a gesture, undo,
    // redo, cancel). A store's put() asks this to enforce the one-door rule.
    bool open() const { return m_depth > 0; }

private:
    Store&                            m_store;
    std::vector<Step>                 m_steps;
    std::size_t                       m_at = 0;      // [0, m_at) done; the rest redoable
    int                               m_depth = 0;   // >0: inside the door
    bool                              m_gesture = false;
    Step                              m_open;        // the gesture being recorded
    std::function<std::vector<Id>()>  m_sel;
    std::function<void()>             m_changed;

    template <class Op>
    std::vector<Id> call(Op& op, const std::vector<Id>&) {
        if constexpr (std::is_void_v<std::invoke_result_t<Op&>>) { op(); return {}; }
        else return op();
    }
    std::vector<Id> selection() const { return m_sel ? m_sel() : std::vector<Id>{}; }
    static bool has(const Step& st, const Id& id) {
        for (const auto& e : st.entries) if (e.id == id) return true;
        return false;
    }
    static bool unchanged(const Step& st) {
        for (const auto& e : st.entries) if (!(e.before == e.after)) return false;
        return true;
    }
    static bool same_ids(const Step& a, const Step& b) {
        if (a.entries.size() != b.entries.size()) return false;
        for (std::size_t i = 0; i < a.entries.size(); ++i)
            if (!(a.entries[i].id == b.entries[i].id)) return false;
        return true;
    }
    // The selection to hand back: the recorded one, minus what is gone now;
    // if that leaves nothing, the first record the step touched that exists.
    std::vector<Id> pick(const std::vector<Id>& want, const Step& st, bool undoing) const {
        std::vector<Id> out;
        for (const auto& id : want) if (m_store.get(id)) out.push_back(id);
        if (!out.empty()) return out;
        for (const auto& e : st.entries)
            if ((undoing ? e.before : e.after) && m_store.get(e.id)) return {e.id};
        return {};
    }
    void keep(Step&& st) {
        m_steps.resize(m_at);                 // a new step forgets what was undone
        m_steps.push_back(std::move(st));
        if (m_steps.size() > kMax) m_steps.erase(m_steps.begin());
        m_at = m_steps.size();
        notify();
    }
    void notify() { if (m_changed) m_changed(); }
};

}  // namespace acorn::core
