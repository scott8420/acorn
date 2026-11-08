#pragma once
// core/Calc -- calculation fields (s014a). A calculation lives in the SOLUTION,
// never the database: a named expression over one table's fields that Acorn
// works out for each record as it is shown (Cards, Grid, Form, List). It is
// never written anywhere, so it works on a read-only connection too.
//
// The language (small, FileMaker-flavoured):
//   numbers        12   3.5
//   text           "Ms "   (\" and \\ inside)
//   fields         first_name   -- a field of the table, or another of its
//                  calculations (no loops). Names are letters, digits, _ .
//   + - * /        arithmetic; an EMPTY operand makes the result empty;
//                  text that reads as a number counts as one
//   &              joins as text (empty joins as nothing)
//   = <> < > <= >= compare: numbers as numbers, anything else as text;
//                  the answer is 1 or 0
//   and or not     (any case) -- 1 or 0
//   Length(t) Upper(t) Lower(t) Left(t, n) Right(t, n) Trim(t)
//   If(test, then, else)  -- else may be left out (empty)
//   Round(x)  Round(x, places)
//   Today()        "YYYY-MM-DD" (dates are ISO text, so they compare)
// Function names in any case. Truth: empty is false; a number is true unless
// 0; text is true unless "". Upper / Lower change ASCII letters only.
// A record's calculation that can't be worked out (a word where a number
// belongs, / 0) shows "?" and says why in words.
//
// GTK-free, headless-testable; the selftest walks the language.
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace acorn::core {

struct Calc {
    std::string table;   // the table it belongs to
    std::string name;    // unique among the table's fields and calculations
    std::string expr;    // as typed
    bool operator==(const Calc&) const = default;
};

// s014b: a SUMMARY -- one value over many records (the found set, or every
// record): Count / Total / Average / Minimum / Maximum of one of the table's
// own fields. Kept in the solution beside the calculations; worked out by
// the DATABASE (one aggregate SELECT), shown the same on every record, and a
// calculation may read it like a field ("share = balance / total_balance").
enum class SummaryOp { Count, Total, Average, Min, Max };
const char* summary_op_name(SummaryOp op);    // "count" "total" "average" "min" "max" -- the file's word
const char* summary_op_label(SummaryOp op);   // "Count" "Total" "Average" "Minimum" "Maximum"
std::optional<SummaryOp> parse_summary_op(const std::string& s);
struct Summary {
    std::string table;
    std::string name;
    SummaryOp   op = SummaryOp::Count;
    std::string field;   // a field of the table (not a calculation: the database works it out)
    bool operator==(const Summary&) const = default;
};
// "Total of balance"
std::string summary_says(const Summary& s);

// A worked-out value: empty, a number, or text.
using CalcValue = std::variant<std::monostate, double, std::string>;
// The value, or why it couldn't be worked out (in words).
struct CalcOutcome {
    CalcValue   value;
    std::string error;   // "" = fine
    bool ok() const { return error.empty(); }
};

struct CalcNode;   // the parsed expression (Calc.cpp)
struct CalcExpr {
    std::shared_ptr<const CalcNode> root;
    std::vector<std::string>        names;   // the fields / calculations it reads, each once, in order
};

// Parse. Refused -> why in words ("a closing ) is missing after “Upper(x”").
std::variant<CalcExpr, std::string> parse_calc(const std::string& src);

// A calculation's name: "" = fine, else why. Letters, digits, _ (and any
// non-ASCII letter); not starting with a digit; not a word of the language
// (and, or, not, a function's name).
std::string check_calc_name(const std::string& name);

// The whole check before a calculation is kept: its name, its expression,
// every name it reads is one of `fields` or another of `calcs` (the table's
// calculations, by name -- `c` itself may be among them, as when changing
// it), the name isn't a field's, and no calculation ends up reading itself.
// "" = fine.
std::string check_calc(const Calc& c, const std::vector<std::string>& fields,
                       const std::map<std::string, Calc>& calcs);

// s014b: a summary's check: its name (as a calculation's -- it is read by
// name), not a field's / calculation's / another summary's name, and `field`
// one of the table's `fields`. Whether the field can be totalled is the
// database's to say (db::verbs::save_summary). "" = fine.
std::string check_summary(const Summary& s, const std::vector<std::string>& fields,
                          const std::vector<std::string>& taken);

// One record's value of a field, by name -- nullopt: no such field.
using CalcLookup = std::function<std::optional<CalcValue>(const std::string&)>;

// Work out one parsed expression. `today` is "YYYY-MM-DD".
CalcOutcome evaluate(const CalcExpr& e, const CalcLookup& field, const std::string& today);

// Every calculation of a table for one record, by name: calculations that
// read others see their values (worked out once); a loop or an expression
// that doesn't parse is an outcome with its words.
std::map<std::string, CalcOutcome> evaluate_all(const std::map<std::string, Calc>& calcs,
                                                const CalcLookup& field, const std::string& today);

// How a value is shown: empty -> "", a whole number without ".0", others to
// 10 significant digits; an outcome that failed -> "?".
std::string calc_text(const CalcValue& v);
std::string calc_text(const CalcOutcome& o);

// s023a: true unless empty, 0 or "" -- what a script's If takes.
bool calc_truthy(const CalcValue& v);

// Today's date, local, "YYYY-MM-DD".
std::string today_iso();

}  // namespace acorn::core
