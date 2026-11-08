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
    int subs = 0;
    for (const auto& x : l.subs) {   // s015b
        if (x.by.empty()) return "a sub-summary needs the field it groups by";
        if (x.height <= 0) return "a sub-summary needs a height";
        subs += x.height;
    }
    if (l.across < 1 || l.across > kLabelsMax) return "labels are 1 to " + std::to_string(kLabelsMax) + " across";   // s016
    if (l.gap < 0) return "the gap between labels can't be negative";
    if (l.header < 0 || l.footer < 0 || l.header + l.footer + subs >= l.height)
        return "a layout's header and footer leave no room for its body";
    for (const auto& o : l.objects) {
        if (o.x < 0 || o.y < 0 || o.w <= 0 || o.h <= 0) return "an object needs a place and a size";
        if (o.kind == ObjectKind::Field && o.field.empty()) return "a field object needs a field";
        if (o.kind == ObjectKind::Portal && (o.from.empty() || o.field.empty()))
            return "a portal needs the table it lists and its link field";
        if (o.kind == ObjectKind::Related && (o.from.empty() || o.field.empty() || o.shows.empty()))
            return "a related field needs its link field, the table it points at and the field it shows";
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
// s011a fix: an object about to land at [y, y + h) stays in the body -- with
// a footer in the way, the footer (its objects too) moves down to make room.
// Before this, Add on a full body put the object ON the footer line, so it
// belonged to the footer and the Form never showed it.
void room_in_body(Layout& l, int y, int h) {
    const bool trailing = std::any_of(l.subs.begin(), l.subs.end(), [](const SubSummary& x) { return !x.leading; });
    if (l.footer == 0 && !trailing) return;
    const int ft = body_bottom(l);   // s015b: the trailing subs move down too
    const int need = up8(y + h + kFormGap);
    if (need <= ft) return;
    const int d = need - ft;
    for (auto& o : l.objects) if (o.y >= ft) o.y += d;
    l.height += d;
}
}  // namespace

void fit(Layout& l) {
    int r = 0, b = 0;
    for (const auto& o : l.objects) r = std::max(r, o.x + o.w);
    l.width = l.objects.empty() ? 2 * kFormMargin : up8(r + kFormMargin);
    if (l.header == 0 && l.footer == 0 && l.subs.empty()) {   // no parts: the body hugs its objects
        for (const auto& o : l.objects) b = std::max(b, o.y + o.h);
        l.height = l.objects.empty() ? 2 * kFormMargin : up8(b + kFormMargin);
        return;
    }
    // Parts: grow only (a dragged line stays where it was put). The body
    // makes room for what's in it, the footer moving down with its objects;
    // then the footer for what's in it.
    // s015b: the body and each sub, top-down, the same way -- what's under
    // a band moves down when the band grows.
    int fb = 0;
    for (std::size_t k = 0;; ++k) {
        const auto bs = bands(l);
        if (k >= bs.size()) break;
        const Band bd = bs[k];
        if (bd.part != Part::Body && bd.part != Part::Sub) continue;
        const int bottom = bd.top + bd.height;
        b = 0;
        for (const auto& o : l.objects) {
            const Part p = part_of(l, o);
            const bool mine = bd.part == Part::Body ? (o.y < bottom && p != Part::Sub)   // the body (and the header, as before)
                                                    : (p == Part::Sub && sub_of(l, o) == bd.sub);
            if (mine) b = std::max(b, o.y + o.h);
        }
        const int need = std::max(b > 0 ? up8(b + kFormGap) : 0, bd.top + kLayoutGrid);
        if (need > bottom) {
            for (auto& o : l.objects) if (o.y >= bottom) o.y += need - bottom;
            if (bd.part == Part::Sub) l.subs[static_cast<std::size_t>(bd.sub)].height += need - bottom;
            l.height += need - bottom;
        }
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
    if (sub_of(l, o) >= 0) return Part::Sub;
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
    if (line == Part::Footer && !l.subs.empty() && !l.subs.back().leading) {
        // s015b: over trailing subs the line is the last one's bottom -- it
        // sizes that sub, the footer moving with it.
        const auto bs = bands(l);
        for (int k = static_cast<int>(bs.size()) - 1; k >= 0; --k)
            if (bs[static_cast<std::size_t>(k)].part == Part::Sub) return move_band_line(l, k, y);
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
        case ObjectKind::Portal: return "the portal to “" + o.from + "”";
        case ObjectKind::Related: return "“" + o.from + "." + o.shows + "”";
        default:                return "the words “" + o.text + "”";
    }
}

int next_row_y(const Layout& l) {   // s010d: in the body -- under its objects, never into the footer
    int b = 0;
    for (const auto& o : l.objects)   // s015b: the body's own (not a sub's)
        if (part_of(l, o) == Part::Body) b = std::max(b, o.y + o.h);
    if (b <= body_top(l)) return body_top(l) + kFormMargin;
    return ((b + kFormGap + kLayoutGrid - 1) / kLayoutGrid) * kLayoutGrid;
}

int add_object(Layout& l, ObjectKind kind, const std::string& what) {
    const int y = next_row_y(l);
    if (kind != ObjectKind::Portal)
        room_in_body(l, y, kind == ObjectKind::Box ? 8 * kLayoutGrid : kind == ObjectKind::Line ? kLayoutGrid : kFormRowH);
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
        case ObjectKind::Portal:    // add_portal / add_related know their link; a bare one would be invalid
        case ObjectKind::Related:
            return 0;
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

// ── s011a: portals ──────────────────────────────────────────────────────────

bool missing_link(const LayoutObject& o, const std::vector<PortalLink>& links) {
    if (o.kind != ObjectKind::Portal) return false;
    return std::find(links.begin(), links.end(), PortalLink{o.from, o.field}) == links.end();
}

std::vector<std::string> portal_columns(const std::vector<std::string>& from_fields, const std::string& link_field,
                                        const std::string& counting) {
    std::vector<std::string> out;
    for (const auto& f : from_fields)
        if (f != link_field && f != counting) out.push_back(f);
    return out;
}

void add_portal(Layout& l, const PortalLink& link, const std::vector<std::string>& columns) {
    LayoutObject o;
    o.kind = ObjectKind::Portal;
    o.from = link.from;
    o.field = link.field;
    o.columns = columns;
    o.x = kFormMargin;
    o.y = next_row_y(l);
    o.w = kFormLabelW + kFormGap + kFormBoxW;                // a default row's width
    o.h = kPortalRowH * (1 + kPortalRows);                   // the names, then four records
    room_in_body(l, o.y, o.h);
    l.objects.push_back(std::move(o));
    fit(l);
}

std::vector<std::string> columns_not_on(const LayoutObject& o, const std::vector<std::string>& from_fields) {
    std::vector<std::string> out;
    for (const auto& f : from_fields)
        if (f != o.field && std::find(o.columns.begin(), o.columns.end(), f) == o.columns.end()) out.push_back(f);
    return out;
}

bool move_column(LayoutObject& o, int i, int delta) {
    const int n = static_cast<int>(o.columns.size()), j = i + delta;
    if (i < 0 || i >= n || j < 0 || j >= n || delta == 0) return false;
    const auto c = o.columns[static_cast<std::size_t>(i)];
    o.columns.erase(o.columns.begin() + i);
    o.columns.insert(o.columns.begin() + j, c);
    return true;
}

std::string link_label(const PortalLink& k) { return k.from + " · " + k.field; }

// ── s011d: related fields ───────────────────────────────────────────────────

bool missing_related_link(const LayoutObject& o, const std::vector<RelatedLink>& links) {
    if (o.kind != ObjectKind::Related) return false;
    return std::find(links.begin(), links.end(), RelatedLink{o.field, o.from}) == links.end();
}

std::string related_label(const std::string& link_field, const std::string& shows) {
    return link_field + " → " + shows;
}

std::vector<std::string> related_not_on(const Layout& l, const RelatedLink& link,
                                        const std::vector<std::string>& to_fields, const std::string& to_key) {
    std::vector<std::string> out;
    for (const auto& f : to_fields) {
        if (f == to_key) continue;
        bool on = false;
        for (const auto& o : l.objects)
            on = on || (o.kind == ObjectKind::Related && o.field == link.field && o.from == link.to && o.shows == f);
        if (!on) out.push_back(f);
    }
    return out;
}

int add_related(Layout& l, const RelatedLink& link, const std::string& shows) {
    const int y = next_row_y(l);
    room_in_body(l, y, kFormRowH);
    l.objects.push_back({ObjectKind::Text, "", shows, kFormMargin, y, kFormLabelW, kFormRowH});
    LayoutObject o;
    o.kind = ObjectKind::Related;
    o.field = link.field;
    o.from = link.to;
    o.shows = shows;
    o.x = kFormMargin + kFormLabelW + kFormGap;
    o.y = y;
    o.w = kFormBoxW;
    o.h = kFormRowH;
    l.objects.push_back(std::move(o));
    fit(l);
    return 2;
}

// ── s012a: the list view ────────────────────────────────────────────────────
int part_top(const Layout& l, Part p) {
    switch (p) {
        case Part::Header: return 0;
        case Part::Body:   return body_top(l);
        case Part::Footer: return footer_top(l);
        case Part::Sub:    return 0;   // which? sub_top
    }
    return 0;
}

int part_height(const Layout& l, Part p) {
    switch (p) {
        case Part::Header: return std::max(0, l.header);
        case Part::Body:   return std::max(0, body_bottom(l) - body_top(l));
        case Part::Footer: return std::max(0, l.footer);
        case Part::Sub:    return 0;
    }
    return 0;
}

std::vector<int> part_objects(const Layout& l, Part p) {
    std::vector<int> out;
    auto behind = [](const LayoutObject& o) { return o.kind == ObjectKind::Line || o.kind == ObjectKind::Box; };
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < l.objects.size(); ++i)
            if (behind(l.objects[i]) == (pass == 0) && part_of(l, l.objects[i]) == p) out.push_back(static_cast<int>(i));
    return out;
}

// ── s012b: words alignment ──────────────────────────────────────────────────
const char* align_name(Align a) {
    switch (a) {
        case Align::Left:   return "left";
        case Align::Center: return "center";
        case Align::Right:  return "right";
    }
    return "right";
}

Align parse_align(const std::string& s) {
    if (s == "left") return Align::Left;
    if (s == "center") return Align::Center;
    return Align::Right;
}

const char* align_label(Align a) {
    return a == Align::Left ? "left" : a == Align::Center ? "centred" : "right";
}

// ── s015b: sub-summary parts ────────────────────────────────────────────────

std::vector<Band> bands(const Layout& l) {
    std::vector<Band> out;
    if (l.header > 0) out.push_back({Part::Header, -1, 0, l.header});
    int y = l.header;
    for (std::size_t i = 0; i < l.subs.size(); ++i)
        if (l.subs[i].leading) { out.push_back({Part::Sub, static_cast<int>(i), y, l.subs[i].height}); y += l.subs[i].height; }
    int trail = 0;
    for (const auto& x : l.subs) if (!x.leading) trail += x.height;
    const int bb = footer_top(l) - trail;
    out.push_back({Part::Body, -1, y, std::max(0, bb - y)});
    y = bb;
    for (std::size_t i = 0; i < l.subs.size(); ++i)
        if (!l.subs[i].leading) { out.push_back({Part::Sub, static_cast<int>(i), y, l.subs[i].height}); y += l.subs[i].height; }
    if (l.footer > 0) out.push_back({Part::Footer, -1, footer_top(l), l.footer});
    return out;
}

int body_top(const Layout& l) {
    int y = l.header;
    for (const auto& x : l.subs) if (x.leading) y += x.height;
    return y;
}

int body_bottom(const Layout& l) {
    int y = footer_top(l);
    for (const auto& x : l.subs) if (!x.leading) y -= x.height;
    return y;
}

int sub_top(const Layout& l, int i) {
    for (const auto& b : bands(l)) if (b.part == Part::Sub && b.sub == i) return b.top;
    return 0;
}

int sub_of(const Layout& l, const LayoutObject& o) {
    if (l.subs.empty()) return -1;
    for (const auto& b : bands(l))
        if (b.part == Part::Sub && o.y >= b.top && o.y < b.top + b.height) return b.sub;
    return -1;
}

std::vector<int> band_objects(const Layout& l, const Band& b) {
    std::vector<int> out;
    auto behind = [](const LayoutObject& o) { return o.kind == ObjectKind::Line || o.kind == ObjectKind::Box; };
    auto in = [&](const LayoutObject& o) {
        const Part p = part_of(l, o);
        return p == b.part && (p != Part::Sub || sub_of(l, o) == b.sub);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < l.objects.size(); ++i)
            if (behind(l.objects[i]) == (pass == 0) && in(l.objects[i])) out.push_back(static_cast<int>(i));
    return out;
}

std::vector<std::string> group_fields(const Layout& l) {
    std::vector<std::string> out;
    auto add = [&](const std::string& f) { if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f); };
    for (const auto& x : l.subs) if (x.leading) add(x.by);
    for (auto it = l.subs.rbegin(); it != l.subs.rend(); ++it) if (!it->leading) add(it->by);
    return out;
}

int group_level(const Layout& l, int i) {
    if (i < 0 || static_cast<std::size_t>(i) >= l.subs.size()) return -1;
    const auto g = group_fields(l);
    return static_cast<int>(std::find(g.begin(), g.end(), l.subs[static_cast<std::size_t>(i)].by) - g.begin());
}

std::string sub_label(const SubSummary& s) {
    return std::string(s.leading ? "leading" : "trailing") + " sub-summary by “" + s.by + "”";
}

int add_sub(Layout& l, const std::string& by, bool leading) {
    // Where it goes: leading -- the body's top (under the leading subs);
    // trailing -- the body's bottom (over the trailing subs).
    const int at = leading ? body_top(l) : body_bottom(l);
    for (auto& o : l.objects) if (o.y >= at) o.y += kSubH;
    l.height += kSubH;
    std::size_t pos = 0;   // leading: after the last leading; trailing: before the first trailing
    if (leading) { for (std::size_t i = 0; i < l.subs.size(); ++i) if (l.subs[i].leading) pos = i + 1; }
    else {
        pos = l.subs.size();
        for (std::size_t i = 0; i < l.subs.size(); ++i) if (!l.subs[i].leading) { pos = i; break; }
    }
    l.subs.insert(l.subs.begin() + static_cast<std::ptrdiff_t>(pos), SubSummary{by, leading, kSubH});
    l.objects.push_back({ObjectKind::Field, by, "", kFormMargin, at + kLayoutGrid, kFormBoxW, kFormRowH});
    return static_cast<int>(pos);
}

bool remove_sub(Layout& l, int i) {
    if (i < 0 || static_cast<std::size_t>(i) >= l.subs.size()) return false;
    const int top = sub_top(l, i), h = l.subs[static_cast<std::size_t>(i)].height;
    std::vector<LayoutObject> kept;
    for (auto& o : l.objects) {
        if (o.y >= top && o.y < top + h) continue;   // its own
        if (o.y >= top + h) o.y -= h;
        kept.push_back(o);
    }
    l.objects = std::move(kept);
    l.subs.erase(l.subs.begin() + i);
    l.height -= h;
    return true;
}

bool move_band_line(Layout& l, int b, int y) {
    const auto bs = bands(l);
    if (b < 0 || static_cast<std::size_t>(b) >= bs.size()) return false;
    const Band bd = bs[static_cast<std::size_t>(b)];
    if (bd.part != Part::Body && bd.part != Part::Sub) return false;
    const int bottom = bd.top + bd.height;
    int lo = bd.top + kLayoutGrid;   // a row, and the band's own objects whole
    for (const auto& o : l.objects)
        if (o.y >= bd.top && o.y < bottom) lo = std::max(lo, up8(o.y + o.h));
    y = std::max(snap(y), lo);
    const int d = y - bottom;
    if (d == 0) return false;
    for (auto& o : l.objects) if (o.y >= bottom) o.y += d;
    if (bd.part == Part::Sub) l.subs[static_cast<std::size_t>(bd.sub)].height += d;
    l.height += d;
    return true;
}

std::vector<ReportBlock> report_blocks(const Layout& l, const std::vector<std::vector<std::string>>& keys) {
    std::vector<ReportBlock> out;
    const auto bs = bands(l);
    const int n = static_cast<int>(keys.size());
    // The outermost level at which records a and b part company (levels
    // past the end -- no change -- when they're alike in every field).
    auto parts_at = [&](int a, int c) {
        const auto& ka = keys[static_cast<std::size_t>(a)];
        const auto& kc = keys[static_cast<std::size_t>(c)];
        std::size_t k = 0;
        while (k < ka.size() && k < kc.size() && ka[k] == kc[k]) ++k;
        return static_cast<int>(k);
    };
    for (int r = 0; r < n; ++r) {
        const int starts = r == 0 ? 0 : parts_at(r - 1, r);       // groups at this level and in start here
        const int ends = r == n - 1 ? 0 : parts_at(r, r + 1);     // ... end here
        for (const auto& bd : bs)
            if (bd.part == Part::Sub && l.subs[static_cast<std::size_t>(bd.sub)].leading && group_level(l, bd.sub) >= starts)
                out.push_back({Part::Sub, bd.sub, r});
        out.push_back({Part::Body, -1, r});
        for (const auto& bd : bs)
            if (bd.part == Part::Sub && !l.subs[static_cast<std::size_t>(bd.sub)].leading && group_level(l, bd.sub) >= ends)
                out.push_back({Part::Sub, bd.sub, r});
    }
    return out;
}

// ── s016: labels ────────────────────────────────────────────────────────────

int labels_width(const Layout& l) {
    const int n = std::max(1, l.across);
    return n * l.width + (n - 1) * std::max(0, l.gap);
}

std::vector<ReportBlock> label_blocks(const Layout& l, int records) {
    std::vector<ReportBlock> out;
    const int n = std::max(1, l.across);
    for (int r = 0; r < records; r += n) out.push_back({Part::Body, -1, r});
    return out;
}

std::string labels_label(const Layout& l) {
    if (!is_labels(l)) return "one per row";
    return std::to_string(l.across) + " across, " + std::to_string(l.gap) + " px apart";
}

}  // namespace acorn::core
