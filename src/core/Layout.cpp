#include "core/Layout.hpp"

#include <algorithm>

// core/Layout.cpp -- the default layout and the model's own checks.

namespace acorn::core {

std::string default_layout_name(const std::string& table) { return table + " Form"; }

Layout default_layout(const std::string& table, const std::vector<std::string>& fields) {
    Layout l;
    l.table = table;
    l.name = default_layout_name(table);
    int y = kFormMargin;
    for (const auto& f : fields) {
        l.objects.push_back({ObjectKind::Text, "", f, kFormMargin, y, kFormLabelW, kFormRowH});
        l.objects.push_back({ObjectKind::Field, f, "", kFormMargin + kFormLabelW + kFormGap, y, kFormBoxW, kFormRowH});
        y += kFormRowStep;
    }
    l.width = kFormMargin + kFormLabelW + kFormGap + kFormBoxW + kFormMargin;
    l.height = y - kFormRowStep + kFormRowH + kFormMargin;
    if (fields.empty()) l.height = 2 * kFormMargin;
    return l;
}

bool missing_field(const LayoutObject& o, const std::vector<std::string>& fields) {
    return o.kind == ObjectKind::Field && std::find(fields.begin(), fields.end(), o.field) == fields.end();
}

std::string check_layout(const Layout& l) {
    if (l.table.empty()) return "a layout needs a table";
    if (l.name.empty() || l.name.front() == ' ' || l.name.back() == ' ')
        return "a layout needs a name (no space at either end)";
    for (unsigned char ch : l.name)
        if (ch < 0x20) return "a layout's name can't hold control characters";
    if (l.name.size() > 120) return "a layout's name is at most 120 bytes";
    if (l.width <= 0 || l.height <= 0) return "a layout needs a size";
    for (const auto& o : l.objects) {
        if (o.x < 0 || o.y < 0 || o.w <= 0 || o.h <= 0) return "an object needs a place and a size";
        if (o.kind == ObjectKind::Field && o.field.empty()) return "a field object needs a field";
    }
    return {};
}

// ── s010b: the editor's geometry ────────────────────────────────────────────

int snap(int v, int grid) {
    if (v < 0) v = 0;
    if (grid <= 1) return v;
    return ((v + grid / 2) / grid) * grid;
}

LayoutObject dragged(const LayoutObject& from, Grab g, int dx, int dy, int grid) {
    LayoutObject o = from;
    const int minsz = std::max(1, grid);
    if (g == Grab::Move) {
        o.x = snap(from.x + dx, grid);
        o.y = snap(from.y + dy, grid);
        return o;
    }
    const bool left   = g == Grab::W || g == Grab::NW || g == Grab::SW;
    const bool right  = g == Grab::E || g == Grab::NE || g == Grab::SE;
    const bool top    = g == Grab::N || g == Grab::NW || g == Grab::NE;
    const bool bottom = g == Grab::S || g == Grab::SW || g == Grab::SE;
    const int r = from.x + from.w, b = from.y + from.h;
    if (left) {
        const int nx = std::min(snap(from.x + dx, grid), r - minsz);
        o.x = std::max(0, nx);
        o.w = r - o.x;
    }
    if (right) o.w = std::max(minsz, snap(r + dx, grid) - from.x);
    if (top) {
        const int ny = std::min(snap(from.y + dy, grid), b - minsz);
        o.y = std::max(0, ny);
        o.h = b - o.y;
    }
    if (bottom) o.h = std::max(minsz, snap(b + dy, grid) - from.y);
    return o;
}

void fit(Layout& l) {
    int r = 0, b = 0;
    for (const auto& o : l.objects) {
        r = std::max(r, o.x + o.w);
        b = std::max(b, o.y + o.h);
    }
    auto up = [](int v) { return ((v + kLayoutGrid - 1) / kLayoutGrid) * kLayoutGrid; };
    l.width  = l.objects.empty() ? 2 * kFormMargin : up(r + kFormMargin);
    l.height = l.objects.empty() ? 2 * kFormMargin : up(b + kFormMargin);
}

std::string unique_layout_name(const std::string& base, const std::vector<std::string>& taken) {
    auto free = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) == taken.end(); };
    if (free(base)) return base;
    for (int i = 2;; ++i)
        if (auto n = base + " " + std::to_string(i); free(n)) return n;
}

std::string object_label(const LayoutObject& o) {
    if (o.kind == ObjectKind::Field) return "“" + o.field + "”";
    return "the words “" + o.text + "”";
}

}  // namespace acorn::core
