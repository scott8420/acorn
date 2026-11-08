#include "Shell.hpp"
#include "Log.hpp"
#include "db/Export.hpp"

#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/stringlist.h>
#include <giomm/liststore.h>

#include <filesystem>

// ─────────────────────────────────────────────────────────────────────────────
// Shell_export.cpp -- Browse -> Export (s018a). A popover: which fields (the
// shown layout's, in reading order, or every field of the table) and the
// format (comma- / tab-separated); Export… asks where, then db::export_file
// writes the records Browse shows -- every one or the found set, in its sort.
// Reads only (SELECTs through the Gate); the file is the only thing written.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

namespace {
std::string curly(const std::string& s) { return "“" + s + "”"; }
}  // namespace

void Shell::fill_export_menu() {  // export: the popover -- which fields, which format, Export…
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_name("shell.browse.export.box");
    box->set_margin(8);
    const db::Table* t = browse_info();
    if (!t || !m_conn) {
        box->append(*Gtk::make_managed<Gtk::Label>("Nothing to export."));
        m_browse_export_pop.set_child(*box);
        return;
    }
    const auto& calcs = m_solution.calcs_of(t->name);
    const core::Layout L = form_layout();
    const auto lf = db::layout_export_fields(L, *t, calcs);
    const auto tf = db::table_export_fields(*t, calcs);
    const std::int64_t n = m_browse_data.total;   // the found set's count when one stands
    auto head = Gtk::make_managed<Gtk::Label>((found_here() ? "The " + std::to_string(n) + " found " : "All " + std::to_string(n) + " ") +
                                              (n == 1 ? "record" : "records") + " of " + curly(t->name) +
                                              (m_browse_req.sort.empty() ? "" : ", sorted by " + curly(m_browse_req.sort)));
    head->set_xalign(0.0f);
    head->add_css_class("acorn-field-name");
    box->append(*head);
    auto fdd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(
        {"The layout's fields -- " + curly(L.name) + " (" + std::to_string(lf.size()) + ")",
         "Every field of " + curly(t->name) + " (" + std::to_string(tf.size()) + ")"}));
    fdd->set_name("shell.browse.export.fields");
    fdd->set_tooltip_text("The layout's: its field boxes top to bottom, left to right. Summaries, related fields, portals "
                          "and binary fields are left out; calculations are worked out per record");
    fdd->set_selected(m_export_every || lf.empty() ? 1 : 0);
    auto wdd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(
        {db::export_format_label(db::ExportFormat::Csv), db::export_format_label(db::ExportFormat::Tab)}));
    wdd->set_name("shell.browse.export.format");
    wdd->set_tooltip_text("UTF-8, a header row of the field names, quotes only where a value needs them");
    wdd->set_selected(m_export_tab ? 1 : 0);
    fdd->property_selected().signal_changed().connect([this, fdd] { m_export_every = fdd->get_selected() == 1; });
    wdd->property_selected().signal_changed().connect([this, wdd] { m_export_tab = wdd->get_selected() == 1; });
    box->append(*fdd);
    box->append(*wdd);
    auto go = Gtk::make_managed<Gtk::Button>("Export…");
    go->add_css_class("acorn-primary");
    go->set_name("shell.browse.export.go");
    go->set_tooltip_text("Pick where the file goes -- nothing in the database changes");
    go->set_sensitive(n > 0);
    go->signal_clicked().connect([this] {
        m_browse_export_pop.popdown();
        export_records_to_file();
    });
    box->append(*go);
    m_browse_export_pop.set_child(*box);
}

void Shell::export_records_to_file() {  // export: the save dialog, then db::export_file, the note
    const db::Table* t = browse_info();
    if (!t || !m_conn) return;
    const auto fmt = m_export_tab ? db::ExportFormat::Tab : db::ExportFormat::Csv;
    db::ExportRequest r;
    r.calcs = m_solution.calcs_of(t->name);
    r.fields = m_export_every ? db::table_export_fields(*t, r.calcs) : db::layout_export_fields(form_layout(), *t, r.calcs);
    if (r.fields.empty()) r.fields = db::table_export_fields(*t, r.calcs);   // a layout with no field boxes: every field
    if (found_here()) r.found = m_found;
    r.sort = m_browse_req.sort;
    r.descending = m_browse_req.descending;
    r.format = fmt;
    r.sums = m_browse_sums;
    r.today = core::today_iso();
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export Records");
    std::string base = t->name;
    for (auto& ch : base) if (ch == '/' || ch == '\\') ch = '-';
    const std::string ending = db::export_ending(fmt);
    dialog->set_initial_name(base + ending);
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto f = Gtk::FileFilter::create();
    f->set_name(db::export_format_label(fmt));
    f->add_pattern("*" + ending);
    filters->append(f);
    dialog->set_filters(filters);
    const std::string table = t->name;
    dialog->save(*this, [this, dialog, r, ending, table](const Glib::RefPtr<Gio::AsyncResult>& res) {
        std::string path;
        try {
            auto file = dialog->save_finish(res);
            if (!file) return;
            path = file->get_path();
        } catch (const Glib::Error&) {
            return;   // cancelled
        }
        const db::Table* t = browse_info();
        if (!t || t->name != table || !m_conn) { browse_note("Not exported: the table changed while the dialog was open.", "error"); return; }
        if (path.size() < ending.size() || path.substr(path.size() - ending.size()) != ending) {
            const std::string ext = std::filesystem::path(path).extension().string();
            if (ext.empty()) path += ending;   // a name with no ending gets one; another ending is the person's
        }
        // s020b: written on a worker (Shell_long.cpp); Stop between reads leaves
        // the file that was there.
        const std::string file = std::filesystem::path(path).filename().string();
        const bool started = run_long("Exporting “" + table + "” to " + file,
            [this, t, r, path, table, file](LongTicket& tk) mutable -> std::function<void()> {
                r.keep_going = [&tk](std::int64_t n) { tk.count = n; return !tk.stop.load(); };
                auto done = db::export_file(*m_conn, m_schema, *t, r, path);
                if (auto lg = log::get(log::Area::Shell))
                    lg->info("export: '{}' {} fields -> {}: {}", table, r.fields.size(), path, done ? "ok" : done.error().message);
                return [this, done, file]() {
                    if (!done && done.error().message == "stopped") {
                        browse_note("Stopped -- " + file + " wasn't written (one already there is left as it was).", "error");
                        return;
                    }
                    if (!done) { browse_note("Not exported: " + done.error().message + ".", "error"); return; }
                    browse_note(db::export_says(*done, file) + ".", "ok");
                };
            });
        if (!started) browse_note("Not exported: Acorn is still reading -- try again when it's done.", "error");
    });
}

}  // namespace acorn
