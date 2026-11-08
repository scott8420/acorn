#include "ImportWindow.hpp"
#include "Registry.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

#include <algorithm>

// ImportWindow.cpp -- see the header. Nothing here writes: Import hands the
// plan to Shell, which runs db::import_records through db::Journal.

namespace acorn {

namespace {
constexpr std::size_t kPreviewRows = 6;    // the file's first lines shown
constexpr int         kCellChars   = 18;   // a cell's width before it is cut short
constexpr std::size_t kListed      = 200;  // refused rows written out in the report

std::string curly(const std::string& s) { return "“" + s + "”"; }

std::string one_line(const std::string& s) {
    std::string out;
    for (char ch : s) out += (ch == '\n' || ch == '\r') ? ' ' : (ch == '\t' ? ' ' : ch);
    return out;
}
}  // namespace

ImportWindow::ImportWindow(Gtk::Window& parent)
    : m_head("import.head"),
      m_header("import.header"),
      m_scroll("import.preview"),
      m_says("import.says"),
      m_reason("import.reason"),
      m_plan_buttons("import.buttons", Gtk::Orientation::HORIZONTAL, 10),
      m_cancel("import.cancel"),
      m_ok("import.go"),
      m_done("import.done"),
      m_refused_scroll("import.refused"),
      m_close("import.close") {
    set_name("import");
    registry::add("import", this);
    set_title("Import Records");
    set_modal(true);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(760, 520);
    set_titlebar(*Gtk::make_managed<Gtk::HeaderBar>());

    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    page->set_margin(18);

    m_head.set_xalign(0.0f);
    m_head.set_wrap(true);
    page->append(m_head);

    m_header.set_label("The first line is field names, not a record");
    m_header.set_tooltip_text("Ticked: the first line's names pick the fields, and it isn't imported");
    m_header.signal_toggled().connect([this] {
        if (m_resetting) return;
        if (m_header.get_active()) {   // the names pick again
            const auto m = db::match_fields(m_data.rows.empty() ? std::vector<std::string>{} : m_data.rows[0], m_table);
            m_resetting = true;
            for (std::size_t i = 0; i < m_picks.size(); ++i) {
                const std::string f = i < m.size() ? m[i] : std::string{};
                const auto at = std::find(m_choices.begin(), m_choices.end(), f);
                m_picks[i]->set_selected(at == m_choices.end() ? 0 : static_cast<guint>(at - m_choices.begin()));
            }
            m_resetting = false;
        }
        refresh();
    });
    page->append(m_header);

    auto* hint = Gtk::make_managed<Gtk::Label>("Pick the field each column goes to -- or “-- skip --”. "
                                               "A field left out gets its default.");
    hint->set_xalign(0.0f);
    hint->set_wrap(true);
    hint->add_css_class("dim-label");
    page->append(*hint);

    m_grid.set_row_spacing(4);
    m_grid.set_column_spacing(12);
    m_grid.set_margin(4);
    m_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_min_content_height(200);
    m_scroll.set_child(m_grid);
    page->append(m_scroll);

    m_says.set_xalign(0.0f);
    m_says.set_wrap(true);
    page->append(m_says);
    m_reason.set_xalign(0.0f);
    m_reason.set_wrap(true);
    m_reason.add_css_class("error");
    page->append(m_reason);

    // The report, after Import (hidden until then).
    m_done.set_xalign(0.0f);
    m_done.set_wrap(true);
    m_done.set_visible(false);
    page->append(m_done);
    m_refused.set_editable(false);
    m_refused.set_cursor_visible(false);
    m_refused.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    m_refused.set_left_margin(6);
    m_refused.set_top_margin(6);
    m_refused.set_name("import.refused_text");
    m_refused_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    m_refused_scroll.set_vexpand(true);
    m_refused_scroll.set_child(m_refused);
    m_refused_scroll.set_visible(false);
    page->append(m_refused_scroll);

    m_plan_buttons.set_halign(Gtk::Align::END);
    m_cancel.set_label("Cancel");
    m_cancel.signal_clicked().connect([this] { set_visible(false); });
    m_ok.set_label("Import");
    m_ok.add_css_class("suggested-action");
    m_ok.set_tooltip_text("Add the records -- Ctrl+Z takes the whole import back in one step");
    m_ok.signal_clicked().connect([this] { on_import(); });
    m_close.set_label("Close");
    m_close.add_css_class("suggested-action");
    m_close.set_visible(false);
    m_close.signal_clicked().connect([this] { set_visible(false); });
    m_plan_buttons.append(m_cancel);
    m_plan_buttons.append(m_ok);
    m_plan_buttons.append(m_close);
    page->append(m_plan_buttons);

    set_child(*page);
    set_default_widget(m_ok);
}

ImportWindow::~ImportWindow() { registry::remove(this); }

void ImportWindow::start(const db::Table& t, const std::string& file_name, db::Delimited d, Import import) {
    m_table = t;
    m_file = file_name;
    m_data = std::move(d);
    m_import = std::move(import);
    m_choices = {""};
    for (const auto& f : db::import_fields(t)) m_choices.push_back(f);
    const auto first = m_data.rows.empty() ? std::vector<std::string>{} : m_data.rows[0];
    const bool names = db::looks_like_header(first, t);
    m_resetting = true;
    m_header.set_active(names);
    m_resetting = false;
    std::size_t lines = 0;
    for (const auto& r : m_data.rows) if (!r.empty()) ++lines;
    m_head.set_text(m_file + " -- " + std::to_string(lines) + (lines == 1 ? " line, " : " lines, ") +
                    std::to_string(m_data.width()) + (m_data.width() == 1 ? " column" : " columns") + ", " +
                    (m_data.sep == '\t' ? "tab-separated" : "comma-separated") + " -- into " + curly(t.name));
    build_grid();
    // Plan mode.
    m_header.set_visible(true);
    m_scroll.set_visible(true);
    m_says.set_visible(true);
    m_reason.set_visible(true);
    m_done.set_visible(false);
    m_refused_scroll.set_visible(false);
    m_cancel.set_visible(true);
    m_ok.set_visible(true);
    m_close.set_visible(false);
    set_default_widget(m_ok);
    refresh();
    present();
}

void ImportWindow::build_grid() {
    while (auto* c = m_grid.get_first_child()) m_grid.remove(*c);
    m_picks.clear();
    m_first.clear();
    const std::size_t w = m_data.width();
    const auto first = m_data.rows.empty() ? std::vector<std::string>{} : m_data.rows[0];
    const auto matched = db::match_fields(first, m_table);
    auto names = Gtk::StringList::create({"-- skip --"});
    for (std::size_t k = 1; k < m_choices.size(); ++k) {
        const db::Column* c = m_table.column(m_choices[k]);
        names->append(m_choices[k] + (c && c->auto_increment ? " (counts up by itself)" : ""));
    }
    for (std::size_t i = 0; i < w; ++i) {
        auto dd = Gtk::make_managed<Gtk::DropDown>(names);
        dd->set_name("import.column." + std::to_string(i + 1));
        dd->set_tooltip_text("Column " + std::to_string(i + 1) + " goes to this field");
        const std::string f = m_header.get_active() && i < matched.size() ? matched[i] : std::string{};
        const auto at = std::find(m_choices.begin(), m_choices.end(), f);
        dd->set_selected(at == m_choices.end() ? 0 : static_cast<guint>(at - m_choices.begin()));
        dd->property_selected().signal_changed().connect([this] { if (!m_resetting) refresh(); });
        m_grid.attach(*dd, static_cast<int>(i), 0);
        m_picks.push_back(dd);
    }
    std::size_t shown = 0;
    for (std::size_t r = 0; r < m_data.rows.size() && shown < kPreviewRows; ++r) {
        if (m_data.rows[r].empty()) continue;
        for (std::size_t i = 0; i < w; ++i) {
            const std::string v = i < m_data.rows[r].size() ? m_data.rows[r][i] : std::string{};
            auto l = Gtk::make_managed<Gtk::Label>(one_line(v));
            l->set_xalign(0.0f);
            l->set_ellipsize(Pango::EllipsizeMode::END);
            l->set_max_width_chars(kCellChars);
            l->set_width_chars(std::min<int>(kCellChars, std::max<int>(6, static_cast<int>(v.size()))));
            if (v.size() > static_cast<std::size_t>(kCellChars) || v.find('\n') != std::string::npos) l->set_tooltip_text(v);
            m_grid.attach(*l, static_cast<int>(i), static_cast<int>(shown) + 1);
            if (r == 0) m_first.push_back(l);
        }
        ++shown;
    }
}

db::ImportPlan ImportWindow::plan() const {
    db::ImportPlan p;
    p.header = m_header.get_active();
    for (auto* dd : m_picks) {
        const guint k = dd->get_selected();
        p.fields.push_back(k < m_choices.size() ? m_choices[k] : std::string{});
    }
    return p;
}

void ImportWindow::refresh() {
    for (auto* w : m_first) {
        if (m_header.get_active()) w->add_css_class("acorn-field-name");
        else w->remove_css_class("acorn-field-name");
    }
    const auto p = plan();
    const std::string why = db::check_import(m_table, p);
    m_reason.set_text(why.empty() ? "" : why + ".");
    if (!why.empty()) {
        m_says.set_text("");
        m_ok.set_sensitive(false);
        return;
    }
    const auto pv = db::import_preview(m_table, m_data, p);
    std::string s = "Will add " + std::to_string(pv.records) + (pv.records == 1 ? " record" : " records") + " to " + curly(m_table.name);
    std::size_t skipped = 0;
    for (const auto& f : p.fields) if (f.empty()) ++skipped;
    if (skipped) s += " (" + std::to_string(skipped) + (skipped == 1 ? " column" : " columns") + " skipped)";
    if (!pv.refused.empty())
        s += ". " + std::to_string(pv.refused.size()) + (pv.refused.size() == 1 ? " row can't" : " rows can't") +
             " go in as they are -- the first, " + db::refusal_says(pv.refused[0]) + ". They'll be listed after";
    m_says.set_text(s + ".");
    m_ok.set_sensitive(pv.records > 0);
}

void ImportWindow::on_import() {
    if (!m_import || !m_ok.get_sensitive()) return;
    auto r = m_import(plan(), m_data);
    if (!r) {
        m_reason.set_text("Not imported: " + r.error().message + ".");
        return;
    }
    // The report.
    m_header.set_visible(false);
    m_scroll.set_visible(false);
    m_says.set_visible(false);
    m_reason.set_visible(false);
    m_cancel.set_visible(false);
    m_ok.set_visible(false);
    m_close.set_visible(true);
    set_default_widget(m_close);
    std::string head = db::import_says(*r, m_table.name) + ".";
    if (r->records > 0) head += " Ctrl+Z takes the whole import back.";
    if (!r->refused.empty()) head += "\nThese rows weren't imported -- nothing of them went in:";
    m_done.set_text(head);
    m_done.set_visible(true);
    std::string list;
    for (std::size_t i = 0; i < r->refused.size() && i < kListed; ++i) list += db::refusal_says(r->refused[i]) + "\n";
    if (r->refused.size() > kListed) list += "… and " + std::to_string(r->refused.size() - kListed) + " more\n";
    m_refused.get_buffer()->set_text(list);
    m_refused_scroll.set_visible(!r->refused.empty());
    m_close.grab_focus();
}

}  // namespace acorn
