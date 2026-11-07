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
    if (l.header < 0 || l.footer < 0 || l.header + l.footer >= l.height)
        return "a layout's header and footer leave no room for its body";
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

namespace {
int up8(int v) { return ((v + kLayoutGrid - 1) / kLayoutGrid) * kLayoutGrid; }
}  // namespace

void fit(Layout& l) {
    int r = 0, b = 0;
    for (const auto& o : l.objects) r = std::max(r, o.x + o.w);
    l.width = l.objects.empty() ? 2 * kFormMargin : up8(r + kFormMargin);
    if (l.header == 0 && l.footer == 0) {   // no parts: the body hugs its objects
        for (const auto& o : l.objects) b = std::max(b, o.y + o.h);
        l.height = l.objects.empty() ? 2 * kFormMargin : up8(b + kFormMargin);
        return;
    }
    // Parts: grow only (a dragged line stays where it was put). The body
    // makes room for what's in it, the footer moving down with its objects;
    // then the footer for what's in it.
    const int ft = footer_top(l);
    int fb = 0;
    for (const auto& o : l.objects)
        if (o.y < ft) b = std::max(b, o.y + o.h);
    const int need = std::max(up8(b + kFormGap), l.header + kLayoutGrid);
    if (need > ft) {
        for (auto& o : l.objects) if (o.y >= ft) o.y += need - ft;
        l.height += need - ft;
    }
    for (const auto& o : l.objects)
        if (o.y >= footer_top(l)) fb = std::max(fb, o.y + o.h);
    if (fb + kFormGap > l.height) {
        l.footer += up8(fb + kFormGap) - l.height;
        l.height = up8(fb + kFormGap);
    }
}

Part part_of(const Layout& l, const LayoutObject& o) {
    if (l.header > 0 && o.y < l.header) return Part::Header;
    if (l.footer > 0 && o.y >= footer_top(l)) return Part::Footer;
    return Part::Body;
}

bool move_part_line(Layout& l, Part line, int y) {
    y = snap(y);
    const int ft = footer_top(l);
    if (line == Part::Header) {
        int lo = 0;   // the header's own objects stay whole
        for (const auto& o : l.objects) if (l.header > 0 && o.y < l.header) lo = std::max(lo, up8(o.y + o.h));
        y = std::max(y, lo);
        const int d = y - l.header;
        if (d == 0) return false;
        for (auto& o : l.objects) if (!(l.header > 0 && o.y < l.header)) o.y += d;
        l.header = y;
        l.height += d;
        return true;
    }
    if (line == Part::Footer && l.footer == 0 && y > l.height) {   // down from the bottom edge: a footer below
        l.footer = y - l.height;
        l.height = y;
        return true;
    }
    if (line == Part::Footer) {
        int lo = l.header + kLayoutGrid, hi = l.height;   // the body keeps a row; the footer its objects
        for (const auto& o : l.objects) {
            if (o.y < ft) lo = std::max(lo, up8(o.y + o.h + kFormGap));   // the room fit() keeps under the body
            else hi = std::min(hi, o.y);
        }
        y = std::clamp(y, lo, std::max(lo, hi));
        if (y == ft) return false;
        l.footer = l.height - y;
        return true;
    }
    return false;
}

bool move_layout_bottom(Layout& l, int y) {
    if (l.footer == 0) return move_part_line(l, Part::Footer, y);
    const int ft = footer_top(l);
    int lo = ft + kLayoutGrid;
    for (const auto& o : l.objects) if (o.y >= ft) lo = std::max(lo, up8(o.y + o.h));
    y = std::max(snap(y), lo);
    if (y == l.height) return false;
    l.footer = y - ft;
    l.height = y;
    return true;
}

std::string unique_layout_name(const std::string& base, const std::vector<std::string>& taken) {
    auto free = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) == taken.end(); };
    if (free(base)) return base;
    for (int i = 2;; ++i)
        if (auto n = base + " " + std::to_string(i); free(n)) return n;
}

std::string object_label(const LayoutObject& o) {
    switch (o.kind) {
        case ObjectKind::Field: return "“" + o.field + "”";
        case ObjectKind::Line:  return "a line";
        case ObjectKind::Box:   return "a box";
        default:                return "the words “" + o.text + "”";
    }
}

int next_row_y(const Layout& l) {   // s010d: in the body -- under its objects, never into the footer
    int b = 0;
    for (const auto& o : l.objects)
        if (part_of(l, o) != Part::Footer) b = std::max(b, o.y + o.h);
    if (b <= l.header) return l.header + kFormMargin;
    return ((b + kFormGap + kLayoutGrid - 1) / kLayoutGrid) * kLayoutGrid;
}

int add_object(Layout& l, ObjectKind kind, const std::string& what) {
    const int y = next_row_y(l);
    const int boxx = kFormMargin + kFormLabelW + kFormGap;
    int n = 1;
    switch (kind) {
        case ObjectKind::Field:
            l.objects.push_back({ObjectKind::Text, "", what, kFormMargin, y, kFormLabelW, kFormRowH});
            l.objects.push_back({ObjectKind::Field, what, "", boxx, y, kFormBoxW, kFormRowH});
            n = 2;
            break;
        case ObjectKind::Text:
            l.objects.push_back({ObjectKind::Text, "", what, kFormMargin, y, kFormLabelW, kFormRowH});
            break;
        case ObjectKind::Line: {
            const int w = std::max(8 * kLayoutGrid, l.width - 2 * kFormMargin);
            l.objects.push_back({ObjectKind::Line, "", "", kFormMargin, y, (w / kLayoutGrid) * kLayoutGrid, kLayoutGrid});
            break;
        }
        case ObjectKind::Box:
            l.objects.push_back({ObjectKind::Box, "", "", kFormMargin, y, 20 * kLayoutGrid, 8 * kLayoutGrid});
            break;
    }
    fit(l);
    return n;
}

std::vector<std::string> fields_not_on(const Layout& l, const std::vector<std::string>& fields) {
    std::vector<std::string> out;
    for (const auto& f : fields) {
        bool on = false;
        for (const auto& o : l.objects) on = on || (o.kind == ObjectKind::Field && o.field == f);
        if (!on) out.push_back(f);
    }
    return out;
}

// ── s010e: several picked ───────────────────────────────────────────────────

void toggle_pick(std::vector<int>& picks, int i) {
    if (i < 0) return;
    if (auto it = std::lower_bound(picks.begin(), picks.end(), i); it != picks.end() && *it == i) picks.erase(it);
    else picks.insert(it, i);
}

bool is_picked(const std::vector<int>& picks, int i) { return std::binary_search(picks.begin(), picks.end(), i); }

std::vector<int> body_objects(const Layout& l) {
    std::vector<int> out;
    for (std::size_t i = 0; i < l.objects.size(); ++i)
        if (part_of(l, l.objects[i]) == Part::Body) out.push_back(static_cast<int>(i));
    return out;
}

Layout moved_objects(const Layout& from, const std::vector<int>& picks, int anchor, int dx, int dy, int grid) {
    Layout l = from;
    const int n = static_cast<int>(from.objects.size());
    std::vector<int> in;
    for (int i : picks) if (i >= 0 && i < n) in.push_back(i);
    if (in.empty()) return l;
    if (anchor < 0 || anchor >= n || !is_picked(in, anchor)) anchor = in.front();
    const auto& a = from.objects[static_cast<std::size_t>(anchor)];
    const auto lone = dragged(a, Grab::Move, dx, dy, grid);
    int ddx = lone.x - a.x, ddy = lone.y - a.y;
    for (int i : in) {   // the group stops whole at the top / left
        const auto& o = from.objects[static_cast<std::size_t>(i)];
        ddx = std::max(ddx, -o.x);
        ddy = std::max(ddy, -o.y);
    }
    for (int i : in) {
        auto& o = l.objects[static_cast<std::size_t>(i)];
        o.x += ddx;
        o.y += ddy;
    }
    return l;
}

int remove_objects(Layout& l, const std::vector<int>& picks) {
    std::vector<int> in;
    for (int i : picks) if (i >= 0 && i < static_cast<int>(l.objects.size())) in.push_back(i);
    std::sort(in.begin(), in.end());
    in.erase(std::unique(in.begin(), in.end()), in.end());
    for (auto it = in.rbegin(); it != in.rend(); ++it) l.objects.erase(l.objects.begin() + *it);
    return static_cast<int>(in.size());
}

std::string picks_label(const Layout& l, const std::vector<int>& picks) {
    if (picks.size() == 1 && picks.front() >= 0 && picks.front() < static_cast<int>(l.objects.size()))
        return object_label(l.objects[static_cast<std::size_t>(picks.front())]);
    return std::to_string(picks.size()) + " objects";
}

}  // namespace acorn::core
