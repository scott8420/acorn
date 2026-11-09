#include "core/Print.hpp"
#include "core/Layout.hpp"

#include <algorithm>

#include <algorithm>

namespace acorn::core {

Paper paper_letter() { return {"Letter", 612, 792, 36}; }
Paper paper_a4() { return {"A4", 595.28, 841.89, 36}; }

const std::vector<std::string>& paper_names() {
    static const std::vector<std::string> n{"Letter", "Legal", "A4", "A5"};
    return n;
}
std::optional<Paper> paper_named(const std::string& name) {
    if (name == "Letter") return paper_letter();
    if (name == "Legal") return Paper{"Legal", 612, 1008, 36};
    if (name == "A4") return paper_a4();
    if (name == "A5") return Paper{"A5", 419.53, 595.28, 36};
    return std::nullopt;
}
Paper layout_paper(const Layout& l, const Paper& desktop) {
    Paper p = paper_named(l.paper).value_or(desktop);
    if (l.landscape && p.width < p.height) std::swap(p.width, p.height);
    p.margin = std::clamp(l.margin, 0, 144);
    return p;
}
const std::vector<int>& margin_choices() {
    static const std::vector<int> m{18, 36, 54, 72};
    return m;
}

std::vector<PrintPage> paginate(int header, int body, int footer, int records, double room) {
    std::vector<PrintPage> out;
    if (records <= 0) { out.push_back({0, 0}); return out; }
    if (body <= 0) { out.push_back({0, records}); return out; }   // nothing to repeat: one page
    const double left = room - header - footer;
    const int per = std::max(1, static_cast<int>(left / body));
    for (int first = 0; first < records; first += per) out.push_back({first, std::min(per, records - first)});
    return out;
}

std::vector<PrintPage> paginate_blocks(int header, int footer, const std::vector<int>& heights,
                                       const std::vector<bool>& keep, double room,
                                       const std::vector<bool>& breaks, const std::vector<int>& carry) {
    std::vector<PrintPage> out;
    const int n = static_cast<int>(heights.size());
    if (n == 0) { out.push_back({0, 0}); return out; }
    const double left = room - header - footer;
    auto kept = [&](int i) { return static_cast<std::size_t>(i) < keep.size() && keep[static_cast<std::size_t>(i)]; };
    auto broke = [&](int i) { return static_cast<std::size_t>(i) < breaks.size() && breaks[static_cast<std::size_t>(i)]; };
    int first = 0;
    while (first < n) {
        // s027b: a page that starts mid-group repeats its headings first.
        double used = first > 0 && static_cast<std::size_t>(first) < carry.size() ? carry[static_cast<std::size_t>(first)] : 0;
        int end = first;   // one past the last that fits
        bool forced = false;
        while (end < n && (end == first || used + heights[static_cast<std::size_t>(end)] <= left + 1e-6)) {
            used += heights[static_cast<std::size_t>(end)];
            ++end;
            if (broke(end - 1)) { forced = true; break; }   // s027b: a page break after a group
        }
        if (end < n && !forced) {   // a heading left at the foot goes over with what it heads (unless it's all the page has)
            int e = end;
            while (e > first && kept(e - 1)) --e;
            if (e > first) end = e;
        }
        out.push_back({first, end - first});
        first = end;
    }
    return out;
}

double print_scale(int width, const Paper& paper) {
    const double w = paper.width - 2 * paper.margin;
    if (width <= 0 || width <= w) return 1.0;
    return w / width;
}

std::string page_words(int page, int pages) { return "Page " + std::to_string(page) + " of " + std::to_string(pages); }

}  // namespace acorn::core
