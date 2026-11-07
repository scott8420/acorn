#include "Shell.hpp"
#include "Log.hpp"
#include "db/NewFile.hpp"

#include <giomm/liststore.h>
#include <algorithm>
#include <filesystem>
#include <utility>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

// Shell_connect.cpp -- the CONNECT area (s004). Pick an engine, point it at a
// file or a server, connect READ-ONLY (RULES: the data is sacred -- nothing
// writes until s005's door), and read the schema on a worker so the window
// never waits on a slow server.
//
// Threading: the worker opens the connection and introspects, parks the
// results under m_result_mx and emits m_connect_done; on_connect_done runs on
// the UI thread, joins, and takes ownership. The UI never touches a connection
// the worker is still using -- the Connect button is disabled while one runs.

namespace acorn {

namespace {

Gtk::Label* field_label(const char* text) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(1.0f);
    l->add_css_class("acorn-field-label");
    return l;
}

std::string summary(const db::Table& t) {
    std::size_t pk = 0;
    for (auto& c : t.columns) pk += c.pk_position > 0;
    std::string s = std::to_string(t.columns.size()) + " column" + (t.columns.size() == 1 ? "" : "s");
    if (pk) s += " · key: " + [&] {
        std::string k;
        for (auto& n : t.primary_key()) k += (k.empty() ? "" : ", ") + n;
        return k;
    }();
    if (!t.foreign_keys.empty())
        s += " · " + std::to_string(t.foreign_keys.size()) + " link" +
             (t.foreign_keys.size() == 1 ? "" : "s");
    if (!t.indexes.empty())
        s += " · " + std::to_string(t.indexes.size()) + " index" +
             (t.indexes.size() == 1 ? "" : "es");
    return s;
}

}  // namespace

void Shell::build_connect_page() {  // connect: the Connect area
    m_connect_page.set_margin(28);

    // Hero: emblem + title.
    auto hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    auto emblem = Gtk::make_managed<Gtk::Image>();
    emblem->set_from_icon_name("acorn-logo-symbolic");
    emblem->set_pixel_size(56);
    emblem->add_css_class("acorn-hero-icon");
    auto titles = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    auto title = Gtk::make_managed<Gtk::Label>("Connect");
    title->set_xalign(0.0f);
    title->add_css_class("acorn-hero-title");
    auto sub = Gtk::make_managed<Gtk::Label>(
        "Open a database to look around. Acorn reads only, for now -- nothing is changed.");
    sub->set_xalign(0.0f);
    sub->set_wrap(true);
    sub->add_css_class("acorn-hero-sub");
    titles->append(*title);
    titles->append(*sub);
    hero->append(*emblem);
    hero->append(*titles);
    m_connect_page.append(*hero);

    // The engines this machine can actually run.
    m_loader.scan(db::Loader::default_dirs());
    for (auto& r : m_loader.rejected())
        if (auto lg = log::get(log::Area::App))
            lg->warn("engine not loaded: {} -- {}", r.file.string(), r.reason);
    m_engines = m_loader.engines();
    // SQLite first: it is the default for new solutions and needs no server.
    std::stable_sort(m_engines.begin(), m_engines.end(), [](db::Engine* a, db::Engine* b) {
        return (a->id() == "sqlite") > (b->id() == "sqlite");
    });
    m_engine_names = Gtk::StringList::create({});
    for (auto* e : m_engines) m_engine_names->append(e->name());
    m_engine_pick.set_model(m_engine_names);
    m_engine_pick.set_name("shell.connect.engine");
    if (auto lg = log::get(log::Area::App)) lg->info("{} engine(s) loaded", m_engines.size());

    // The form, on a calm panel.
    auto panel = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    panel->add_css_class("acorn-panel");
    panel->set_size_request(480, -1);
    panel->set_hexpand(false);   // explicit: its entries would otherwise propagate expand

    auto engine_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto el = field_label("Engine");
    el->set_size_request(90, -1);
    m_engine_pick.set_hexpand(true);
    engine_row->append(*el);
    engine_row->append(m_engine_pick);
    panel->append(*engine_row);

    // File engines: a path + Open...
    auto file_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto fl = field_label("File");
    fl->set_size_request(90, -1);
    m_path.set_hexpand(true);
    m_path.set_placeholder_text("/path/to/database.sqlite");
    m_path.signal_activate().connect(sigc::mem_fun(*this, &Shell::on_connect));
    m_browse.set_label("Open…");
    m_browse.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_browse_file));
    file_row->append(*fl);
    file_row->append(m_path);
    file_row->append(m_browse);
    m_new_file.set_label("New…");
    m_new_file.set_tooltip_text("Make a new, empty SQLite database file (Ctrl+N)");
    m_new_file.set_action_name("win.new-file");
    file_row->append(m_new_file);
    m_file_rows.append(*file_row);
    panel->append(m_file_rows);

    // Server engines: host / port / database / user / password.
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(10);
    int row = 0;
    const auto add = [&](const char* label, Gtk::Entry& e, const char* hint) {
        auto l = field_label(label);
        l->set_size_request(90, -1);
        e.set_hexpand(true);
        e.set_placeholder_text(hint);
        e.signal_activate().connect(sigc::mem_fun(*this, &Shell::on_connect));
        grid->attach(*l, 0, row);
        grid->attach(e, 1, row);
        ++row;
    };
    add("Host", m_host, "localhost");
    add("Port", m_port, "default");
    add("Database", m_database, "name");
    add("User", m_user, "user name");
    add("Password", m_password, "from the keyring, if remembered");
    m_password.set_visibility(false);
    m_password.set_input_purpose(Gtk::InputPurpose::PASSWORD);
    m_remember.set_label("Remember the password in the GNOME keyring");
    m_remember.set_active(true);
    grid->attach(m_remember, 1, row);
    m_server_rows.append(*grid);
    panel->append(m_server_rows);

    auto actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    m_connect.set_label("Connect");
    m_connect.add_css_class("acorn-primary");
    m_connect.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_connect));
    auto badge = Gtk::make_managed<Gtk::Label>("READ-ONLY");
    badge->add_css_class("acorn-badge");
    badge->set_valign(Gtk::Align::CENTER);
    actions->append(m_connect);
    actions->append(*badge);
    panel->append(*actions);

    m_status.set_xalign(0.0f);
    m_status.set_wrap(true);
    m_status.add_css_class("acorn-status");
    panel->append(m_status);

    // Recent connections, beside the form (s004b). A click connects again;
    // the x forgets it (and its stored password).
    auto recent = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    recent->add_css_class("acorn-panel");
    recent->set_hexpand(true);
    recent->set_size_request(240, -1);
    auto rt = Gtk::make_managed<Gtk::Label>("Recent");
    rt->set_xalign(0.0f);
    rt->add_css_class("acorn-field-label");
    auto rscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    rscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    rscroll->set_vexpand(true);
    rscroll->set_min_content_height(120);
    m_recent_list.set_selection_mode(Gtk::SelectionMode::NONE);
    m_recent_list.set_activate_on_single_click(true);
    m_recent_list.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        const int i = row ? row->get_index() : -1;
        if (i >= 0 && static_cast<std::size_t>(i) < m_recents.size())
            on_recent_open(m_recents[i].key());
    });
    rscroll->set_child(m_recent_list);
    recent->append(*rt);
    recent->append(*rscroll);

    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    top->append(*panel);
    top->append(*recent);
    m_connect_page.append(*top);

    // What we found: tables and views, on their own panel.
    auto found = Gtk::make_managed<Gtk::ScrolledWindow>();
    found->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    found->set_vexpand(true);
    found->add_css_class("acorn-panel");
    m_tables.set_selection_mode(Gtk::SelectionMode::NONE);
    m_tables.set_activate_on_single_click(true);
    m_tables.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {   // s004c
        const int i = row ? row->get_index() : -1;
        if (i >= 0 && static_cast<std::size_t>(i) < m_schema.tables.size())
            open_table(m_schema.tables[i].name);
    });
    found->set_child(m_tables);
    m_connect_page.append(*found);

    m_engine_pick.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &Shell::on_engine_changed));
    m_connect_done.connect(sigc::mem_fun(*this, &Shell::on_connect_done));

    if (m_engines.empty()) {
        m_connect.set_sensitive(false);
        set_status("No database engines were found. Reinstall Acorn, or check the log.", "error");
    }
    on_engine_changed();
    show_schema();

    m_stack.add(m_connect_page, "connect", "Connect");
}

void Shell::on_engine_changed() {  // connect: file vs server rows
    const auto i = m_engine_pick.get_selected();
    db::Engine* e = i < m_engines.size() ? m_engines[i] : nullptr;
    const bool server = e && e->caps().needs_server;
    m_file_rows.set_visible(!server);
    m_server_rows.set_visible(server);
    if (e && server)
        m_port.set_placeholder_text(e->dialect() == db::Dialect::MariaDB ? "3306" : "5432");
}

void Shell::on_new_file() {  // connect: Ctrl+N -- make an empty SQLite file, open it
    // s005a. db::new_file is the one place Acorn creates a database, and it
    // refuses ANY existing path -- even if the save dialog's own "Replace?"
    // was answered yes. Nothing existing is ever overwritten.
    db::Engine* e = nullptr;
    for (std::size_t i = 0; i < m_engines.size(); ++i)
        if (!m_engines[i]->caps().needs_server) { e = m_engines[i]; m_engine_pick.set_selected(i); break; }
    m_stack.set_visible_child("connect");
    if (!e) { set_status("No file engine (SQLite) is installed.", "error"); return; }

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("New database file");
    dialog->set_initial_name("untitled.sqlite");
    dialog->set_accept_label("Create");
    dialog->save(*this, [this, dialog, e](const Glib::RefPtr<Gio::AsyncResult>& res) {
        std::filesystem::path path;
        try {
            auto f = dialog->save_finish(res);
            if (!f) return;
            path = f->get_path();
        } catch (const Glib::Error&) {
            return;   // cancelled
        }
        if (path.extension().empty()) path += ".sqlite";
        auto made = db::new_file(*e, path);
        if (auto lg = log::get(log::Area::App))
            lg->info("new file {}: {}", path.string(), made ? "created" : made.error().message);
        if (!made) {
            set_status("Not created: " + made.error().message +
                       (made.error().code == db::Error::Code::Exists ? ". Use Open… to look at it." : ""),
                       "error");
            return;
        }
        m_path.set_text(path.string());
        on_connect();   // the normal read-only open; it becomes a recent
    });
}

void Shell::on_browse_file() {  // connect: pick an SQLite file
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open a database");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto lite = Gtk::FileFilter::create();
    lite->set_name("SQLite databases");
    for (auto p : {"*.sqlite", "*.sqlite3", "*.db", "*.db3"}) lite->add_pattern(p);
    auto all = Gtk::FileFilter::create();
    all->set_name("All files");
    all->add_pattern("*");
    filters->append(lite);
    filters->append(all);
    dialog->set_filters(filters);
    dialog->open(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& res) {
        try {
            if (auto f = dialog->open_finish(res)) {
                m_path.set_text(f->get_path());
                on_connect();
            }
        } catch (const Glib::Error&) {
            // Cancelled or dismissed -- nothing to do.
        }
    });
}

void Shell::on_open_file() {  // connect: Ctrl+O -- SQLite + the file dialog
    for (std::size_t i = 0; i < m_engines.size(); ++i)
        if (!m_engines[i]->caps().needs_server) { m_engine_pick.set_selected(i); break; }
    m_stack.set_visible_child("connect");
    on_browse_file();
}

db::Engine* Shell::engine_by_id(const std::string& id) const {  // connect
    for (auto* e : m_engines)
        if (e->id() == id) return e;
    return nullptr;
}

void Shell::fill_form(const core::SavedConnection& c) {  // connect: a recent -> the form
    for (std::size_t i = 0; i < m_engines.size(); ++i)
        if (m_engines[i]->id() == c.engine) { m_engine_pick.set_selected(i); break; }
    if (c.is_file()) {
        m_path.set_text(c.path);
    } else {
        m_host.set_text(c.host);
        m_port.set_text(c.port > 0 ? std::to_string(c.port) : "");
        m_database.set_text(c.database);
        m_user.set_text(c.user);
        m_password.set_text("");   // the keyring supplies it
    }
}

void Shell::on_connect() {  // connect: read the form -> keyring -> start
    if (m_worker.joinable()) return;                 // one at a time
    const auto i = m_engine_pick.get_selected();
    if (i >= m_engines.size()) return;
    db::Engine* e = m_engines[i];

    db::ConnectParams p;                             // read_only = true by default
    core::SavedConnection saved;
    saved.engine = e->id();
    if (e->caps().needs_server) {
        p.host = m_host.get_text();
        p.port = std::atoi(m_port.get_text().c_str());
        p.database = m_database.get_text();
        p.user = m_user.get_text();
        p.password = m_password.get_text();
        m_password.set_text("");                     // never left in the form
        saved.host = p.host;
        saved.port = p.port;
        saved.database = p.database;
        saved.user = p.user;
    } else {
        p.path = m_path.get_text();
        if (p.path.empty()) { set_status("Choose a database file first.", "error"); return; }
        saved.path = p.path;
    }
    m_conn_label = e->name() + " — " + saved.label();
    m_pending = saved;
    m_pending_remember = e->caps().needs_server && m_remember.get_active();
    m_pending_password = p.password;
    m_pending_from_keyring = false;
    m_connect.set_sensitive(false);

    // A server with no typed password: ask the keyring first (async -- a
    // locked keyring may prompt). Nothing stored -> try without one; some
    // servers trust local users.
    if (e->caps().needs_server && p.password.empty()) {
        set_status("Looking up the password in the keyring…", "");
        std::weak_ptr<bool> alive = m_alive;
        keyring::lookup(account_for(saved),
            [this, alive, e, p](std::optional<std::string> pw, std::string err) mutable {
                if (alive.expired() || !*alive.lock()) return;
                if (!err.empty())
                    if (auto lg = log::get(log::Area::App)) lg->warn("keyring lookup: {}", err);
                if (pw) {
                    p.password = std::move(*pw);
                    m_pending_from_keyring = true;
                }
                start_connect(e, std::move(p));
            });
        return;
    }
    start_connect(e, std::move(p));
}

void Shell::start_connect(db::Engine* e, db::ConnectParams p) {  // connect: open + introspect on a worker
    set_status("Connecting to " + m_conn_label + "…", "");
    if (auto lg = log::get(log::Area::App))
        lg->info("connect (read-only): {}{}", m_conn_label,
                 m_pending_from_keyring ? " [password from keyring]" : "");

    m_worker = std::thread([this, e, p = std::move(p)]() mutable {
        std::unique_ptr<db::Connection> conn;
        db::Schema schema;
        std::string error;
        auto c = e->open(p);
        std::fill(p.password.begin(), p.password.end(), '\0');
        p.password.clear();
        if (c) {
            if (auto s = (*c)->introspect()) {
                conn = std::move(*c);
                schema = std::move(*s);
            } else {
                error = s.error().message;
            }
        } else {
            error = c.error().message;
        }
        {
            std::lock_guard lk(m_result_mx);
            m_result_conn = std::move(conn);
            m_result_schema = std::move(schema);
            m_result_error = std::move(error);
        }
        m_connect_done.emit();
    });
}

void Shell::on_connect_done() {  // connect: back on the UI thread
    if (m_worker.joinable()) m_worker.join();
    m_connect.set_sensitive(true);
    std::unique_ptr<db::Connection> conn;
    std::string error;
    {
        std::lock_guard lk(m_result_mx);
        conn = std::move(m_result_conn);
        error = std::move(m_result_error);
        if (conn) m_schema = std::move(m_result_schema);
    }
    const std::string typed = std::exchange(m_pending_password, std::string{});
    if (!conn) {
        set_status("Could not connect: " + error +
                   (m_pending_from_keyring ? " (The stored password may be out of date -- type it again.)" : ""),
                   "error");
        if (auto lg = log::get(log::Area::App)) lg->warn("connect failed: {}", error);
        return;
    }
    m_conn = std::move(conn);

    // It worked: remember the connection (never the password), and settle the
    // keyring the way the checkbox says.
    remember_connection(m_pending);
    if (!m_pending.is_file()) {
        const auto acct = account_for(m_pending);
        if (m_pending_remember && !typed.empty()) {
            keyring::store(acct, typed, [](bool ok, std::string err) {
                if (auto lg = log::get(log::Area::App)) {
                    if (ok) lg->info("password stored in the keyring");
                    else lg->warn("keyring store failed: {}", err);
                }
            });
        } else if (!m_pending_remember) {
            keyring::clear(acct, [](bool removed, std::string err) {
                if (auto lg = log::get(log::Area::App))
                    if (removed || !err.empty())
                        lg->info("keyring clear: {}", err.empty() ? "removed" : err);
            });
        }
    }
    std::size_t tables = 0, views = 0;
    for (auto& t : m_schema.tables) (t.kind == db::Table::Kind::View ? views : tables)++;
    set_status("Connected to " + m_conn_label + " (read-only): " + std::to_string(tables) +
               " table" + (tables == 1 ? "" : "s") + ", " + std::to_string(views) + " view" +
               (views == 1 ? "" : "s") + ".", "ok");
    set_title("Acorn — " + m_conn_label);
    show_schema();
    rebuild_table_list();   // s004c: the Tables area follows the connection
}

void Shell::show_schema() {  // connect: paint the table list
    while (auto* child = m_tables.get_first_child()) m_tables.remove(*child);
    if (m_schema.tables.empty()) {
        auto empty = Gtk::make_managed<Gtk::Label>(
            m_conn ? "This database has no tables yet." : "Tables and views appear here once you connect.");
        empty->add_css_class("acorn-status");
        empty->set_margin(12);
        m_tables.append(*empty);
        return;
    }
    for (auto& t : m_schema.tables) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        row->set_margin(6);
        auto name = Gtk::make_managed<Gtk::Label>(t.name);
        name->set_xalign(0.0f);
        name->set_size_request(200, -1);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        auto info = Gtk::make_managed<Gtk::Label>(summary(t));
        info->set_xalign(0.0f);
        info->set_hexpand(true);
        info->add_css_class("acorn-status");
        row->append(*name);
        row->append(*info);
        if (t.kind == db::Table::Kind::View) {
            auto b = Gtk::make_managed<Gtk::Label>("VIEW");
            b->add_css_class("acorn-badge");
            row->append(*b);
        }
        m_tables.append(*row);
    }
}

void Shell::set_status(const std::string& text, const char* kind) {  // connect: "", "ok", "error"
    m_status.set_text(text);
    m_status.remove_css_class("ok");
    m_status.remove_css_class("error");
    if (kind && *kind) m_status.add_css_class(kind);
}

}  // namespace acorn
