#include "core/Print.hpp"

#include <algorithm>

namespace acorn::core {

Paper paper_letter() { return {"Letter", 612, 792, 36}; }
Paper paper_a4() { return {"A4", 595.28, 841.89, 36}; }

std::vector<PrintPage> paginate(int header, int body, int footer, int records, double room) {
    std::vector<PrintPage> out;
    if (records <= 0) { out.push_back({0, 0}); return out; }
    if (body <= 0) { out.push_back({0, records}); return out; }   // nothing to repeat: one page
    const double left = room - header - footer;
    const int per = std::max(1, static_cast<int>(left / body));
    for (int first = 0; first < records; first += per) out.push_back({first, std::min(per, records - first)});
    return out;
}

double print_scale(int width, const Paper& paper) {
    const double w = paper.width - 2 * paper.margin;
    if (width <= 0 || width <= w) return 1.0;
    return w / width;
}

std::string page_words(int page, int pages) { return "Page " + std::to_string(page) + " of " + std::to_string(pages); }

}  // namespace acorn::core
