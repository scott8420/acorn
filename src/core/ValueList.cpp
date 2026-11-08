#include "core/ValueList.hpp"

#include <algorithm>

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
}  // namespace

std::vector<std::string> parse_list_values(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto nl = text.find('\n', at);
        const std::string line = trim(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (!line.empty() && !has(out, line)) out.push_back(line);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}

std::string list_values_text(const std::vector<std::string>& values) {
    std::string s;
    for (const auto& v : values) s += (s.empty() ? "" : "\n") + v;
    return s;
}

std::string check_value_list_name(const std::string& name) {
    if (name.empty()) return "A value list needs a name";
    if (name.find('\n') != std::string::npos || name.find('\r') != std::string::npos)
        return "A value list's name is one line";
    if (trim(name) != name) return "A value list's name can't start or end with a space";
    std::size_t cps = 0;
    for (unsigned char ch : name) if ((ch & 0xC0) != 0x80) ++cps;
    if (cps > 64) return "A value list's name is at most 64 letters";
    return {};
}

std::string check_value_list(const ValueList& v, const std::vector<std::string>& taken,
                             const std::vector<std::string>* from_fields) {
    if (auto why = check_value_list_name(v.name); !why.empty()) return why;
    if (has(taken, v.name)) return curly(v.table) + " already has a value list " + curly(v.name);
    if (v.typed()) {
        if (v.values.empty()) return "Type at least one value";
        return {};
    }
    if (v.from.empty()) return "Pick the table it reads";
    if (!from_fields) return "there's no table " + curly(v.from);
    if (!has(*from_fields, v.field)) return curly(v.from) + " has no field " + curly(v.field);
    if (!v.shows.empty()) {
        if (v.shows == v.field) return "It shows " + curly(v.field) + " already -- pick another field to show, or none";
        if (!has(*from_fields, v.shows)) return curly(v.from) + " has no field " + curly(v.shows);
    }
    return {};
}

std::string value_list_says(const ValueList& v) {
    const std::string only = v.strict ? " -- only these" : "";   // s025a
    if (!v.typed()) {
        if (v.shows.empty()) return "every " + curly(v.field) + " of " + curly(v.from) + only;
        return curly(v.field) + " of " + curly(v.from) + ", showing " + curly(v.shows) + only;
    }
    constexpr std::size_t kShown = 4;
    std::string s;
    for (std::size_t i = 0; i < v.values.size() && i < kShown; ++i) s += (i ? " · " : "") + v.values[i];
    if (v.values.size() > kShown) s += "  + " + std::to_string(v.values.size() - kShown) + " more";
    return s + only;
}

std::vector<ListItem> typed_items(const ValueList& v) {
    std::vector<ListItem> out;
    for (const auto& x : v.values) out.push_back({x, x});
    return out;
}

}  // namespace acorn::core
