#include "Shell.hpp"
#include "Log.hpp"

#include "db/NewFile.hpp"

#include <filesystem>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

// Shell_sample.cpp -- the SAMPLE category (s010f). Acorn's first look isn't an
// empty window: at startup it opens the sample Address Book -- twelve made-up
// people, their addresses and phones, two links, a kept "Contact Card"
// layout -- so Browse, the Graph and Layouts all have something to show.
//
// The sample lives in Acorn's own folder, never beside a database of yours.
// It is made the first time (db::new_file + db::make_sample, through the door,
// the journal cleared after -- it isn't your step to undo) and then simply
// opened again: what you change in it stays. Delete the file to get a fresh
// one. Menu -> "Open the Sample at Startup" turns the startup open off.

namespace acorn {

std::string Shell::sample_file() const {  // sample: in Acorn's own data folder
    return (std::filesystem::path(Glib::get_user_data_dir()) / "acorn" / "sample" / "Sample Address Book.sqlite").string();
}

std::string Shell::startup_file() const {  // sample: ~/.config/acorn/startup.json
    return (std::filesystem::path(Glib::get_user_config_dir()) / "acorn" / "startup.json").string();
}

void Shell::bind_startup() {  // sample: the check box, the saved choice, the startup open
    m_startup = core::load_startup(startup_file());
    add_action("open-sample", sigc::mem_fun(*this, &Shell::on_open_sample));
    m_startup_action = add_action_bool("sample-at-startup", [this] {
        m_startup.open_sample = !m_startup.open_sample;
        m_startup_action->change_state(m_startup.open_sample);
        if (!core::save_startup(startup_file(), m_startup))
            if (auto lg = log::get(log::Area::Shell)) lg->warn("startup: could not save {}", startup_file());
    }, m_startup.open_sample);
    if (m_startup.open_sample) Glib::signal_idle().connect_once([this] { on_open_sample(); });
}

void Shell::on_open_sample() {  // sample: make it if missing, then the normal connect
    if (m_worker.joinable()) return;   // a connect is already under way
    db::Engine* e = nullptr;
    for (std::size_t i = 0; i < m_engines.size(); ++i)
        if (!m_engines[i]->caps().needs_server) { e = m_engines[i]; m_engine_pick.set_selected(i); break; }
    if (!e) { set_status("No file engine (SQLite) is installed -- the sample needs it.", "error"); return; }
    const std::filesystem::path path = sample_file();
    bool fresh = false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path.parent_path(), ec);
        auto made = db::new_file(*e, path);
        if (auto lg = log::get(log::Area::App))
            lg->info("sample: {}: {}", path.string(), made ? "created" : made.error().message);
        if (!made) { set_status("The sample wasn't made: " + made.error().message + ".", "error"); return; }
        fresh = true;
    }
    m_path.set_text(path.string());
    m_pending_starter = fresh;   // filled once it is open read-write
    m_pending_sample = true;
    on_connect();   // the normal open: Acorn made it, so it opens read-write behind the gate
}

}  // namespace acorn
