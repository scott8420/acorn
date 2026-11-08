#include "db/Script.hpp"

#include "core/Calc.hpp"

#include <algorithm>
#include <variant>

namespace acorn::db {

namespace {
std::string curly(const std::string& s) { return "“" + s + "”"; }
}  // namespace

std::string script_label(const core::Script& s) { return "Script " + curly(s.name); }

ScriptRun::ScriptRun(Journal& j, const core::Solution& sol, core::Script s, ScriptState st)
    : m_j(j), m_sol(sol), m_script(std::move(s)), m_state(std::move(st)), m_top(m_script) {
    m_j.begin_batch(script_label(m_script));
}

ScriptRun::~ScriptRun() {
    if (!m_finished && m_j.in_batch()) m_j.end_batch();
}

const Table* ScriptRun::table() {
    if (!m_schema) {
        auto s = m_j.store().connection().introspect();
        if (!s) return nullptr;
        m_schema = std::move(*s);
    }
    return m_schema->table(m_state.table);
}

ScriptPause ScriptRun::stop(const std::string& why, bool refused) {
    m_finished = true;
    m_words = (m_calls.empty() ? std::string("Step ") : "In " + curly(m_script.name) + " (performed), step ") +
              std::to_string(m_at + 1) + " (" + core::step_label(m_script.steps[m_at].kind) + "): " + why;
    if (refused && m_writes > 0) {
        if (auto r = m_j.cancel_batch(); r) {
            m_put_back = true;
            m_writes = 0;
            m_words += " -- what the script wrote before it was put back";
        } else {
            m_words += " -- and what it wrote couldn't be put back (" + r.error().message + "); Ctrl+Z tries again";
            m_j.end_batch();
        }
    } else {
        if (refused) (void)m_j.cancel_batch();
        else m_j.end_batch();
        if (!refused && m_writes > 0) m_words += " -- what it wrote is kept; Ctrl+Z puts it back";
    }
    if (m_state.record.empty() || !still_there(m_state.record)) m_state.record = first_found();
    return ScriptPause::Stopped;
}

ScriptPause ScriptRun::done() {
    m_finished = true;
    m_at = m_script.steps.size();
    m_j.end_batch();
    m_words.clear();
    return ScriptPause::Done;
}

bool ScriptRun::still_there(const Store::Id& id) {
    return !id.empty() && m_j.store().get(id).has_value();
}

std::string ScriptRun::first_found() {
    const Table* t = table();
    if (!t) return {};
    PageRequest r;
    r.limit = 1;
    r.sort = m_state.sort;
    r.descending = m_state.descending;
    Connection& c = m_j.store().connection();
    auto p = m_state.found.empty() ? read_page(c, *t, r) : read_found(c, *t, m_state.found, r, &*m_schema);
    if (!p || p->ids.empty()) return {};
    return p->ids[0];
}

std::expected<Value, std::string> ScriptRun::work_out(const std::string& e, const Column& col) {
    auto out = value_of(e);
    if (!out) return std::unexpected(out.error());
    const std::string text = core::calc_text(*out);
    if (text.empty()) return Value();   // empty -> the field emptied
    auto v = parse_field(text, col);
    if (!v) return std::unexpected(curly(col.name) + ": " + v.error());
    return *v;
}

std::expected<core::CalcValue, std::string> ScriptRun::value_of(const std::string& e) {
    auto parsed = core::parse_calc(e);
    if (auto* err = std::get_if<std::string>(&parsed)) return std::unexpected(*err);
    const Table* t = table();
    if (!t) return std::unexpected("there's no table " + curly(m_state.table));
    // The picked record's values, by field (none picked: every field empty).
    std::map<std::string, core::CalcValue> vals;
    if (!m_state.record.empty())
        if (auto got = m_j.store().get(m_state.record); got && std::holds_alternative<RowDef>(*got)) {
            const auto& d = std::get<RowDef>(*got);
            for (std::size_t i = 0; i < d.columns.size(); ++i)
                if (const Column* c = t->column(d.columns[i])) vals[d.columns[i]] = calc_value(d.values[i], c->type);
        }
    const std::string today = core::today_iso();
    core::CalcLookup fields = [&](const std::string& n) -> std::optional<core::CalcValue> {
        if (auto it = vals.find(n); it != vals.end()) return it->second;
        if (t->column(n)) return core::CalcValue{};
        return std::nullopt;
    };
    const auto calcs = core::evaluate_all(m_sol.calcs_of(m_state.table), fields, today);
    core::CalcLookup with_calcs = [&](const std::string& n) -> std::optional<core::CalcValue> {
        if (!n.empty() && n[0] == '$') {   // s023a: a variable -- empty until set
            auto it = m_vars.find(n);
            return it == m_vars.end() ? core::CalcValue{} : it->second;
        }
        if (auto v = fields(n)) return v;
        if (auto it = calcs.find(n); it != calcs.end()) {
            return it->second.value;   // a calculation that failed reads as empty
        }
        return std::nullopt;
    };
    const auto out = core::evaluate(std::get<core::CalcExpr>(parsed), with_calcs, today);
    if (!out.ok()) return std::unexpected(out.error);
    return out.value;
}

std::size_t ScriptRun::next_branch(std::size_t i) const {
    using K = core::StepKind;
    int depth = 0;
    for (std::size_t j = i + 1; j < m_script.steps.size(); ++j) {
        const auto k = m_script.steps[j].kind;
        if (k == K::If) ++depth;
        else if (k == K::EndIf) { if (depth == 0) return j; --depth; }
        else if ((k == K::ElseIf || k == K::Else) && depth == 0) return j;
    }
    return m_script.steps.size();
}

std::size_t ScriptRun::end_of(std::size_t i) const {
    std::size_t j = i;
    while (j < m_script.steps.size() && m_script.steps[j].kind != core::StepKind::EndIf) j = next_branch(j);
    return j;
}

std::size_t ScriptRun::loop_end(std::size_t i) const {
    int depth = 0;
    for (std::size_t j = i + 1; j < m_script.steps.size(); ++j) {
        const auto k = m_script.steps[j].kind;
        if (k == core::StepKind::Loop) ++depth;
        else if (k == core::StepKind::EndLoop) { if (depth == 0) return j; --depth; }
    }
    return m_script.steps.size();
}

std::size_t ScriptRun::loop_start(std::size_t e) const {
    int depth = 0;
    for (std::size_t j = e; j-- > 0;) {
        const auto k = m_script.steps[j].kind;
        if (k == core::StepKind::EndLoop) ++depth;
        else if (k == core::StepKind::Loop) { if (depth == 0) return j; --depth; }
    }
    return m_script.steps.size();
}

std::vector<Store::Id> ScriptRun::found_ids() {
    const Table* t = table();
    if (!t) return {};
    PageRequest r;
    r.limit = 100000;
    r.sort = m_state.sort;
    r.descending = m_state.descending;
    Connection& c = m_j.store().connection();
    auto p = m_state.found.empty() ? read_page(c, *t, r) : read_found(c, *t, m_state.found, r, &*m_schema);
    return p ? p->ids : std::vector<Store::Id>{};
}

bool ScriptRun::branch(std::size_t i, ScriptPause& pause) {
    using K = core::StepKind;
    std::size_t j = next_branch(i);
    while (j < m_script.steps.size() && m_script.steps[j].kind == K::ElseIf) {
        m_at = j;   // a refusal names the Else If
        auto c = value_of(m_script.steps[j].text);
        if (!c) { pause = stop(c.error(), false); return false; }
        if (core::calc_truthy(*c)) { m_next = j + 1; return true; }
        j = next_branch(j);
    }
    m_next = j < m_script.steps.size() ? j + 1 : j;   // past the Else / End If
    return true;
}

bool ScriptRun::step(std::size_t i, ScriptPause& pause) {
    const auto& st = m_script.steps[i];
    using K = core::StepKind;
    const Table* t = table();
    if (!t) { pause = stop("there's no table " + curly(m_state.table), false); return false; }
    switch (st.kind) {
    case K::GoToLayout:
        if (!m_sol.layouts.contains(m_state.table) || !m_sol.layouts.at(m_state.table).contains(st.field)) {
            pause = stop(curly(m_state.table) + " has no layout " + curly(st.field), false);
            return false;
        }
        m_state.layout = st.field;
        return true;
    case K::Find: {
        std::vector<Criterion> crit{Criterion{st.field, st.text}};
        PageRequest r;
        r.limit = 1;
        r.sort = m_state.sort;
        r.descending = m_state.descending;
        auto p = read_found(m_j.store().connection(), *t, crit, r, &*m_schema);
        if (!p) { pause = stop(p.error().message, false); return false; }
        if (p->ids.empty()) { pause = stop("no records match " + find_summary(crit), false); return false; }
        m_state.found = std::move(crit);
        if (!m_state.record.empty()) {
            // Still the pick when it's among the found ones.
            PageRequest all;
            all.limit = 100000;
            auto every = read_found(m_j.store().connection(), *t, m_state.found, all, &*m_schema);
            if (!every || std::find(every->ids.begin(), every->ids.end(), m_state.record) == every->ids.end())
                m_state.record = p->ids[0];
        } else {
            m_state.record = p->ids[0];
        }
        return true;
    }
    case K::ShowAll:
        m_state.found.clear();
        if (m_state.record.empty()) m_state.record = first_found();
        return true;
    case K::Sort:
        if (!t->column(st.field)) { pause = stop("there's no field " + curly(st.field), false); return false; }
        m_state.sort = st.field;
        m_state.descending = st.descending;
        return true;
    case K::NewRecord: {
        auto a = core::parse_assignments(st.text);
        if (auto* err = std::get_if<std::string>(&a)) { pause = stop(*err, false); return false; }
        std::vector<std::string> cols;
        std::vector<Value> vals;
        for (const auto& [f, e] : std::get<0>(a)) {
            const Column* c = t->column(f);
            if (!c) { pause = stop("there's no field " + curly(f), true); return false; }
            auto v = work_out(e, *c);
            if (!v) { pause = stop(v.error(), true); return false; }
            cols.push_back(f);
            vals.push_back(std::move(*v));
        }
        auto made = verbs::new_record(m_j, m_state.table, cols, vals);
        if (!made) { pause = stop(made.error().message, true); return false; }
        ++m_writes;
        m_state.record = *made;
        return true;
    }
    case K::SetField: {
        if (m_state.record.empty()) { pause = stop("there's no record to set " + curly(st.field) + " in", false); return false; }
        const Column* c = t->column(st.field);
        if (!c) { pause = stop("there's no field " + curly(st.field), true); return false; }
        auto v = work_out(st.text, *c);
        if (!v) { pause = stop(v.error(), true); return false; }
        auto r = verbs::set_field(m_j, m_state.record, st.field, *v);
        if (!r) { pause = stop(r.error().message, true); return false; }
        ++m_writes;
        return true;
    }
    case K::DeleteRecord:
        if (m_state.record.empty()) { pause = stop("there's no record to delete", false); return false; }
        m_asking = true;
        m_words = "Delete " + Store::describe(m_state.record) + "?";
        pause = ScriptPause::Ask;
        return false;
    case K::Preview:
        m_at = i + 1;
        pause = ScriptPause::Preview;
        return false;
    case K::Print:
        m_at = i + 1;
        pause = ScriptPause::Print;
        return false;
    case K::Message:
        m_at = i + 1;
        m_words = st.text;
        pause = ScriptPause::Message;
        return false;
    case K::If: {   // s023a
        auto c = value_of(st.text);
        if (!c) { pause = stop(c.error(), false); return false; }
        if (core::calc_truthy(*c)) return true;
        return branch(i, pause);
    }
    case K::ElseIf:
    case K::Else:   // reached by running the branch before it: that branch ran, so past the End If
        m_next = std::min(end_of(i) + 1, m_script.steps.size());
        return true;
    case K::EndIf:
        return true;
    case K::SetVariable: {
        auto v = value_of(st.text);
        if (!v) { pause = stop(v.error(), false); return false; }
        m_vars[st.field] = *v;
        return true;
    }
    // s023b
    case K::Loop:
        m_passes[i] = 0;   // entered from above: a fresh count
        return true;
    case K::EndLoop: {
        const std::size_t s = loop_start(i);
        if (s >= m_script.steps.size()) return true;
        if (++m_passes[s] >= core::kLoopMax) {
            pause = stop("the Loop at step " + std::to_string(s + 1) + " went round " + std::to_string(core::kLoopMax) +
                         " times -- stopped there (an Exit Loop If that never comes true?)", false);
            return false;
        }
        m_next = s + 1;
        return true;
    }
    case K::ExitLoopIf: {
        auto c = value_of(st.text);
        if (!c) { pause = stop(c.error(), false); return false; }
        if (core::calc_truthy(*c)) m_next = std::min(loop_end(i) + 1, m_script.steps.size());
        return true;
    }
    case K::GoToRecord: {
        const auto ids = found_ids();
        if (ids.empty()) { pause = stop("there are no records to go to", false); return false; }
        const auto here = std::find(ids.begin(), ids.end(), m_state.record);
        const long n = static_cast<long>(ids.size());
        const long at = here == ids.end() ? -1 : static_cast<long>(here - ids.begin());
        long to = 0;
        if (st.field == "first") to = 0;
        else if (st.field == "last") to = n - 1;
        else if (st.field == "next" || st.field == "previous") {
            to = st.field == "next" ? at + 1 : (at < 0 ? -1 : at - 1);
            if (to < 0 || to >= n) {   // off the end: out of the Loop it's in, else the run stops
                const std::size_t e = loop_end(i);
                if (e < m_script.steps.size()) { m_next = e + 1; return true; }
                pause = stop(std::string("there's no ") + (st.field == "next" ? "next record -- this is the last"
                                                                               : "previous record -- this is the first"), false);
                return false;
            }
        } else {   // number
            auto v = value_of(st.text);
            if (!v) { pause = stop(v.error(), false); return false; }
            const auto* d = std::get_if<double>(&*v);
            if (!d || *d < 1 || *d > static_cast<double>(n) || *d != static_cast<double>(static_cast<long>(*d))) {
                pause = stop("there's no record number " + core::calc_text(*v) + " -- the found set has " + std::to_string(n), false);
                return false;
            }
            to = static_cast<long>(*d) - 1;
        }
        m_state.record = ids[static_cast<std::size_t>(to)];
        return true;
    }
    case K::PerformScript: {
        const core::Script* sub = m_sol.script(m_state.table, st.field);
        if (!sub) { pause = stop(curly(m_state.table) + " has no script " + curly(st.field), false); return false; }
        if (static_cast<int>(m_calls.size()) >= core::kPerformDepth) {
            pause = stop("scripts performed " + std::to_string(core::kPerformDepth) + " deep -- stopped there (a script "
                         "that performs itself without end?)", false);
            return false;
        }
        m_calls.push_back(Frame{m_script, i + 1, std::move(m_vars), std::move(m_passes)});
        m_script = *sub;
        m_vars.clear();     // its own $variables
        m_passes.clear();
        m_next = 0;
        return true;
    }
    }
    return true;
}

ScriptPause ScriptRun::go() {
    if (m_finished) return m_put_back || !m_words.empty() ? ScriptPause::Stopped : ScriptPause::Done;
    if (m_asking) return ScriptPause::Ask;
    for (;;) {
        if (m_at >= m_script.steps.size()) {   // s023b: a performed script ended -- back to the one that performed it
            if (m_calls.empty()) break;
            Frame f = std::move(m_calls.back());
            m_calls.pop_back();
            m_script = std::move(f.script);
            m_at = f.back;
            m_vars = std::move(f.vars);
            m_passes = std::move(f.passes);
            continue;
        }
        ScriptPause p = ScriptPause::Done;
        m_next.reset();
        if (!step(m_at, p)) return p;
        m_at = m_next ? *m_next : m_at + 1;   // s023a: a branch jumps
    }
    return done();
}

void ScriptRun::answer(bool yes) {
    if (!m_asking || m_finished) return;
    m_asking = false;
    if (!yes) { (void)stop("not deleted -- the script stopped there", false); return; }
    auto r = verbs::delete_record(m_j, m_state.record);
    if (!r) { (void)stop(r.error().message, true); return; }
    ++m_writes;
    m_state.record = first_found();
    ++m_at;
}

}  // namespace acorn::db
