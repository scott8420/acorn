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

}  // namespace acorn::core
