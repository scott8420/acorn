#include "core/Script.hpp"

#include "core/Calc.hpp"

#include <algorithm>
#include <cctype>
#include <variant>

namespace acorn::core {

namespace {
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}
std::string curly(const std::string& s) { return "“" + s + "”"; }
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

struct KindRow { StepKind k; const char* name; const char* label; bool writes; };
const KindRow kKinds[] = {
    {StepKind::GoToLayout,   "go_to_layout",  "Go to Layout",  false},
    {StepKind::Find,         "find",          "Find",          false},
    {StepKind::ShowAll,      "show_all",      "Show All",      false},
    {StepKind::Sort,         "sort",          "Sort",          false},
    {StepKind::NewRecord,    "new_record",    "New Record",    true},
    {StepKind::SetField,     "set_field",     "Set Field",     true},
    {StepKind::DeleteRecord, "delete_record", "Delete Record", true},
    {StepKind::Preview,      "preview",       "Preview",       false},
    {StepKind::Print,        "print",         "Print...",      false},
    {StepKind::Message,      "show_message",  "Show Message",  false},
    {StepKind::If,           "if",            "If",            false},   // s023a
    {StepKind::ElseIf,       "else_if",       "Else If",       false},
    {StepKind::Else,         "else",          "Else",          false},
    {StepKind::EndIf,        "end_if",        "End If",        false},
    {StepKind::SetVariable,  "set_variable",  "Set Variable",  false},
    {StepKind::Loop,         "loop",          "Loop",          false},   // s023b
    {StepKind::ExitLoopIf,   "exit_loop_if",  "Exit Loop If",  false},
    {StepKind::EndLoop,      "end_loop",      "End Loop",      false},
    {StepKind::GoToRecord,   "go_to_record",  "Go to Record",  false},
    {StepKind::PerformScript, "perform_script", "Perform Script", false},
};
const KindRow& row_of(StepKind k) {
    for (const auto& r : kKinds) if (r.k == k) return r;
    return kKinds[2];
}
// The first line of some text, cut to `n` letters (bytes; never mid-UTF-8).
std::string first_line(const std::string& s, std::size_t n = 40) {
    std::string l = s.substr(0, s.find('\n'));
    if (l.size() <= n) return l + (l.size() < s.size() ? " ..." : "");
    std::size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(l[cut]) & 0xC0) == 0x80) --cut;
    return l.substr(0, cut) + "...";
}
}  // namespace

const std::vector<StepKind>& step_kinds() {
    static const std::vector<StepKind> all = [] {
        std::vector<StepKind> v;
        for (const auto& r : kKinds) v.push_back(r.k);
        return v;
    }();
    return all;
}
const char* step_name(StepKind k) { return row_of(k).name; }
const char* step_label(StepKind k) { return row_of(k).label; }
bool step_writes(StepKind k) { return row_of(k).writes; }
std::optional<StepKind> parse_step_kind(const std::string& s) {
    for (const auto& r : kKinds) if (s == r.name) return r.k;
    return std::nullopt;
}

std::string step_says(const ScriptStep& s) {
    switch (s.kind) {
    case StepKind::GoToLayout: return "Go to Layout " + curly(s.field);
    case StepKind::Find:       return "Find " + curly(s.field) + " " + (s.text == "=" ? "empty" : "has " + curly(s.text));
    case StepKind::ShowAll:    return "Show All";
    case StepKind::Sort:       return "Sort by " + curly(s.field) + (s.descending ? ", descending" : "");
    case StepKind::NewRecord: {
        auto a = parse_assignments(s.text);
        if (auto* v = std::get_if<0>(&a); v && !v->empty()) {
            std::string f;
            for (const auto& [k, e] : *v) f += (f.empty() ? "" : ", ") + curly(k);
            return "New Record with " + f;
        }
        return "New Record";
    }
    case StepKind::SetField:     return "Set Field " + curly(s.field) + " to " + first_line(s.text);
    case StepKind::DeleteRecord: return "Delete Record (asks first)";
    case StepKind::Preview:      return "Preview";
    case StepKind::Print:        return "Print...";
    case StepKind::Message:      return "Show Message " + curly(first_line(s.text));
    case StepKind::If:           return "If " + first_line(s.text);
    case StepKind::ElseIf:       return "Else If " + first_line(s.text);
    case StepKind::Else:         return "Else";
    case StepKind::EndIf:        return "End If";
    case StepKind::SetVariable:  return "Set Variable " + s.field + " to " + first_line(s.text);
    case StepKind::Loop:         return "Loop";
    case StepKind::ExitLoopIf:   return "Exit Loop If " + first_line(s.text);
    case StepKind::EndLoop:      return "End Loop";
    case StepKind::GoToRecord:   return "Go to Record " + s.field + (s.field == "number" ? " " + first_line(s.text) : "");
    case StepKind::PerformScript: return "Perform Script " + curly(s.field);
    }
    return "?";
}

std::variant<std::vector<std::pair<std::string, std::string>>, std::string>
parse_assignments(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t at = 0;
    int line_no = 0;
    while (at <= text.size()) {
        const auto nl = text.find('\n', at);
        const std::string line = trim(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        ++line_no;
        if (!line.empty()) {
            const std::string where = "line " + std::to_string(line_no);
            const auto eq = line.find('=');
            if (eq == std::string::npos) return where + " needs “field = what”";
            const std::string f = trim(line.substr(0, eq));
            const std::string e = trim(line.substr(eq + 1));
            if (f.empty()) return where + " has no field before the =";
            if (e.empty()) return where + " has nothing after the = (\"\" makes it empty)";
            out.push_back({f, e});
        }
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}

std::string check_variable_name(const std::string& name) {
    if (name.size() < 2 || name[0] != '$') return "A variable's name starts with $ -- $count";
    for (std::size_t i = 1; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const bool ok = std::isalpha(c) || c == '_' || c >= 0x80 || (i > 1 && std::isdigit(c));
        if (!ok) return "A variable's name is $ then letters, digits and _ -- " + curly(name) + " isn't";
    }
    if (name.size() > 64) return "A variable's name is at most 64 letters";
    return {};
}

const std::vector<std::string>& go_to_record_whats() {
    static const std::vector<std::string> w{"first", "next", "previous", "last", "number"};
    return w;
}

int step_depth(const std::vector<ScriptStep>& steps, std::size_t i) {
    int d = 0;
    for (std::size_t j = 0; j < i && j < steps.size(); ++j) {
        const auto k = steps[j].kind;
        if (k == StepKind::If || k == StepKind::Loop) ++d;   // s023b: a Loop indents too
        else if ((k == StepKind::EndIf || k == StepKind::EndLoop) && d > 0) --d;
    }
    if (i < steps.size()) {
        const auto k = steps[i].kind;
        if ((k == StepKind::EndIf || k == StepKind::Else || k == StepKind::ElseIf || k == StepKind::EndLoop) && d > 0) --d;
    }
    return d;
}

std::string check_script_name(const std::string& name) {
    if (name.empty()) return "A script needs a name";
    if (name.find('\n') != std::string::npos || name.find('\r') != std::string::npos) return "A script's name is one line";
    if (trim(name) != name) return "A script's name has no spaces at either end";
    if (name.size() > 64) return "A script's name is at most 64 letters";
    return {};
}

std::string check_script(const Script& s, const std::vector<std::string>& taken,
                         const std::vector<std::string>& fields, const std::vector<std::string>& layouts) {
    if (auto why = check_script_name(s.name); !why.empty()) return why;
    if (has(taken, s.name)) return curly(s.table) + " already has a script named " + curly(s.name);
    if (s.steps.empty()) return "A script needs a step";
    if (s.steps.size() > static_cast<std::size_t>(kScriptStepsMax))
        return "A script has at most " + std::to_string(kScriptStepsMax) + " steps";
    struct Open { std::size_t at; bool seen_else; bool loop = false; };
    std::vector<Open> ifs;   // s023a: the Ifs (s023b: and Loops) still open, innermost last
    for (std::size_t i = 0; i < s.steps.size(); ++i) {
        const auto& st = s.steps[i];
        const std::string at = "Step " + std::to_string(i + 1) + " (" + step_label(st.kind) + ")";
        auto field_ok = [&]() -> std::string {
            if (st.field.empty()) return at + " needs a field";
            if (!has(fields, st.field)) return at + ": there's no field " + curly(st.field) + " in " + curly(s.table);
            return {};
        };
        auto expr_ok = [&](const std::string& e) -> std::string {
            auto p = parse_calc(e);
            if (auto* err = std::get_if<std::string>(&p)) return at + ": " + *err;
            return {};
        };
        switch (st.kind) {
        case StepKind::GoToLayout:
            if (st.field.empty()) return at + " needs a layout";
            if (!has(layouts, st.field)) return at + ": " + curly(s.table) + " has no layout " + curly(st.field);
            break;
        case StepKind::Find:
            if (auto w = field_ok(); !w.empty()) return w;
            if (trim(st.text).empty()) return at + " needs something to find (= finds the empty ones)";
            break;
        case StepKind::Sort:
            if (auto w = field_ok(); !w.empty()) return w;
            break;
        case StepKind::SetField:
            if (auto w = field_ok(); !w.empty()) return w;
            if (trim(st.text).empty()) return at + " needs what to set it to (\"\" makes it empty)";
            if (auto w = expr_ok(st.text); !w.empty()) return w;
            break;
        case StepKind::NewRecord: {
            auto a = parse_assignments(st.text);
            if (auto* err = std::get_if<std::string>(&a)) return at + ": " + *err;
            for (const auto& [f, e] : std::get<0>(a)) {
                if (!has(fields, f)) return at + ": there's no field " + curly(f) + " in " + curly(s.table);
                if (auto w = expr_ok(e); !w.empty()) return w;
            }
            break;
        }
        case StepKind::Message:
            if (trim(st.text).empty()) return at + " needs words to show";
            break;
        case StepKind::ShowAll: case StepKind::DeleteRecord: case StepKind::Preview: case StepKind::Print:
            break;
        case StepKind::If:
        case StepKind::ElseIf:
            if (st.kind == StepKind::ElseIf) {
                if (ifs.empty() || ifs.back().loop) return at + " has no If before it";
                if (ifs.back().seen_else) return at + " comes after the Else -- it would never be asked";
            }
            if (trim(st.text).empty()) return at + " needs a condition -- last_name = \"Thorne\", $n > 3";
            if (auto w = expr_ok(st.text); !w.empty()) return w;
            if (st.kind == StepKind::If) ifs.push_back({i, false});
            break;
        case StepKind::Else:
            if (ifs.empty() || ifs.back().loop) return at + " has no If before it";
            if (ifs.back().seen_else) return at + ": its If already has an Else";
            ifs.back().seen_else = true;
            break;
        case StepKind::EndIf:
            if (ifs.empty()) return at + " has no If to end";
            if (ifs.back().loop) return at + " comes before the End Loop of step " + std::to_string(ifs.back().at + 1);
            ifs.pop_back();
            break;
        case StepKind::Loop:   // s023b
            ifs.push_back({i, false, true});
            break;
        case StepKind::EndLoop:
            if (ifs.empty()) return at + " has no Loop to end";
            if (!ifs.back().loop) return at + " comes before the End If of step " + std::to_string(ifs.back().at + 1);
            ifs.pop_back();
            break;
        case StepKind::ExitLoopIf: {
            const bool in_loop = std::any_of(ifs.begin(), ifs.end(), [](const Open& o) { return o.loop; });
            if (!in_loop) return at + " isn't inside a Loop";
            if (trim(st.text).empty()) return at + " needs a condition -- $n > 10";
            if (auto w = expr_ok(st.text); !w.empty()) return w;
            break;
        }
        case StepKind::GoToRecord:
            if (!has(go_to_record_whats(), st.field)) return at + " needs first, next, previous, last or number";
            if (st.field == "number") {
                if (trim(st.text).empty()) return at + " needs which number -- 1, $n";
                if (auto w = expr_ok(st.text); !w.empty()) return w;
            }
            break;
        case StepKind::PerformScript:
            if (st.field.empty()) return at + " needs a script";
            if (st.field != s.name && !has(taken, st.field))
                return at + ": " + curly(s.table) + " has no script " + curly(st.field);
            break;
        case StepKind::SetVariable:
            if (auto w = check_variable_name(st.field); !w.empty()) return at + ": " + w;
            if (trim(st.text).empty()) return at + " needs what to set it to (\"\" makes it empty)";
            if (auto w = expr_ok(st.text); !w.empty()) return w;
            break;
        }
    }
    if (!ifs.empty())
        return "Step " + std::to_string(ifs.back().at + 1) + (ifs.back().loop ? " (Loop) has no End Loop" : " (If) has no End If");
    return {};
}

std::string script_says(const Script& s) {
    std::string out = std::to_string(s.steps.size()) + (s.steps.size() == 1 ? " step: " : " steps: ");
    for (std::size_t i = 0; i < s.steps.size(); ++i) {
        if (i == 5) { out += " · +" + std::to_string(s.steps.size() - 5) + " more"; break; }
        out += (i ? " · " : "") + std::string(step_label(s.steps[i].kind));
    }
    return out;
}

}  // namespace acorn::core
