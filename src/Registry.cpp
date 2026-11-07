#include "Registry.hpp"
#include "Log.hpp"

#include <gtkmm/widget.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace acorn::registry {

namespace {
// name -> widget. A vector-of-pairs would do at this size, but the map keeps
// find() O(1) and mirrors what a bigger project reaches for.
std::unordered_map<std::string, Gtk::Widget*>& table() {
    static std::unordered_map<std::string, Gtk::Widget*> t;
    return t;
}
}  // namespace

void add(std::string_view name, Gtk::Widget* w) {
    auto& t = table();
    const std::string key(name);
    if (auto it = t.find(key); it != t.end() && it->second != w) {
        // Duplicate live name: the s191 self-rebuild-collision tripwire. Warn,
        // don't crash -- a seed should survive its own teaching mistakes.
        if (auto lg = log::get(log::Area::Registry))
            lg->warn("duplicate widget name '{}' -- overwriting the older entry", key);
    }
    t[key] = w;
    if (auto lg = log::get(log::Area::Registry))
        lg->trace("+ {} ({} live)", key, t.size());
}

void remove(Gtk::Widget* w) {
    auto& t = table();
    for (auto it = t.begin(); it != t.end(); ++it) {
        if (it->second == w) { t.erase(it); return; }
    }
}

Gtk::Widget* find(std::string_view name) {
    auto& t = table();
    auto it = t.find(std::string(name));
    return it == t.end() ? nullptr : it->second;
}

void dump() {
    auto& t = table();
    std::vector<std::string> names;
    names.reserve(t.size());
    for (const auto& [k, _] : t) names.push_back(k);
    // Sorted so the dump reads the same run to run.
    std::sort(names.begin(), names.end());
    auto lg = log::get(log::Area::Registry);
    if (lg) lg->info("registry: {} live widget(s)", names.size());
    for (const auto& n : names)
        if (lg) lg->info("  {}", n);
}

std::size_t size() { return table().size(); }

}  // namespace acorn::registry
