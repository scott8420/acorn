#pragma once
// db/Script -- running a script (s017a). GTK-free: the Shell drives it and
// does what needs the person (a question, a message) or the screen (Preview,
// Print...); the selftest drives it the same way on every engine.
//
// A run walks the steps in order on a STATE -- the table, the picked layout,
// the found set's criteria, the sort, the picked record -- the same things
// Browse shows, which the Shell copies in before and back out after each
// pause.
//
// Every WRITING step (New Record, Set Field, Delete Record) goes through the
// door -- verbs::new_record / set_field / delete_record -- inside ONE journal
// batch, so a whole run is ONE step on Ctrl+Z ("Script “tidy up”").
//
// A run stops:
//   * at the end -- Done;
//   * when a write is REFUSED -- everything the run wrote is put back
//     (cancel_batch) and nothing is kept: the database is never left
//     half-scripted;
//   * when a step can't go on (a Find that finds nothing, No to Delete
//     Record, a step with no record to work on) -- what the run wrote so far
//     is kept, as the one step; Ctrl+Z puts it back.
#include "core/Script.hpp"
#include "core/Solution.hpp"
#include "db/Browse.hpp"
#include "db/Journal.hpp"

#include <string>
#include <vector>

namespace acorn::db {

struct ScriptState {
    std::string            table;
    std::string            layout;       // the picked layout's name ("" = the default)
    std::vector<Criterion> found;        // the found set's criteria (none: every record)
    std::string            sort;         // "" = the key order
    bool                   descending = false;
    Store::Id              record;       // the picked record ("" = none)
    bool operator==(const ScriptState&) const = default;
};

enum class ScriptPause {
    Done,      // every step ran
    Stopped,   // words() says why; wrote_kept() whether its writes were kept
    Ask,       // words() is the question -- answer(yes / no), then go() again
    Message,   // words() to show; go() again after OK
    Preview,   // show Preview of the state's layout; go() again
    Print,     // the print dialog over those pages; go() again
};

class ScriptRun {
public:
    ScriptRun(Journal& j, const core::Solution& sol, core::Script s, ScriptState st);
    ~ScriptRun();   // a run dropped mid-way keeps what it wrote (end_batch)
    ScriptRun(const ScriptRun&) = delete;
    ScriptRun& operator=(const ScriptRun&) = delete;

    // Run on until a pause (or the end).
    ScriptPause go();
    // After Ask: yes does the step (Delete Record), no stops the run.
    void answer(bool yes);

    const ScriptState&  state() const { return m_state; }
    const std::string&  words() const { return m_words; }
    // The step the run paused / stopped at (0-based), or steps().size() when done.
    std::size_t         at() const { return m_at; }
    const core::Script& script() const { return m_script; }
    bool finished() const { return m_finished; }
    // Writes made (and kept) by the run -- a step on Ctrl+Z when > 0.
    int  writes() const { return m_writes; }
    bool put_back() const { return m_put_back; }   // a refused write put every write back

private:
    ScriptPause stop(const std::string& why, bool refused);
    ScriptPause done();
    bool        step(std::size_t i, ScriptPause& pause);   // false: paused / stopped (pause says which)
    const Table* table();
    // The expression `e` against the picked record, as the field `col` would
    // hold it. "" error = fine.
    std::expected<Value, std::string> work_out(const std::string& e, const Column& col);
    std::string first_found();   // the first record of the found set in the state's order ("" none)
    bool        still_there(const Store::Id& id);

    Journal&             m_j;
    const core::Solution& m_sol;
    core::Script         m_script;
    ScriptState          m_state;
    std::optional<Schema> m_schema;
    std::size_t          m_at = 0;
    std::string          m_words;
    bool                 m_asking = false;
    bool                 m_finished = false;
    bool                 m_put_back = false;
    int                  m_writes = 0;
};

// The label a run's step has on Ctrl+Z: Script “name”.
std::string script_label(const core::Script& s);

}  // namespace acorn::db
