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
            [this, table, name](const db::ImportPlan& p, const db::Delimited& data, ImportWindow::Done done) {
                using Fail = std::unexpected<db::Error>;
                const db::Table* t = browse_info();
                if (!t || t->name != table || !m_journal) {
                    done(Fail(db::Error{db::Error::Code::Changed, "the table Browse shows changed"}));
                    return;
                }
                if (m_script_run) {
                    done(Fail(db::Error{db::Error::Code::Unsupported, "a script is running -- let it finish first"}));
                    return;
                }
                // s020c: on the worker (Shell_long.cpp) -- every row a write
                // through the door, one batch, one step. `data` is the
                // window's own: nothing can start() it again while shut.
                const bool started = run_long("Importing " + name + " into “" + table + "”",
                    [this, tc = *t, p, &data, done, table](LongTicket& tk) -> std::function<void()> {
                        std::expected<db::ImportResult, db::Error> r = std::unexpected(db::Error{db::Error::Code::Internal, "not run"});
                        try {   // the window waits for `done` -- it must come back whatever happens
                            r = db::import_records(*m_journal, tc, data, p,
                                                   [&tk](std::int64_t n) { tk.count = n; return !tk.stop.load(); });
                        } catch (const std::exception& e) {
                            if (m_journal->in_batch()) (void)m_journal->cancel_batch();
                            r = std::unexpected(db::Error{db::Error::Code::Internal, e.what()});
                        }
                        if (auto lg = log::get(log::Area::App))
                            lg->info("journal: import into '{}': {}{}", table,
                                     r ? std::to_string(r->records) + " made, " + std::to_string(r->refused.size()) + " refused"
                                       : r.error().message,
                                     r && r->stopped_at ? ", stopped before line " + std::to_string(r->stopped_at) : "");
                        return [this, r, done, table]() {
                            if (r)
                                browse_note(db::import_says(*r, table) + (r->records > 0 || r->updated > 0 ? ". Ctrl+Z takes it back." : "."),
                                            r->refused.empty() && !r->stopped_at ? "ok" : "error");
                            if (r) load_browse_page();
                            done(r);
                        };
                    });
                if (!started) {
                    done(Fail(db::Error{db::Error::Code::Unsupported, "Acorn is still reading -- try again when it's done"}));
                    return;
                }
                m_import_win->running([this] { return m_long_ticket ? m_long_ticket->count.load() : std::int64_t{0}; },
                                      [this] { stop_long(); });
            });
    });
}

}  // namespace acorn
