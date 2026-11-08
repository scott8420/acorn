#pragma once
// core/Script -- scripts (s017a). A SCRIPT lives in the SOLUTION, never the
// database: a named list of STEPS (FileMaker's script steps -- steps, not
// code), kept under the table it runs on, as calculations, summaries and value
// lists are.
//
// The steps:
//   Go to Layout “name”            -- the table's layout to show
//   Find  field  what              -- a Find box: "a" contains, "=" empty,
//                                     "=x", "<1970-01-01" ... (s013a's rules)
//   Show All                       -- every record again
//   Sort  field  ascending|descending
//   New Record  field = expression -- one line each (may be none)
//   Set Field  field = expression  -- on the picked record
//   Delete Record                  -- asks first; No stops the script
//   Preview                        -- Browse -> Preview of the picked layout
//   Print...                       -- the print dialog over those pages
//   Show Message “text”            -- waits for OK
// s023a -- steps that decide:
//   If  condition / Else If  condition / Else / End If
//                                  -- the steps between run when the
//                                     condition is true (not empty, 0 or "");
//                                     nest freely; every If has its End If
//   Set Variable  $name = expression -- kept for the run; any expression
//                                     after it reads $name (empty until set)
// An expression is the calculation language (s014a), worked out against the
// picked record: `"Ada"`, `Upper(last_name)`, `Today`, `n + 1`.
// GTK-free, headless-testable.
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace acorn::core {

enum class StepKind { GoToLayout, Find, ShowAll, Sort, NewRecord, SetField, DeleteRecord, Preview, Print, Message,
                      If, ElseIf, Else, EndIf, SetVariable };   // s023a

struct ScriptStep {
    StepKind    kind = StepKind::ShowAll;
    std::string field;    // Find / Sort / Set Field: the field; Go to Layout: the layout's name;
                          // Set Variable: the variable ($name)
    std::string text;     // Find: what; Set Field: the expression; New Record: the lines;
                          // Show Message: the words
    bool        descending = false;   // Sort
    bool operator==(const ScriptStep&) const = default;
};

struct Script {
    std::string table;                 // the table it runs on
    std::string name;                  // unique among the table's scripts
    std::vector<ScriptStep> steps;
    bool operator==(const Script&) const = default;
};

// Every kind, in the order the editor offers them.
const std::vector<StepKind>& step_kinds();
const char* step_name(StepKind k);    // "go_to_layout" ... -- the file's word
const char* step_label(StepKind k);   // "Go to Layout" ... -- the person's
std::optional<StepKind> parse_step_kind(const std::string& s);
// Whether a step writes the database (through the door).
bool step_writes(StepKind k);

// "Set Field “last_name” to Upper(last_name)", "Find “city” has “Mill”",
// "Sort by “birthday”, descending" -- one line for the editor and the menu.
std::string step_says(const ScriptStep& s);

// New Record's lines: "field = expression", one a line; blank lines skipped.
// Refused (in words, with the line's number): no "=", no field before it,
// nothing after it.
std::variant<std::vector<std::pair<std::string, std::string>>, std::string>
parse_assignments(const std::string& text);

// s023a: a variable's name: "" = fine, else why. $ then a letter or _, then
// letters, digits, _ ($count, $last_seen).
std::string check_variable_name(const std::string& name);
// s023a: how deep step i sits inside If blocks (its own If / Else / End If
// count as the outer depth) -- the editor indents by it.
int step_depth(const std::vector<ScriptStep>& steps, std::size_t i);

// A script's name: "" = fine, else why. Any words, one line, no spaces at
// either end, at most 64 characters.
std::string check_script_name(const std::string& name);

// The whole check before a script is kept. `taken`: the table's other
// scripts. `fields`: the table's fields; `layouts`: its layouts' names.
// Refused in words, naming the step: no steps; a field / layout that isn't
// there; a Find with nothing to find; an expression that doesn't parse; New
// Record's lines. "" = fine.
std::string check_script(const Script& s, const std::vector<std::string>& taken,
                         const std::vector<std::string>& fields, const std::vector<std::string>& layouts);

// "3 steps: Find · Sort · Preview" -- how the Tables area says what it does.
std::string script_says(const Script& s);

inline constexpr int kScriptStepsMax = 200;

}  // namespace acorn::core
