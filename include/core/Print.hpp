#pragma once
// core/Print -- PREVIEW and printing (s015a): how a layout's records fall on
// pages. A page is the paper less its margins; the layout's header sits at
// its top and its footer at its foot on EVERY page, and as many bodies (one
// per record) as fit go between. A record is never cut across two pages. With
// no records there is still one page (the header and the footer). Units are
// points (1/72 in) -- a layout pixel prints as a point, scaled down whole when
// the layout is wider than the paper.
// GTK-free, headless-testable.
#include <string>
#include <vector>

namespace acorn::core {

struct Paper {
    std::string name;            // "Letter" / "A4"
    double width = 0, height = 0;   // points
    double margin = 36;          // each side, points (half an inch)
};
Paper paper_letter();
Paper paper_a4();

struct PrintPage {
    int first = 0;   // the first record on it
    int count = 0;   // how many
    bool operator==(const PrintPage&) const = default;
};
// `header` / `body` / `footer` -- the layout's parts' heights; `room` -- the
// page's height to fill (already scaled into layout units). A body taller
// than the room still gets a page each.
std::vector<PrintPage> paginate(int header, int body, int footer, int records, double room);

// How much a layout `width` wide is scaled to fit `paper`'s printable width
// (1 when it fits).
double print_scale(int width, const Paper& paper);

// "Page 2 of 5"
std::string page_words(int page, int pages);

}  // namespace acorn::core
