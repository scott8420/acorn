#pragma once
// db/Print -- what one record looks like on paper (s015a): every object of a
// layout resolved to the words it prints -- a field's value as Browse shows
// it, a calculation worked out, a summary's value, a related field read
// through its link, a portal's rows (as many as its box holds). Lines and
// boxes print as drawn; nothing here draws. GTK-free (the selftest reads it
// against the fixture); the Shell lays it on pages with Cairo.
#include "core/Calc.hpp"
#include "core/Layout.hpp"
#include "db/Browse.hpp"

#include <map>
#include <string>
#include <vector>

namespace acorn::db {

struct PrintedObject {
    std::string text;                               // Field / Text / Related / a calc or summary
    std::vector<std::vector<std::string>> rows;     // Portal: its records' columns, in order
    bool missing = false;                           // a field or link that's gone: prints a dashed box
};
// One per layout object, in the layout's order.
using PrintedRecord = std::vector<PrintedObject>;

PrintedRecord print_record(Connection& c, const Schema& s, const Table& t, const core::Layout& l, const Row& row,
                           const std::map<std::string, core::Calc>& calcs,
                           const std::map<std::string, core::CalcValue>& sums, const std::string& today);

// The most records Preview lays out (the rest say so).
inline constexpr int kPrintMax = 2000;

}  // namespace acorn::db
