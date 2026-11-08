#include "Shell.hpp"
#include "Log.hpp"
#include "db/Accounts.hpp"
#include "db/Journal.hpp"

#include <gtkmm/alertdialog.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <glibmm/main.h>

// ─────────────────────────────────────────────────────────────────────────────
// Shell_accounts.cpp -- the Accounts area. s019a: the engine's own accounts
// and what each may do with this database's tables -- read / add / change /
// delete, in words -- one card per account, yours marked. SQLite has none and
// says so. s019b: the changes, each one step through the door (db::verbs --
// new_account / set_rights / delete_account), Ctrl+Z gives it back:
//   * New Account… -- an editor in place: a name, MariaDB's host (% = any),
//     a password typed twice (sent to the server, never kept); the exact SQL
//     it will run under it, the password shown as ********.
//   * per card, the rights the account was GIVEN: four check boxes per table
//     and for every table. A box's tooltip is the exact SQL ticking or
//     unticking runs. MariaDB's "every table" is `db`.* -- a table's box it
//     covers is ticked and greyed ("untick it on Every table").
//   * Delete Account -- asks first (Cancel is the default), says what Ctrl+Z
//     brings back, shows the SQL.
// s021: Password… on a card -- an editor in place (typed twice, the SQL with
// '********'); a locked account is unlocked by it. Lock / Unlock (MariaDB).
// Ctrl+Z puts the old password back where the server shows Acorn the stored
// one (MariaDB; PostgreSQL to a superuser) -- otherwise said before you press.
// Greyed, with the reason in the tooltip, on a read-only connection, when
// this account can't make accounts, and on your own account and one that may
// do everything.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn {

namespace {
Gtk::Label* line(const std::string& s, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    l->set_xalign(0.0f);
    l->set_wrap(true);
    l->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    if (css) l->add_css_class(css);
    return l;
}
std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ";\n") + s;
    return out;
}
constexpr std::pair<unsigned, const char*> kRights[] = {
    {db::kRead, "read"}, {db::kAdd, "add"}, {db::kChange, "change"}, {db::kDelete, "delete"}};
}  // namespace

void Shell::build_accounts_page() {  // accounts: the Accounts area
    m_accounts_page.set_margin(28);
    m_accounts_page.set_spacing(12);
    auto hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    auto title = Gtk::make_managed<Gtk::Label>("Accounts");
    title->set_xalign(0.0f);
    title->add_css_class("acorn-hero-title");
    m_accounts_where.set_xalign(0.0f);
    m_accounts_where.set_wrap(true);
    m_accounts_where.add_css_class("acorn-hero-sub");
    hero->append(*title);
    hero->append(m_accounts_where);
    m_accounts_page.append(*hero);

    // s019b: New Account… and the note.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_accounts_new.set_label("New Account…");
    m_accounts_new.signal_clicked().connect([this] {
        m_acct_name.set_text("");
        m_acct_host.set_text("%");
        m_acct_pw.set_text("");
        m_acct_pw2.set_text("");
        m_pw_editor.set_visible(false);   // s021: one editor at a time
        m_accounts_editor.set_visible(true);
        account_editor_check();
        m_acct_name.grab_focus();
    });
    bar->append(m_accounts_new);
    m_accounts_note.set_xalign(0.0f);
    m_accounts_note.set_wrap(true);
    m_accounts_note.set_hexpand(true);
    m_accounts_note.add_css_class("acorn-status");
    m_accounts_note.set_visible(false);
    bar->append(m_accounts_note);
    m_accounts_page.append(*bar);

    // The editor, in place, hidden until New Account…
    m_accounts_editor.add_css_class("acorn-card");
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(10);
    auto label = [](const char* s) {
        auto l = Gtk::make_managed<Gtk::Label>(s);
        l->set_xalign(1.0f);
        return l;
    };
    m_acct_name.set_hexpand(true);
    m_acct_name.set_placeholder_text("a name -- clerk");
    m_acct_host.set_tooltip_text("Where it may log in from: % is any host, localhost only this machine");
    m_acct_host_label.set_xalign(1.0f);
    m_acct_pw.set_visibility(false);
    m_acct_pw2.set_visibility(false);
    m_acct_pw.set_tooltip_text("Sent to the server once -- Acorn keeps no copy, and never writes it in the journal");
    grid->attach(*label("Name"), 0, 0);
    grid->attach(m_acct_name, 1, 0);
    grid->attach(m_acct_host_label, 0, 1);
    grid->attach(m_acct_host, 1, 1);
    grid->attach(*label("Password"), 0, 2);
    grid->attach(m_acct_pw, 1, 2);
    grid->attach(*label("Again"), 0, 3);
    grid->attach(m_acct_pw2, 1, 3);
    m_accounts_editor.append(*grid);
    m_acct_sql.set_xalign(0.0f);
    m_acct_sql.set_wrap(true);
    m_acct_sql.set_selectable(true);
    m_acct_sql.add_css_class("acorn-mono");
    m_accounts_editor.append(m_acct_sql);
    m_acct_say.set_xalign(0.0f);
    m_acct_say.set_wrap(true);
    m_acct_say.add_css_class("acorn-status");
    m_accounts_editor.append(m_acct_say);
    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    m_acct_cancel.set_label("Cancel");
    m_acct_make.set_label("Make Account");
    m_acct_make.add_css_class("acorn-primary");
    buttons->append(m_acct_cancel);
    buttons->append(m_acct_make);
    m_accounts_editor.append(*buttons);
    m_accounts_editor.set_visible(false);
    m_accounts_editor.set_size_request(420, -1);
    m_accounts_editor.set_halign(Gtk::Align::START);
    for (auto* e : {&m_acct_name, &m_acct_host, &m_acct_pw, &m_acct_pw2})
        e->signal_changed().connect([this] { account_editor_check(); });
    for (auto* e : {&m_acct_name, &m_acct_host, &m_acct_pw2})
        e->signal_activate().connect([this] { make_account(); });
    m_acct_cancel.signal_clicked().connect([this] {
        m_acct_pw.set_text("");
        m_acct_pw2.set_text("");
        m_accounts_editor.set_visible(false);
    });
    m_acct_make.signal_clicked().connect([this] { make_account(); });
    m_accounts_page.append(m_accounts_editor);

    // s021: Set Password, in place, hidden until a card's Password…
    m_pw_editor.add_css_class("acorn-card");
    m_pw_title.set_xalign(0.0f);
    m_pw_title.add_css_class("acorn-card-title");
    m_pw_editor.append(m_pw_title);
    auto pgrid = Gtk::make_managed<Gtk::Grid>();
    pgrid->set_row_spacing(6);
    pgrid->set_column_spacing(10);
    m_pw1.set_hexpand(true);
    m_pw1.set_visibility(false);
    m_pw2.set_visibility(false);
    m_pw1.set_tooltip_text("Sent to the server once -- Acorn keeps no copy, and never writes it in the journal");
    pgrid->attach(*label("New password"), 0, 0);
    pgrid->attach(m_pw1, 1, 0);
    pgrid->attach(*label("Again"), 0, 1);
    pgrid->attach(m_pw2, 1, 1);
    m_pw_editor.append(*pgrid);
    m_pw_sql.set_xalign(0.0f);
    m_pw_sql.set_wrap(true);
    m_pw_sql.set_selectable(true);
    m_pw_sql.add_css_class("acorn-mono");
    m_pw_editor.append(m_pw_sql);
    m_pw_say.set_xalign(0.0f);
    m_pw_say.set_wrap(true);
    m_pw_say.add_css_class("acorn-status");
    m_pw_editor.append(m_pw_say);
    auto pbuttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    pbuttons->set_halign(Gtk::Align::END);
    m_pw_cancel.set_label("Cancel");
    m_pw_set.set_label("Set Password");
    m_pw_set.add_css_class("acorn-primary");
    pbuttons->append(m_pw_cancel);
    pbuttons->append(m_pw_set);
    m_pw_editor.append(*pbuttons);
    m_pw_editor.set_visible(false);
    m_pw_editor.set_size_request(420, -1);
    m_pw_editor.set_halign(Gtk::Align::START);
    for (auto* e : {&m_pw1, &m_pw2}) e->signal_changed().connect([this] { password_editor_check(); });
    m_pw2.signal_activate().connect([this] { set_account_password(); });
    m_pw_cancel.signal_clicked().connect([this] {
        m_pw1.set_text("");
        m_pw2.set_text("");
        m_pw_editor.set_visible(false);
    });
    m_pw_set.signal_clicked().connect([this] { set_account_password(); });
    m_accounts_page.append(m_pw_editor);

    m_accounts_scroll.set_vexpand(true);
    m_accounts_scroll.set_hexpand(true);
    m_accounts_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_accounts_page.append(m_accounts_scroll);
    m_stack.add(m_accounts_page, "accounts", "Accounts");
    paint_accounts();
}

void Shell::accounts_note(const std::string& text, const char* kind) {  // accounts: under the Accounts title
    m_accounts_note.set_text(text);
    m_accounts_note.set_visible(!text.empty());
    m_accounts_note.remove_css_class("ok");
    m_accounts_note.remove_css_class("error");
    if (kind && *kind) m_accounts_note.add_css_class(kind);
}

void Shell::account_editor_check() {  // accounts: New Account's live line and SQL
    if (!m_conn || !m_conn_engine) return;
    const auto d = m_conn_engine->dialect();
    const bool maria = d == db::Dialect::MariaDB;
    m_acct_host.set_visible(maria);
    m_acct_host_label.set_visible(maria);
    const std::string name = m_acct_name.get_text();
    const std::string host = maria ? std::string(m_acct_host.get_text()) : std::string{};
    const std::string pw = m_acct_pw.get_text(), pw2 = m_acct_pw2.get_text();
    std::string why;
    if (!m_accounts_can) why = m_accounts_why;
    else if (name.empty()) why = "Name the account.";
    else if (auto w = db::check_account_name(d, name, host); !w.empty()) why = w + ".";
    else if (pw.empty()) why = "Type a password for it -- twice.";
    else if (pw != pw2) why = pw2.empty() ? "Type the password again." : "The two passwords differ.";
    const bool named = !name.empty() && db::check_account_name(d, name, host).empty();
    db::AccountDef a;
    a.name = name;
    a.host = host;
    m_acct_sql.set_text(named ? "Will run:\n" + joined(db::account_sql(d, m_conn->quote_ident("x").front(), {}, std::nullopt, a,
                                                                          std::string("x"), true)) + ";"
                              : std::string{});
    m_acct_sql.set_visible(named);
    m_acct_say.set_text(why.empty() ? "Ready -- the server gets the password; Acorn keeps no copy of what you type. Ctrl+Z "
                                      "deletes the account again."
                                    : why);
    m_acct_say.remove_css_class("error");
    m_acct_say.remove_css_class("ok");
    m_acct_say.add_css_class(why.empty() ? "ok" : "error");
    m_acct_make.set_sensitive(why.empty());
}

void Shell::make_account() {  // accounts: New Account -> verbs::new_account
    account_editor_check();
    if (!m_acct_make.get_sensitive() || !m_journal || !m_conn_engine) return;
    const bool maria = m_conn_engine->dialect() == db::Dialect::MariaDB;
    const std::string name = m_acct_name.get_text();
    const std::string host = maria ? std::string(m_acct_host.get_text()) : std::string{};
    auto r = db::verbs::new_account(*m_journal, name, host, m_acct_pw.get_text());
    m_acct_pw.set_text("");    // never left lying in the window
    m_acct_pw2.set_text("");
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: new account: {}", r ? m_journal->undo_label() : r.error().message);   // never the password
    if (!r) {
        m_acct_say.set_text("Not made: " + r.error().message + ".");
        m_acct_say.remove_css_class("ok");
        m_acct_say.add_css_class("error");
        return;
    }
    m_accounts_editor.set_visible(false);
    paint_accounts(true);
    journal_note("Made the account " + db::account_words(name, host) + ". Ctrl+Z deletes it again" +
                 std::string(m_passwords_kept ? "; a redo makes it with this password."
                                              : " (a redo makes it with a password no one knows -- set one after)."), "ok");
}

void Shell::ask_delete_account(const std::string& name, const std::string& host) {  // accounts: asks first
    if (!m_journal) return;
    const auto id = db::Store::account_id(name, host);
    auto pv = db::verbs::account_preview(*m_journal, id, std::nullopt);
    const bool maria = m_conn_engine && m_conn_engine->dialect() == db::Dialect::MariaDB;
    auto dlg = Gtk::AlertDialog::create("Delete the account " + db::account_words(name, host) + "?");
    dlg->set_detail(
        "It goes from the whole server, not only this database.\n\n"
        "Ctrl+Z can make it again with what it may do here" +
        std::string(m_passwords_kept ? " and its password as it was"
                    : maria ? " -- locked, without its password (set one to let it in)"
                            : " -- with a password no one knows: PostgreSQL shows the stored one only to a superuser "
                              "(set one to let it in)") +
        " -- but not what it may do in other databases.\n\nWill run:\n" + (pv ? joined(*pv) + ";" : pv.error().message));
    dlg->set_buttons({"Cancel", "Delete Account"});
    dlg->set_cancel_button(0);
    dlg->set_default_button(0);   // never pre-chosen
    dlg->set_modal(true);
    dlg->choose(*this, [this, dlg, name, host, id](Glib::RefPtr<Gio::AsyncResult>& res) {
        int pick = 0;
        try { pick = dlg->choose_finish(res); } catch (const Glib::Error&) { pick = 0; }
        if (pick != 1 || !m_journal) return;
        auto r = db::verbs::delete_account(*m_journal, id);
        if (auto lg = log::get(log::Area::App))
            lg->info("journal: delete account '{}': {}", name, r ? "deleted" : r.error().message);
        paint_accounts(true);
        if (r) journal_note("Deleted the account " + db::account_words(name, host) + ". Ctrl+Z makes it again with its "
                            "rights here" + std::string(m_passwords_kept ? " and its password." : " -- not its password."), "ok");
        else   journal_note("Not deleted: " + r.error().message + ".", "error");
    });
}

void Shell::open_password_editor(const db::AccountDef& a) {  // accounts: s021 -- Password… on a card
    m_accounts_editor.set_visible(false);
    m_pw_for = a;
    m_pw_title.set_text("Password for " + db::account_words(a.name, a.host));
    m_pw1.set_text("");
    m_pw2.set_text("");
    m_pw_editor.set_visible(true);
    password_editor_check();
    m_pw1.grab_focus();
}

void Shell::password_editor_check() {  // accounts: s021 -- Set Password's live line and SQL
    if (!m_conn || !m_conn_engine) return;
    const auto d = m_conn_engine->dialect();
    const std::string pw = m_pw1.get_text(), pw2 = m_pw2.get_text();
    std::string why;
    if (!m_accounts_can) why = m_accounts_why;
    else if (pw.empty()) why = "Type the new password -- twice.";
    else if (pw != pw2) why = pw2.empty() ? "Type it again." : "The two passwords differ.";
    db::AccountDef want = m_pw_for;
    want.locked = false;
    m_pw_sql.set_text("Will run:\n" + joined(db::account_sql(d, m_conn->quote_ident("x").front(), {}, m_pw_for, want,
                                                             std::string("x"), true)) + ";");
    std::string ready = "Ready -- the server gets the password; Acorn keeps no copy of what you type.";
    if (m_pw_for.locked) ready += " It unlocks the account too.";
    ready += m_passwords_kept ? " Ctrl+Z puts the old password back."
                              : " PostgreSQL shows the stored password only to a superuser, so Ctrl+Z can't put the old "
                                "one back -- this one isn't a step.";
    m_pw_say.set_text(why.empty() ? ready : why);
    m_pw_say.remove_css_class("error");
    m_pw_say.remove_css_class("ok");
    m_pw_say.add_css_class(why.empty() ? "ok" : "error");
    m_pw_set.set_sensitive(why.empty());
}

void Shell::set_account_password() {  // accounts: s021 -- verbs::set_password
    password_editor_check();
    if (!m_pw_set.get_sensitive() || !m_journal) return;
    const auto who = db::account_words(m_pw_for.name, m_pw_for.host);
    bool stepped = false;
    auto r = db::verbs::set_password(*m_journal, db::Store::account_id(m_pw_for.name, m_pw_for.host), m_pw1.get_text(),
                                     &stepped);
    m_pw1.set_text("");    // never left lying in the window
    m_pw2.set_text("");
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: set password '{}': {}", m_pw_for.name, r ? (stepped ? "a step" : "set, no step") : r.error().message);
    if (!r) {
        m_pw_say.set_text("Not set: " + r.error().message + ".");
        m_pw_say.remove_css_class("ok");
        m_pw_say.add_css_class("error");
        return;
    }
    m_pw_editor.set_visible(false);
    paint_accounts(true);
    if (stepped) journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z puts the old password back.", "ok");
    else         journal_note("Set the password for " + who + " -- not a step: the server doesn't show Acorn the old "
                              "one, so Ctrl+Z can't put it back.", "ok");
}

void Shell::lock_account(const std::string& name, const std::string& host, bool lock) {  // accounts: s021 -- verbs::set_locked
    if (!m_journal) return;
    auto r = db::verbs::set_locked(*m_journal, db::Store::account_id(name, host), lock);
    if (auto lg = log::get(log::Area::App))
        lg->info("journal: {} '{}': {}", lock ? "lock" : "unlock", name, r ? m_journal->undo_label() : r.error().message);
    paint_accounts(true);
    if (r) journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
    else   journal_note(std::string(lock ? "Not locked: " : "Not unlocked: ") + r.error().message + ".", "error");
}

void Shell::paint_accounts(bool keep) {  // accounts: read on the worker (s020c), painted by paint_account_cards
    if (!m_conn || !m_conn_engine) {
        m_accounts_can = false;
        m_accounts_new.set_visible(false);
        m_accounts_editor.set_visible(false);
        m_pw_editor.set_visible(false);
        m_accounts_where.set_text("Connect to a database first.");
        m_accounts_scroll.set_child(*account_status("The server's accounts appear here once you connect."));
        return;
    }
    // `keep`: a repaint after a change -- the cards stay until the new read
    // lands (no flash); otherwise say what's happening at once.
    if (!keep) {
        m_accounts_can = false;
        m_accounts_new.set_visible(false);
        m_accounts_where.set_text(m_conn_label);
        m_accounts_scroll.set_child(*account_status("Reading the accounts…"));
    }
    const auto d = m_conn_engine->dialect();
    db::Store* st = m_journal ? &m_journal->store() : nullptr;
    const bool writable = m_journal && m_writable;
    const bool started = run_long("Reading the server's accounts",
        [this, d, schema = m_schema, st, writable](LongTicket& tk) -> std::function<void()> {
            auto cards = std::make_shared<db::AccountCards>(db::read_account_cards(
                *m_conn, d, schema, st, writable, [&tk](std::int64_t n) { tk.count = n; return !tk.stop.load(); }));
            return [this, cards] { paint_account_cards(*cards); };
        });
    if (!started) m_accounts_again = true;   // a worker is reading: long_done reads them after
}

Gtk::Label* Shell::account_status(const std::string& s) {  // accounts: one line in place of the cards
    auto l = line(s, "acorn-status");
    l->set_valign(Gtk::Align::START);
    return l;
}

void Shell::paint_account_cards(const db::AccountCards& c) {  // accounts: one card per account, what it may do per table
    auto status = [this](const std::string& s) { m_accounts_scroll.set_child(*account_status(s)); };
    m_accounts_can = false;
    m_accounts_new.set_visible(false);
    if (!m_conn || !m_conn_engine) return;
    const auto d = m_conn_engine->dialect();
    const auto& r = c.list;
    if (auto lg = log::get(log::Area::Shell))
        lg->info("accounts: {}{}", r ? std::to_string(r->accounts.size()) + " read" : r.error().message,
                 c.stopped ? " (stopped)" : "");
    if (!r) {
        m_accounts_where.set_text(m_conn_label);
        status("Couldn't read the accounts: " + r.error().message + ".");
        return;
    }
    if (!r->supported) {
        m_accounts_editor.set_visible(false);
        m_accounts_where.set_text(m_conn_label);
        status(r->why);
        return;
    }
    if (c.stopped) {   // s020c: Stop between accounts -- say so, offer the read again
        m_accounts_editor.set_visible(false);
        m_accounts_where.set_text(m_conn_label);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        box->set_valign(Gtk::Align::START);
        box->append(*account_status("Stopped before every account was read -- nothing to show."));
        auto again = Gtk::make_managed<Gtk::Button>("Read Again");
        again->set_name("shell.accounts.again");
        again->set_halign(Gtk::Align::START);
        again->signal_clicked().connect([this] { paint_accounts(); });
        box->append(*again);
        m_accounts_scroll.set_child(*box);
        return;
    }
    const std::size_t n = r->accounts.size();
    std::string w = std::to_string(n) + (n == 1 ? " account" : " accounts") + " · what each may do with this database's "
                    "records -- read, add, change, delete";
    if (r->only_you) w += " · only yours is shown: seeing the others takes the right to read the server's account list";
    m_accounts_where.set_text(w);

    // s019b: may this connection change accounts -- and if not, why not
    // (s020c: worked out with the read, db::accounts_why).
    m_accounts_why = c.why;
    m_accounts_can = m_accounts_why.empty() && m_journal;
    m_passwords_kept = c.passwords_kept;
    if (!m_accounts_can) m_pw_editor.set_visible(false);
    m_accounts_new.set_visible(true);
    m_accounts_new.set_sensitive(m_accounts_can);
    m_accounts_new.set_tooltip_text(m_accounts_can ? "Make an account on the server -- a name, a password; rights after, per table"
                                                   : m_accounts_why);
    if (!m_accounts_can) m_accounts_editor.set_visible(false);
    else if (m_accounts_editor.get_visible()) account_editor_check();

    std::vector<std::string> tables;
    for (const auto& t : m_schema.tables) tables.push_back(t.name);
    const char q = m_conn->quote_ident("x").front();
    const std::string dbname = c.database;
    const bool maria = d == db::Dialect::MariaDB;

    auto flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_name("shell.accounts.cards");
    flow->set_selection_mode(Gtk::SelectionMode::NONE);
    flow->set_homogeneous(false);
    flow->set_min_children_per_line(1);
    flow->set_max_children_per_line(4);
    flow->set_row_spacing(12);
    flow->set_column_spacing(12);
    flow->set_valign(Gtk::Align::START);
    for (const auto& a : r->accounts) {
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("acorn-card");
        card->set_size_request(300, -1);
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto nm = line(db::account_name(a), "acorn-card-title");
        nm->set_hexpand(true);
        head->append(*nm);
        card->append(*head);
        if (const auto tags = db::account_tags(a); !tags.empty()) card->append(*line(tags, "acorn-field-name"));
        for (const auto& l : db::account_lines(a, tables)) card->append(*line(l));
        if (!m_accounts_can) { flow->append(*card); continue; }

        // Why this one can't be changed here, if it can't.
        const std::string hold = db::account_hold(a);
        auto del = Gtk::make_managed<Gtk::Button>();
        del->set_icon_name("user-trash-symbolic");
        del->add_css_class("flat");
        del->set_name("shell.accounts.delete");
        const bool can_delete = hold.empty() && !a.admin;
        del->set_sensitive(can_delete);
        del->set_tooltip_text(can_delete ? "Delete Account -- asks first"
                              : !hold.empty() ? hold
                                              : "It makes accounts itself -- Acorn leaves deleting an account like that to "
                                                "the server's own tools.");
        del->signal_clicked().connect([this, nme = a.name, hst = a.host] { ask_delete_account(nme, hst); });
        head->append(*del);
        if (!hold.empty()) { card->append(*line(hold, "acorn-status")); flow->append(*card); continue; }

        // Its own rights -- the boxes. Read through the store (the picture undo
        // uses) on the worker, with the list.
        const auto id = db::Store::account_id(a.name, a.host);
        const auto got = c.given.find(id);
        if (got == c.given.end() || !got->second) {
            card->append(*line("Can't read what it was given here: " +
                               (got == c.given.end() ? std::string("not read") : got->second.error()) + ".", "acorn-status"));
            flow->append(*card);
            continue;
        }
        const auto& pic = *got->second;

        // s021: how it logs in -- the lock, Password…, Lock / Unlock.
        const bool has_pw = db::has_password(d, pic);
        if (pic.locked)
            card->append(*line(has_pw ? "Locked -- no one logs in with it until it's unlocked."
                                      : "Locked, and no password -- Password… lets it in.", "acorn-status"));
        auto acts = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        acts->set_margin_top(4);
        auto pwb = Gtk::make_managed<Gtk::Button>("Password…");
        pwb->set_name("shell.accounts.password");
        pwb->set_sensitive(!a.admin);
        pwb->set_tooltip_text(a.admin ? "It makes accounts itself -- Acorn leaves its password to the server's own tools."
                              : pic.locked ? "Set a new password -- that unlocks it too"
                                           : "Set a new password -- typed twice, sent to the server, never kept");
        pwb->signal_clicked().connect([this, pic] { open_password_editor(pic); });
        acts->append(*pwb);
        if (maria) {
            auto lkb = Gtk::make_managed<Gtk::Button>(pic.locked ? "Unlock" : "Lock");
            lkb->set_name("shell.accounts.lock");
            db::AccountDef want = pic;
            want.locked = !pic.locked;
            const auto sql = db::account_sql(d, q, dbname, pic, want, std::nullopt, true);
            const bool can = !a.admin && (!pic.locked || has_pw);
            lkb->set_sensitive(can);
            lkb->set_tooltip_text(a.admin ? "It makes accounts itself -- Acorn leaves locking it to the server's own tools."
                                  : !can ? "It has no password -- unlocked, anyone could log in as it. Password… unlocks it too."
                                         : std::string(pic.locked ? "Let it log in again. Runs:\n" : "Keep everyone from logging "
                                                       "in with it (its rights stay). Runs:\n") + joined(sql) + ";");
            lkb->signal_clicked().connect([this, nme = a.name, hst = a.host, lock = !pic.locked] { lock_account(nme, hst, lock); });
            acts->append(*lkb);
        }
        card->append(*acts);

        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(8);
        grid->set_row_spacing(2);
        grid->set_margin_top(6);
        auto given = line("Given here:", "acorn-field-name");
        grid->attach(*given, 0, 0);
        for (int c = 0; c < 4; ++c) {
            auto h = Gtk::make_managed<Gtk::Label>(kRights[c].second);
            h->add_css_class("acorn-field-name");
            grid->attach(*h, c + 1, 0);
        }
        std::vector<std::string> rows{""};   // "" = every table
        for (const auto& t : tables) rows.push_back(t);
        int row = 1;
        for (const auto& t : rows) {
            auto rl = line(t.empty() ? "Every table" : t);
            rl->set_ellipsize(Pango::EllipsizeMode::END);
            rl->set_max_width_chars(18);
            grid->attach(*rl, 0, row);
            for (int c = 0; c < 4; ++c) {
                const unsigned bit = kRights[c].first;
                auto has = [&](const std::string& tt) {
                    auto it = pic.tables.find(tt);
                    return it != pic.tables.end() && (it->second & bit);
                };
                bool on = false, covered = false;
                if (t.empty()) {
                    if (maria) on = pic.every & bit;
                    else {
                        on = !tables.empty();
                        for (const auto& tt : tables) on = on && has(tt);
                    }
                } else {
                    covered = maria && (pic.every & bit);
                    on = covered || has(t);
                }
                auto cb = Gtk::make_managed<Gtk::CheckButton>();
                cb->set_name("shell.accounts.right");
                cb->set_halign(Gtk::Align::CENTER);
                cb->set_active(on);
                if (covered) {
                    cb->set_sensitive(false);
                    cb->set_tooltip_text("Given on every table -- untick it on Every table");
                } else {
                    // The exact SQL the click will run, worked out from the picture.
                    db::AccountDef want = pic;
                    auto flip = [&](const std::string& tt) {
                        unsigned& p = want.tables[tt];
                        p = on ? (p & ~bit) : (p | bit);
                    };
                    if (!t.empty()) flip(t);
                    else if (maria) want.every = on ? (want.every & ~bit) : (want.every | bit);
                    else for (const auto& tt : tables) flip(tt);
                    std::erase_if(want.tables, [](const auto& kv) { return kv.second == 0; });
                    const auto sql = db::account_sql(d, q, dbname, pic, want, std::nullopt, false);
                    cb->set_tooltip_text(std::string(on ? "Unticking runs:\n" : "Ticking runs:\n") +
                                         (sql.empty() ? "nothing" : joined(sql) + ";"));
                    cb->signal_toggled().connect([this, id, t, bit, cb] {
                        const bool now = cb->get_active();
                        Glib::signal_idle().connect_once([this, id, t, bit, now] {   // repaint after the click is done
                            if (!m_journal) return;
                            auto res = db::verbs::set_rights(*m_journal, id, t, bit, now);
                            if (auto lg = log::get(log::Area::App))
                                lg->info("journal: rights: {}", res ? m_journal->undo_label() : res.error().message);
                            paint_accounts(true);
                            if (res) journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
                            else     journal_note("Not changed: " + res.error().message + ".", "error");
                        });
                    });
                }
                grid->attach(*cb, c + 1, row);
            }
            ++row;
        }
        card->append(*grid);
        flow->append(*card);
    }
    m_accounts_scroll.set_child(*flow);
}

}  // namespace acorn
