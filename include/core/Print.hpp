#pragma once
// core/Print -- PREVIEW and printing (s015a): how a layout's records fall on
// pages. A page is the paper less its margins; the layout's header sits at
// its top and its footer at its foot on EVERY page, and as many bodies (one
// per record) as fit go between. A record is never cut across two pages. With
// no records there is still one page (the header and the footer). Units are
// points (1/72 in) -- a layout pixel prints as a point, scaled down whole when
// the layout is wider than the paper.
// GTK-free, headless-testable.
#include <optional>
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
// s026a: the papers a layout may name, in the order offered; one by name
// (an unknown name: nullopt).
const std::vector<std::string>& paper_names();
std::optional<Paper> paper_named(const std::string& name);
// A layout's page: its paper (or `desktop` when it names none / one Acorn
// doesn't know), on its side when landscape, its margin.
struct Layout;
Paper layout_paper(const Layout& l, const Paper& desktop);
// The margins a layout may pick, points: 1/4, 1/2, 3/4, 1 inch.
const std::vector<int>& margin_choices();

struct PrintPage {
    int first = 0;   // the first record on it
    int count = 0;   // how many
    bool operator==(const PrintPage&) const = default;
};
// `header` / `body` / `footer` -- the layout's parts' heights; `room` -- the
// page's height to fill (already scaled into layout units). A body taller
// than the room still gets a page each.
std::vector<PrintPage> paginate(int header, int body, int footer, int records, double room);
// s015b: a report's run of parts (core::report_blocks) on pages: `heights` --
// each block's, in order; `first` / `count` on a page are BLOCK indexes. A
// block marked `keep` (a leading sub-summary) is never the last thing on a
// page while something follows it -- it moves over with what it heads.
// No blocks: one page.
// s027b: `breaks[i]` -- a page ends after block i; `carry[i]` -- the height a
// page STARTING at block i (not the first page) gives to repeated headings.
std::vector<PrintPage> paginate_blocks(int header, int footer, const std::vector<int>& heights,
                                       const std::vector<bool>& keep, double room,
                                       const std::vector<bool>& breaks = {}, const std::vector<int>& carry = {});

// How much a layout `width` wide is scaled to fit `paper`'s printable width
// (1 when it fits).
double print_scale(int width, const Paper& paper);

// "Page 2 of 5"
std::string page_words(int page, int pages);

}  // namespace acorn::core
