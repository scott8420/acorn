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
    : m_j(j), m_sol(sol), m_script(std::move(s)), m_state(std::move(st)) {
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
    m_words = "Step " + std::to_string(m_at + 1) + " (" + core::step_label(m_script.steps[m_at].kind) + "): " + why;
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
        if (auto v = fields(n)) return v;
        if (auto it = calcs.find(n); it != calcs.end()) {
            return it->second.value;   // a calculation that failed reads as empty
        }
        return std::nullopt;
    };
    const auto out = core::evaluate(std::get<core::CalcExpr>(parsed), with_calcs, today);
    if (!out.ok()) return std::unexpected(out.error);
    const std::string text = core::calc_text(out);
    if (text.empty()) return Value();   // empty -> the field emptied
    auto v = parse_field(text, col);
    if (!v) return std::unexpected(curly(col.name) + ": " + v.error());
    return *v;
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
    }
    return true;
}

ScriptPause ScriptRun::go() {
    if (m_finished) return m_put_back || !m_words.empty() ? ScriptPause::Stopped : ScriptPause::Done;
    if (m_asking) return ScriptPause::Ask;
    while (m_at < m_script.steps.size()) {
        ScriptPause p = ScriptPause::Done;
        if (!step(m_at, p)) return p;
        ++m_at;
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
