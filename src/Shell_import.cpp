#include "Shell.hpp"
#include "ImportWindow.hpp"
#include "Log.hpp"
#include "db/Import.hpp"

#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <giomm/liststore.h>

#include <filesystem>
#include <fstream>
#include <iterator>

// ─────────────────────────────────────────────────────────────────────────────
// Shell_import.cpp -- Browse -> Import… (s018b). GTK's open dialog; the file
// read whole and split (db::read_delimited); ImportWindow shows its first rows
// and the column -> field matches; Import runs db::import_records through the
// journal -- every record a write through the door, the whole import ONE step
// on Ctrl+Z -- and Browse shows the table again.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

namespace {
constexpr std::uintmax_t kImportMaxBytes = 64ull * 1024 * 1024;   // read whole: a bigger file is refused in words
}  // namespace

void Shell::import_records_from_file() {  // import: the open dialog, then the file read and ImportWindow
    const db::Table* t = browse_info();
    if (!t || !m_conn || !browse_writable()) return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import Records into “" + t->name + "”");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto both = Gtk::FileFilter::create();
    both->set_name("CSV or tab-separated (.csv, .tsv, .txt)");
    for (const char* p : {"*.csv", "*.CSV", "*.tsv", "*.TSV", "*.tab", "*.txt"}) both->add_pattern(p);
    filters->append(both);
    auto any = Gtk::FileFilter::create();
    any->set_name("Every file");
    any->add_pattern("*");
    filters->append(any);
    dialog->set_filters(filters);
    const std::string table = t->name;
    dialog->open(*this, [this, dialog, table](const Glib::RefPtr<Gio::AsyncResult>& res) {
        std::string path;
        try {
            auto file = dialog->open_finish(res);
            if (!file) return;
            path = file->get_path();
        } catch (const Glib::Error&) {
            return;   // cancelled
        }
        const db::Table* t = browse_info();
        if (!t || t->name != table || !m_conn || !browse_writable()) {
            browse_note("Not imported: the table changed while the dialog was open.", "error");
            return;
        }
        const std::string name = std::filesystem::path(path).filename().string();
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) { browse_note("Not imported: can't read " + name + ".", "error"); return; }
        if (size > kImportMaxBytes) {
            browse_note("Not imported: " + name + " is over 64 MB -- Acorn reads an import whole.", "error");
            return;
        }
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) { browse_note("Not imported: can't read " + name + ".", "error"); return; }
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        auto d = db::read_delimited(text, db::guess_separator(text, name));
        if (auto lg = log::get(log::Area::Shell))
            lg->info("import: '{}' <- {}: {}", table, path, d ? std::to_string(d->rows.size()) + " rows" : d.error());
        if (!d) { browse_note("Not imported: " + name + ", " + d.error() + ".", "error"); return; }
        if (d->rows.empty() || d->width() == 0) { browse_note("Not imported: " + name + " is empty.", "error"); return; }
        if (!m_import_win) m_import_win = std::make_unique<ImportWindow>(*this);
        m_import_win->start(*t, name, std::move(*d),
            [this, table](const db::ImportPlan& p, const db::Delimited& data) -> std::expected<db::ImportResult, db::Error> {
                const db::Table* t = browse_info();
                if (!t || t->name != table || !m_journal)
                    return std::unexpected(db::Error{db::Error::Code::Changed, "the table Browse shows changed"});
                if (m_script_run) return std::unexpected(db::Error{db::Error::Code::Unsupported, "a script is running -- let it finish first"});
                auto r = db::import_records(*m_journal, *t, data, p);
                if (auto lg = log::get(log::Area::App))
                    lg->info("journal: import into '{}': {}", table,
                             r ? std::to_string(r->records) + " made, " + std::to_string(r->refused.size()) + " refused" : r.error().message);
                if (!r) return r;
                browse_note(db::import_says(*r, table) + (r->records > 0 ? ". Ctrl+Z takes it back." : "."),
                            r->refused.empty() ? "ok" : "error");
                Glib::signal_idle().connect_once([this] { load_browse_page(); });
                return r;
            });
    });
}

}  // namespace acorn
