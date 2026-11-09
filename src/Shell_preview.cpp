#include "Shell.hpp"
#include "Log.hpp"
#include "core/Print.hpp"
#include "db/Print.hpp"

#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <giomm/liststore.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/label.h>
#include <gtkmm/pagesetup.h>
#include <gtkmm/papersize.h>
#include <gtkmm/printoperation.h>
#include <pangomm/layout.h>

#include <cmath>
#include <filesystem>

// Shell_preview.cpp -- PREVIEW, PRINT, PDF (s015a). Browse -> Preview lays the
// List's layout on paper: the header at the top and the footer at the foot
// of EVERY page, the body once per record between them, never a record cut
// in two, "Page 2 of 5" under each. It is read-only. Print... hands the same
// pages to GtkPrintOperation; Save as PDF... draws them on Cairo's PDF
// surface -- one drawing function for all three, so what is previewed is
// what prints. The records are the ones Browse shows: every record, or the
// found ones after a Find, in Browse's order (at most db::kPrintMax).
// s015b: a layout with SUB-SUMMARY parts is a report -- the records sorted by
// its groups' fields first, each sub printed where its group starts (leading)
// or ends (trailing), its summaries worked out over that group (one GROUP BY
// SELECT per level).
// s016: a LABELS layout (across > 1) prints its body `across` to a row, `gap`
// apart both ways, as many rows a page as fit -- no sub-summaries.

namespace acorn {

struct PreviewDoc {
    core::Layout layout;
    core::Paper paper;
    double scale = 1.0;
    double room = 0;                          // the page's height in layout units, the folio line left out
    std::vector<db::PrintedRecord> records;   // each record as its body prints it
    struct Block {
        core::ReportBlock at;
        db::PrintedRecord sub;                // a sub-summary: its record with its group's summaries
        int height = 0;
    };
    std::vector<Block> blocks;                // s015b: what prints, in order (pages are over these)
    std::vector<core::PrintPage> pages;
    std::string title;                        // "contacts -- Contact List"
    int total = 0;                            // records there were (more than shown when capped)
};

namespace {

constexpr double kFolio = 18;   // the "Page 2 of 5" line under the page's content, points

std::string curly(const std::string& s) { return "“" + s + "”"; }

core::Paper default_paper() {
    const std::string n = Gtk::PaperSize::get_default();   // the desktop's (the locale's) paper: "na_letter", "iso_a4" ...
    const Gtk::PaperSize ps(n);
    if (n.find("letter") != std::string::npos) return core::paper_letter();
    if (n.find("a4") != std::string::npos) return core::paper_a4();
    core::Paper p;
    p.name = ps.get_display_name();
    p.width = ps.get_width(Gtk::Unit::POINTS);
    p.height = ps.get_height(Gtk::Unit::POINTS);
    return p;
}

void text_in(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& s, double x, double y, double w, double h,
             Pango::Alignment how, double grey, bool bold = false, const core::LayoutObject* o = nullptr) {
    if (s.empty() || w <= 2) return;
    auto pl = Pango::Layout::create(cr);
    Pango::FontDescription fd(o && core::has_font(*o) ? core::font_desc(*o, "Sans", 9) : std::string("Sans 9"));   // s026b
    if (bold) fd.set_weight(Pango::Weight::BOLD);
    pl->set_font_description(fd);
    pl->set_text(s);
    pl->set_width(static_cast<int>((w - 4) * PANGO_SCALE));
    pl->set_ellipsize(Pango::EllipsizeMode::END);
    pl->set_alignment(how);
    int tw = 0, th = 0;
    pl->get_pixel_size(tw, th);
    cr->save();
    cr->rectangle(x, y, w, h);
    cr->clip();
    cr->set_source_rgb(grey, grey, grey);
    cr->move_to(x + 2, y + std::max(0.0, (std::min<double>(h, kFolio * 2) - th) / 2));
    pl->show_in_cairo_context(cr);
    cr->restore();
}

core::Band band_of(const core::Layout& L, core::Part part, int sub = -1) {
    for (const auto& b : core::bands(L)) if (b.part == part && b.sub == sub) return b;
    return {part, sub, core::part_top(L, part), 0};
}

// One band of the layout for one record, its top at `y0` (layout units).
void draw_part(const Cairo::RefPtr<Cairo::Context>& cr, const PreviewDoc& d, const core::Band& band, const db::PrintedRecord& rec,
               double y0) {
    const auto& L = d.layout;
    const int top = band.top;
    for (int oi : core::band_objects(L, band)) {
        const auto& o = L.objects[static_cast<std::size_t>(oi)];
        if (static_cast<std::size_t>(oi) >= rec.size()) continue;
        const auto& p = rec[static_cast<std::size_t>(oi)];
        const double x = o.x, y = y0 + o.y - top, w = o.w, h = o.h;
        cr->set_line_width(0.6);
        if (p.missing) {
            cr->set_source_rgb(0.6, 0.6, 0.6);
            cr->set_dash(std::vector<double>{3, 2}, 0);
            cr->rectangle(x + 0.5, y + 0.5, w - 1, h - 1);
            cr->stroke();
            cr->unset_dash();
            text_in(cr, p.text, x, y, w, h, Pango::Alignment::LEFT, 0.55);
            continue;
        }
        switch (o.kind) {
        case core::ObjectKind::Button: break;   // s017b: a button is for the screen -- never printed
        case core::ObjectKind::Line:
            cr->set_source_rgb(0.45, 0.45, 0.45);
            if (w >= h) { cr->move_to(x, y + h / 2); cr->line_to(x + w, y + h / 2); }
            else        { cr->move_to(x + w / 2, y); cr->line_to(x + w / 2, y + h); }
            cr->stroke();
            break;
        case core::ObjectKind::Box:
            cr->set_source_rgb(0.55, 0.55, 0.55);
            cr->rectangle(x + 0.5, y + 0.5, w - 1, h - 1);
            cr->stroke();
            break;
        case core::ObjectKind::Text:
            text_in(cr, p.text, x, y, w, h, o.align == core::Align::Left ? Pango::Alignment::LEFT
                                            : o.align == core::Align::Center ? Pango::Alignment::CENTER
                                                                             : Pango::Alignment::RIGHT, 0.35, false, &o);
            break;
        case core::ObjectKind::Field:
        case core::ObjectKind::Related:
            text_in(cr, p.text, x, y, w, h, Pango::Alignment::LEFT, 0.05, false, &o);
            if (core::is_labels(L)) break;          // s016: a label is plain words
            cr->set_source_rgb(0.82, 0.82, 0.82);   // a hairline under the value, where its box was
            cr->move_to(x, y + h - 0.5);
            cr->line_to(x + w, y + h - 0.5);
            cr->stroke();
            break;
        case core::ObjectKind::Portal: {
            cr->set_source_rgb(0.6, 0.6, 0.6);
            cr->rectangle(x + 0.5, y + 0.5, w - 1, h - 1);
            cr->stroke();
            const std::size_t n = std::max<std::size_t>(1, o.columns.size());
            const double cw = w / static_cast<double>(n);
            for (std::size_t c = 0; c < o.columns.size(); ++c)
                text_in(cr, o.columns[c], x + c * cw, y, cw, core::kPortalRowH, Pango::Alignment::LEFT, 0.35, true);
            double ry = y + core::kPortalRowH;
            for (const auto& r : p.rows) {
                if (ry + core::kPortalRowH > y + h + 0.5) break;
                cr->set_source_rgb(0.85, 0.85, 0.85);
                cr->move_to(x, ry + 0.5);
                cr->line_to(x + w, ry + 0.5);
                cr->stroke();
                for (std::size_t c = 0; c < r.size() && c < n; ++c)
                    text_in(cr, r[c], x + c * cw, ry, cw, core::kPortalRowH, Pango::Alignment::LEFT, 0.05);
                ry += core::kPortalRowH;
            }
            break;
        }
        }
    }
}

// Page `i`, on a surface whose units are points with (0, 0) the paper's corner.
void draw_page(const Cairo::RefPtr<Cairo::Context>& cr, const PreviewDoc& d, int i) {
    const auto& P = d.paper;
    const auto& pg = d.pages[static_cast<std::size_t>(i)];
    cr->save();
    cr->set_source_rgb(1, 1, 1);
    cr->paint();
    cr->translate(P.margin, P.margin);
    cr->scale(d.scale, d.scale);
    const auto& L = d.layout;
    const int header = core::part_height(L, core::Part::Header);
    const int footer = core::part_height(L, core::Part::Footer);
    static const db::PrintedRecord kNone;
    const db::PrintedRecord& lead = pg.count > 0 ? d.records[static_cast<std::size_t>(d.blocks[static_cast<std::size_t>(pg.first)].at.record)]
                                    : d.records.empty() ? kNone : d.records.front();
    const bool any = !lead.empty() || L.objects.empty();
    if (header > 0 && any) draw_part(cr, d, band_of(L, core::Part::Header), lead, 0);
    double y = header;
    for (int k = 0; k < pg.count; ++k) {
        const auto& b = d.blocks[static_cast<std::size_t>(pg.first + k)];
        if (b.at.part == core::Part::Sub) draw_part(cr, d, band_of(L, core::Part::Sub, b.at.sub), b.sub, y);
        else
            for (int c = 0; c < std::max(1, L.across); ++c) {   // s016: a row of labels (one, for a list)
                const auto r = static_cast<std::size_t>(b.at.record + c);
                if (r >= d.records.size()) break;
                cr->save();
                cr->translate(c * (L.width + L.gap), 0);
                draw_part(cr, d, band_of(L, core::Part::Body), d.records[r], y);
                cr->restore();
            }
        y += b.height;
    }
    if (footer > 0 && any) draw_part(cr, d, band_of(L, core::Part::Footer), lead, d.room - footer);
    cr->restore();
    // The folio, under the content, in the bottom margin's top.
    const double fy = P.height - P.margin - kFolio + 4;
    text_in(cr, core::page_words(i + 1, static_cast<int>(d.pages.size())), P.margin, fy, P.width - 2 * P.margin, kFolio,
            Pango::Alignment::CENTER, 0.5);
    text_in(cr, d.title, P.margin, fy, P.width - 2 * P.margin, kFolio, Pango::Alignment::LEFT, 0.65);
}

}  // namespace

void Shell::paint_preview() {  // preview: the List's layout on pages, read-only
    const db::Table* t = browse_info();
    if (!t || !m_conn) {
        auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        outer->set_name("shell.browse.preview_view");
        m_browse_scroll.set_child(*outer);
        return;
    }
    if (m_busy) { m_preview_again = true; return; }   // s020b: painted again when the read running ends

    auto doc = std::make_shared<PreviewDoc>();
    bool kept = false;
    doc->layout = form_layout(&kept);
    doc->paper = core::layout_paper(doc->layout, default_paper());   // s026a: the layout's page
    doc->scale = core::print_scale(core::labels_width(doc->layout), doc->paper);   // s016: a row of labels
    doc->room = (doc->paper.height - 2 * doc->paper.margin - kFolio) / doc->scale;
    doc->title = t->name + " — " + doc->layout.name;

    // s015b: a sub-summary by a field the table no longer has is left out
    // (the line above the pages says so).
    std::string gone;
    const bool labels = core::is_labels(doc->layout);   // s016: labels print no sub-summaries
    if (labels) while (!doc->layout.subs.empty()) core::remove_sub(doc->layout, static_cast<int>(doc->layout.subs.size()) - 1);
    for (int i = static_cast<int>(doc->layout.subs.size()) - 1; i >= 0; --i)
        if (!doc->layout.subs[static_cast<std::size_t>(i)].by.empty() &&   // s027a: a grand summary has no field to lose
            !t->column(doc->layout.subs[static_cast<std::size_t>(i)].by)) {
            gone = doc->layout.subs[static_cast<std::size_t>(i)].by;
            core::remove_sub(doc->layout, i);
        }
    const auto groups = core::group_fields(doc->layout);

    // The records Browse shows -- every one, or the found ones -- in its order
    // (s015b: grouped first, when the layout has sub-summaries).
    db::PageRequest all = m_browse_req;
    all.offset = 0;
    all.limit = db::kPrintMax;
    all.group = groups;
    // s020b: the reading runs on a worker (Shell_long.cpp) -- what it needs is
    // copied here; t and m_schema stay put while the window is shut.
    const bool found = found_here();
    const std::vector<db::Criterion> crit = found ? m_found : std::vector<db::Criterion>{};
    const auto calcs = m_solution.calcs_of(t->name);
    const auto sums = m_solution.summaries_of(t->name);
    const auto browse_sums = m_browse_sums;
    const std::string table = t->name;
    auto why_p = std::make_shared<std::string>();
    run_long("Reading the records of " + curly(table) + " for Preview",
             [this, t, doc, all, found, crit, calcs, sums, browse_sums, groups, labels, why_p, kept, gone, table](LongTicket& tk) -> std::function<void()> {
    std::string& why = *why_p;
    const auto today = core::today_iso();
    auto page = found ? db::read_found(*m_conn, *t, crit, all, &m_schema) : db::read_page(*m_conn, *t, all);
    if (!page) why = page.error().message;
    else {
        doc->total = static_cast<int>(page->total);
        for (const auto& row : page->rows) {
            if (tk.stop) { why = "stopped"; break; }
            doc->records.push_back(db::print_record(*m_conn, m_schema, *t, doc->layout, row, calcs, browse_sums, today));
            ++tk.count;
        }
    }
    if (page && why.empty()) {
        // s015b: each record's group values, the groups' summaries (one
        // SELECT per level), then what prints in order.
        std::vector<std::vector<std::string>> keys(page->rows.size());
        std::vector<int> at;
        for (const auto& g : groups)
            for (std::size_t k = 0; k < t->columns.size(); ++k) if (t->columns[k].name == g) at.push_back(static_cast<int>(k));
        for (std::size_t r = 0; r < page->rows.size(); ++r)
            for (std::size_t k = 0; k < at.size(); ++k)
                keys[r].push_back(db::cell_text(page->rows[r][static_cast<std::size_t>(at[k])], t->columns[static_cast<std::size_t>(at[k])].type));
        std::vector<db::GroupSums> level(groups.size());
        if (!sums.empty())
            for (std::size_t k = 0; k < groups.size() && why.empty() && !tk.stop; ++k) {
                auto gs = db::read_group_summaries(*m_conn, *t, sums, std::vector<std::string>(groups.begin(), groups.begin() + static_cast<std::ptrdiff_t>(k) + 1),
                                                   crit, &m_schema);
                if (gs) level[k] = std::move(*gs);
                else why = gs.error().message;
            }
        const auto run = labels ? core::label_blocks(doc->layout, static_cast<int>(page->rows.size()))
                                : core::report_blocks(doc->layout, keys);
        for (const auto& b : run) {
            PreviewDoc::Block blk;
            blk.at = b;
            if (b.part == core::Part::Sub) {
                blk.height = doc->layout.subs[static_cast<std::size_t>(b.sub)].height;
                const int lv = core::group_level(doc->layout, b.sub);
                const auto& key = keys[static_cast<std::size_t>(b.record)];
                std::map<std::string, core::CalcValue> gsum;   // every summary: the group's, else empty
                for (const auto& [n, x] : sums) gsum[n] = core::CalcValue{};
                if (doc->layout.subs[static_cast<std::size_t>(b.sub)].by.empty())   // s027a: a grand summary -- the whole found set's
                    for (const auto& [n, v] : browse_sums) gsum[n] = v;
                else if (lv >= 0 && static_cast<std::size_t>(lv) < level.size()) {
                    const std::vector<std::string> prefix(key.begin(), key.begin() + std::min<std::ptrdiff_t>(lv + 1, static_cast<std::ptrdiff_t>(key.size())));
                    if (auto it = level[static_cast<std::size_t>(lv)].find(prefix); it != level[static_cast<std::size_t>(lv)].end())
                        for (const auto& [n, v] : it->second) gsum[n] = v;
                }
                blk.sub = db::print_record(*m_conn, m_schema, *t, doc->layout, page->rows[static_cast<std::size_t>(b.record)], calcs, gsum, today);
            } else {
                blk.height = core::part_height(doc->layout, core::Part::Body) + (labels ? doc->layout.gap : 0);
            }
            doc->blocks.push_back(std::move(blk));
        }
    }
    std::vector<int> heights;
    std::vector<bool> keep;
    for (const auto& b : doc->blocks) {
        heights.push_back(b.height);
        keep.push_back(b.at.part == core::Part::Sub && doc->layout.subs[static_cast<std::size_t>(b.at.sub)].leading);
    }
    doc->pages = core::paginate_blocks(core::part_height(doc->layout, core::Part::Header),
                                       core::part_height(doc->layout, core::Part::Footer), heights, keep, doc->room);
    if (auto lg = log::get(log::Area::Shell))
        lg->info("preview: '{}' {} records -> {} pages ({}, scale {:.3f}){}", doc->title, doc->records.size(), doc->pages.size(),
                 doc->paper.name, doc->scale, why.empty() ? "" : " -- " + why);

    // Back on the UI thread: the pages, if Browse still shows this Preview.
    return [this, doc, why_p, groups, labels, gone, kept, found, table]() {
    const std::string& why = *why_p;
    const db::Table* t = browse_info();
    if (!t || t->name != table || m_browse_view != BrowseView::Preview) { m_preview_again = true; return; }
    m_preview = doc;
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    outer->set_halign(Gtk::Align::START);
    outer->set_name("shell.browse.preview_view");

    form_caption(*outer, *t, doc->layout, kept, {});   // the layout's name and its picker, as Form / List
    // The caption: what this is, and the two ways out of it.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    const int n = static_cast<int>(doc->records.size());
    std::string says = std::to_string(n) +
                       (found ? " found" : "") + (n == 1 ? " record" : " records") + " · " +
                       std::to_string(doc->pages.size()) + (doc->pages.size() == 1 ? " page" : " pages") + " · " +
                       doc->paper.name + (doc->paper.width > doc->paper.height ? " landscape" : "");
    if (doc->total > n) says += " · the first " + std::to_string(n) + " of " + std::to_string(doc->total);
    if (!groups.empty()) {   // s015b
        says += " · grouped by ";
        for (std::size_t k = 0; k < groups.size(); ++k) says += (k ? ", " : "") + curly(groups[k]);
    }
    if (labels) says += " · labels " + core::labels_label(doc->layout);
    if (!gone.empty()) says += " · there's no " + curly(gone) + " now -- its sub-summary is left out";
    if (doc->scale < 1.0) says += " · scaled to " + std::to_string(static_cast<int>(std::lround(doc->scale * 100))) + "%";
    const bool stopped = why == "stopped";   // s020b
    auto cap = Gtk::make_managed<Gtk::Label>(why.empty() ? says
                                             : stopped ? "Stopped before every record was read -- nothing to print. Read Again reads them."
                                                       : "Could not read the records: " + why);
    cap->set_xalign(0.0f);
    cap->add_css_class(why.empty() ? "acorn-status" : "acorn-missing");
    cap->set_name("shell.browse.preview_says");
    cap->set_hexpand(true);
    bar->append(*cap);
    auto pr = Gtk::make_managed<Gtk::Button>("Print…");
    pr->set_name("shell.browse.print");
    pr->set_tooltip_text("Print these pages");
    pr->signal_clicked().connect([this] { print_preview(); });
    auto pdf = Gtk::make_managed<Gtk::Button>("Save as PDF…");
    pdf->set_name("shell.browse.pdf");
    pdf->set_tooltip_text("These pages as a PDF file");
    pdf->signal_clicked().connect([this] { save_preview_pdf(); });
    pr->set_sensitive(why.empty());
    pdf->set_sensitive(why.empty());
    if (stopped) {   // s020b: the way back after Stop
        auto again = Gtk::make_managed<Gtk::Button>("Read Again");
        again->set_name("shell.browse.preview_again");
        again->set_tooltip_text("Read the records for Preview again");
        again->signal_clicked().connect([this] { paint_preview(); });
        bar->append(*again);
    }
    bar->append(*pr);
    bar->append(*pdf);
    outer->append(*bar);

    // s026a: the layout's page -- paper, on its side, margins. A change keeps
    // the layout (one step) and the pages are laid out again.
    {
        auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        page->set_name("shell.browse.page");
        const core::Layout L = doc->layout;
        std::vector<Glib::ustring> pw{"Desktop's (" + default_paper().name + ")"};
        for (const auto& n2 : core::paper_names()) pw.push_back(n2);
        auto paper_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(pw));
        paper_dd->set_name("shell.browse.paper");
        const auto& names = core::paper_names();
        const auto at = std::find(names.begin(), names.end(), L.paper);
        paper_dd->set_selected(at == names.end() ? 0 : static_cast<guint>(at - names.begin()) + 1);
        paper_dd->set_tooltip_text("The paper this layout prints on -- kept with the layout");
        auto side = Gtk::make_managed<Gtk::CheckButton>("Landscape");
        side->set_name("shell.browse.landscape");
        side->set_active(L.landscape);
        std::vector<Glib::ustring> mw{"¼ in margins", "½ in margins", "¾ in margins", "1 in margins"};
        auto margin_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(mw));
        margin_dd->set_name("shell.browse.margin");
        const auto& ms = core::margin_choices();
        const auto mat = std::find(ms.begin(), ms.end(), L.margin);
        margin_dd->set_selected(mat == ms.end() ? 1 : static_cast<guint>(mat - ms.begin()));
        auto keep = [this, L, paper_dd, side, margin_dd] {
            core::Layout want = L;
            const auto& nm = core::paper_names();
            const guint p = paper_dd->get_selected();
            want.paper = p == 0 || p > nm.size() ? std::string{} : nm[p - 1];
            want.landscape = side->get_active();
            const guint m = margin_dd->get_selected();
            want.margin = m < core::margin_choices().size() ? core::margin_choices()[m] : 36;
            if (want == L || !m_journal) return;
            Glib::signal_idle().connect_once([this, want] {
                auto r = db::verbs::save_layout(*m_journal, want);
                if (auto lg = log::get(log::Area::App))
                    lg->info("layout: page of '{}': {}", want.name, r ? m_journal->undo_label() : r.error().message);
                if (!r) { browse_note("Page not kept: " + r.error().message + ".", "error"); return; }
                sync_undo();
                browse_note("Did " + m_journal->undo_label() + " -- " + (want.paper.empty() ? std::string("the desktop's paper")
                            : want.paper) + (want.landscape ? ", landscape" : "") + ". Ctrl+Z undoes it.", "ok");
                paint_preview();
            });
        };
        paper_dd->property_selected().signal_changed().connect(keep);
        side->signal_toggled().connect(keep);
        margin_dd->property_selected().signal_changed().connect(keep);
        page->append(*paper_dd);
        page->append(*side);
        page->append(*margin_dd);
        outer->append(*page);
    }
    if (m_print_when_ready && why.empty()) {   // s026a: Ctrl+P asked for these pages
        m_print_when_ready = false;
        Glib::signal_idle().connect_once([this] { print_preview(); });
    }

    // The pages, one under another, at their size in points.
    for (std::size_t i = 0; i < doc->pages.size(); ++i) {
        auto da = Gtk::make_managed<Gtk::DrawingArea>();
        da->add_css_class("acorn-paper");
        da->set_name("shell.browse.page." + std::to_string(i + 1));
        da->set_content_width(static_cast<int>(std::ceil(doc->paper.width)));
        da->set_content_height(static_cast<int>(std::ceil(doc->paper.height)));
        da->set_halign(Gtk::Align::START);
        std::weak_ptr<PreviewDoc> weak = doc;
        da->set_draw_func([weak, i](const Cairo::RefPtr<Cairo::Context>& cr, int, int) {
            if (auto d = weak.lock()) draw_page(cr, *d, static_cast<int>(i));
        });
        outer->append(*da);
    }
    m_browse_scroll.set_child(*outer);
    };   // the UI part
    });  // the worker
}

void Shell::print_shortcut() {  // preview: s026a -- Ctrl+P: the shown layout's pages, then the print dialog
    if (!m_conn || !browse_info() || m_finding) return;
    if (m_stack.get_visible_child_name() != "browse") m_stack.set_visible_child("browse");
    if (m_browse_view == BrowseView::Preview && m_preview && !m_preview->pages.empty() && !m_busy) { print_preview(); return; }
    m_print_when_ready = true;   // the Preview paint prints once its pages land
    if (m_browse_view == BrowseView::Preview) paint_preview();
    else m_browse_preview_btn.set_active(true);
}

void Shell::print_preview() {  // preview: Print... -- the same pages through GtkPrintOperation
    auto doc = m_preview;
    if (!doc || doc->pages.empty()) return;
    auto op = Gtk::PrintOperation::create();
    op->set_job_name("Acorn -- " + doc->title);
    op->set_n_pages(static_cast<int>(doc->pages.size()));
    op->set_unit(Gtk::Unit::POINTS);
    op->set_use_full_page(true);   // (0, 0) is the paper's corner: the margins are ours
    auto setup = Gtk::PageSetup::create();
    const std::string& pn = doc->paper.name;   // s026a: + Legal, A5, landscape
    setup->set_paper_size(Gtk::PaperSize(pn == "A4" ? "iso_a4" : pn == "Letter" ? "na_letter" : pn == "Legal" ? "na_legal"
                                         : pn == "A5" ? "iso_a5" : Gtk::PaperSize::get_default()));
    setup->set_orientation(doc->paper.width > doc->paper.height ? Gtk::PageOrientation::LANDSCAPE
                                                                : Gtk::PageOrientation::PORTRAIT);
    op->set_default_page_setup(setup);
    op->signal_draw_page().connect([doc](const Glib::RefPtr<Gtk::PrintContext>& ctx, int nr) {
        if (nr >= 0 && static_cast<std::size_t>(nr) < doc->pages.size()) draw_page(ctx->get_cairo_context(), *doc, nr);
    });
    try {
        const auto r = op->run(Gtk::PrintOperation::Action::PRINT_DIALOG, *this);
        if (auto lg = log::get(log::Area::Shell)) lg->info("preview: print '{}' -> {}", doc->title, static_cast<int>(r));
        if (r == Gtk::PrintOperation::Result::APPLY) browse_note("Sent " + std::to_string(doc->pages.size()) + " pages to print.", "ok");
    } catch (const Glib::Error& e) {
        browse_note("Not printed: " + std::string(e.what()) + ".", "error");
    }
}

void Shell::save_preview_pdf() {  // preview: Save as PDF... -- Cairo's PDF surface, the same pages
    auto doc = m_preview;
    if (!doc || doc->pages.empty()) return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Save as PDF");
    std::string base = doc->title;
    for (auto& ch : base) if (ch == '/' || ch == '\\') ch = '-';
    dialog->set_initial_name(base + ".pdf");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto f = Gtk::FileFilter::create();
    f->set_name("PDF documents");
    f->add_pattern("*.pdf");
    filters->append(f);
    dialog->set_filters(filters);
    dialog->save(*this, [this, dialog, doc](const Glib::RefPtr<Gio::AsyncResult>& res) {
        std::string path;
        try {
            auto file = dialog->save_finish(res);
            if (!file) return;
            path = file->get_path();
        } catch (const Glib::Error&) {
            return;   // cancelled
        }
        if (path.size() < 4 || path.substr(path.size() - 4) != ".pdf") path += ".pdf";
        try {
            auto surface = Cairo::PdfSurface::create(path, doc->paper.width, doc->paper.height);
            auto cr = Cairo::Context::create(surface);
            for (std::size_t i = 0; i < doc->pages.size(); ++i) {
                draw_page(cr, *doc, static_cast<int>(i));
                cr->show_page();
            }
            surface->finish();
        } catch (const std::exception& e) {
            browse_note("Not saved: " + std::string(e.what()) + ".", "error");
            return;
        }
        if (auto lg = log::get(log::Area::Shell)) lg->info("preview: pdf '{}' -> {}", doc->title, path);
        browse_note("Saved " + std::to_string(doc->pages.size()) + (doc->pages.size() == 1 ? " page" : " pages") +
                        " as " + std::filesystem::path(path).filename().string() + ".", "ok");
    });
}

}  // namespace acorn
