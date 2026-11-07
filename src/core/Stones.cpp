#include "core/Stones.hpp"

#include <algorithm>

namespace acorn::core {

std::optional<Stone> Stones::get(const Id& id) const {
    const auto it = m_items.find(id);
    if (it == m_items.end()) return std::nullopt;
    return it->second;
}

void Stones::put(const Id& id, const std::optional<Stone>& v) {
    // The gate: a write with the door shut, outside a load, is a stray.
    if (m_loading == 0 && m_door && !m_door()) ++m_strays;
    if (v) m_items[id] = *v;
    else   m_items.erase(id);
    if (m_changed) m_changed();
}

std::vector<Stones::Id> Stones::ids() const {
    std::vector<Id> out;
    for (const auto& [id, s] : m_items) out.push_back(id);
    std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
        return m_items.at(a).pos < m_items.at(b).pos;
    });
    return out;
}

Stones::Id Stones::fresh_id() { return "s" + std::to_string(m_next++); }

double Stones::pos_after(const Id& id) const {
    const auto order = ids();
    if (order.empty()) return 1.0;
    const auto it = std::find(order.begin(), order.end(), id);
    if (it == order.end()) return m_items.at(order.back()).pos + 1.0;   // append
    const double here = m_items.at(*it).pos;
    if (it + 1 == order.end()) return here + 1.0;
    return (here + m_items.at(*(it + 1)).pos) / 2.0;                     // between
}

namespace stones {

namespace {
// "Delete “Ruby”" -- the step names what it was about.
std::string quoted(const Stones& s, const Stones::Id& id) {
    const auto v = s.get(id);
    return "“" + (v && !v->name.empty() ? v->name : std::string("Untitled")) + "”";
}

int clamp_size(int n) { return std::clamp(n, 1, 10); }

// Swap places with the neighbour on one side. Both records are in the step.
bool swap_with(StoneJournal& j, Stones& s, const Stones::Id& id, int dir, const std::string& label) {
    const auto order = s.ids();
    const auto it = std::find(order.begin(), order.end(), id);
    if (it == order.end()) return false;
    const auto i = it - order.begin();
    const auto k = i + dir;
    if (k < 0 || k >= static_cast<long>(order.size())) return false;
    const Stones::Id other = order[static_cast<std::size_t>(k)];
    return j.run(label, {id, other}, [&] {
        Stone a = *s.get(id), b = *s.get(other);
        std::swap(a.pos, b.pos);
        s.put(id, a);
        s.put(other, b);
    });
}
}  // namespace

Stones::Id add(StoneJournal& j, Stones& s, const std::string& name, const Stones::Id& after) {
    Stones::Id made;
    j.run("Add “" + name + "”", {}, [&] {
        made = s.fresh_id();
        s.put(made, Stone{name, 3, s.pos_after(after)});
        return std::vector<Stones::Id>{made};
    });
    return made;
}

bool remove(StoneJournal& j, Stones& s, const Stones::Id& id) {
    if (!s.get(id)) return false;
    return j.run("Delete " + quoted(s, id), {id}, [&] { s.put(id, std::nullopt); });
}

bool rename(StoneJournal& j, Stones& s, const Stones::Id& id, const std::string& name) {
    auto v = s.get(id);
    if (!v) return false;
    return j.run("Rename " + quoted(s, id), {id}, [&] { v->name = name; s.put(id, *v); });
}

bool move_up(StoneJournal& j, Stones& s, const Stones::Id& id)   { return swap_with(j, s, id, -1, "Move up"); }
bool move_down(StoneJournal& j, Stones& s, const Stones::Id& id) { return swap_with(j, s, id, +1, "Move down"); }

bool resize(StoneJournal& j, Stones& s, const Stones::Id& id, int size) {
    auto v = s.get(id);
    if (!v) return false;
    return j.run("Size of " + quoted(s, id), {id},
                 [&] { v->size = clamp_size(size); s.put(id, *v); }, "size:" + id);
}

void drag_begin(StoneJournal& j, Stones& s, const Stones::Id& id) {
    if (s.get(id)) j.begin("Size of " + quoted(s, id), {id});
}

void drag_to(StoneJournal& j, Stones& s, const Stones::Id& id, int size) {
    auto v = s.get(id);
    if (!v || v->size == clamp_size(size)) return;
    // Through the door even mid-drag: inside the open gesture this just writes.
    j.run("Size", {id}, [&] { v->size = clamp_size(size); s.put(id, *v); });
}

bool drag_end(StoneJournal& j) { return j.commit(); }

Stones::Id neighbour(const Stones& s, const Stones::Id& id) {
    const auto order = s.ids();
    const auto it = std::find(order.begin(), order.end(), id);
    if (it == order.end()) return {};
    if (it + 1 != order.end()) return *(it + 1);
    if (it != order.begin()) return *(it - 1);
    return {};
}

const std::vector<Verb>& verbs() {
    static const std::vector<Verb> table = {
        {"add",       [](StoneJournal& j, Stones& s, const Stones::Id& f) { add(j, s, "New", f); }},
        {"remove",    [](StoneJournal& j, Stones& s, const Stones::Id& f) { remove(j, s, f); }},
        {"rename",    [](StoneJournal& j, Stones& s, const Stones::Id& f) { rename(j, s, f, "Renamed"); }},
        {"move_up",   [](StoneJournal& j, Stones& s, const Stones::Id& f) { (void)f; move_up(j, s, s.ids().back()); }},
        {"move_down", [](StoneJournal& j, Stones& s, const Stones::Id& f) { move_down(j, s, f); }},
        {"resize",    [](StoneJournal& j, Stones& s, const Stones::Id& f) { resize(j, s, f, 9); }},
        {"drag",      [](StoneJournal& j, Stones& s, const Stones::Id& f) {
             drag_begin(j, s, f); drag_to(j, s, f, 5); drag_to(j, s, f, 8); drag_end(j); }},
    };
    return table;
}

}  // namespace stones
}  // namespace acorn::core
