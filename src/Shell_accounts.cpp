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
//     can't bring back (its password), shows the SQL.
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
    m_acct_make.add_css_class("suggested-action");
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
    m_acct_say.set_text(why.empty() ? "Ready -- the server gets the password; Acorn keeps no copy. Ctrl+Z deletes the account again."
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
    paint_accounts();
    journal_note("Made the account " + db::account_words(name, host) + ". Ctrl+Z deletes it again "
                 "(a redo would make it without its password).", "ok");
}

void Shell::ask_delete_account(const std::string& name, const std::string& host) {  // accounts: asks first
    if (!m_journal) return;
    const auto id = db::Store::account_id(name, host);
    auto pv = db::verbs::account_preview(*m_journal, id, std::nullopt);
    const bool maria = m_conn_engine && m_conn_engine->dialect() == db::Dialect::MariaDB;
    auto dlg = Gtk::AlertDialog::create("Delete the account " + db::account_words(name, host) + "?");
    dlg->set_detail(
        "It goes from the whole server, not only this database.\n\n"
        "Ctrl+Z can make it again with what it may do here -- but NOT its password " +
        std::string(maria ? "(it comes back locked: set a password and unlock it before anyone logs in with it)"
                          : "(it comes back with none: set one before anyone logs in with it)") +
        ", and not what it may do in other databases.\n\nWill run:\n" + (pv ? joined(*pv) + ";" : pv.error().message));
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
        paint_accounts();
        if (r) journal_note("Deleted the account " + db::account_words(name, host) + ". Ctrl+Z makes it again with its "
                            "rights here -- not its password.", "ok");
        else   journal_note("Not deleted: " + r.error().message + ".", "error");
    });
}

void Shell::paint_accounts() {  // accounts: one card per account, what it may do per table
    auto status = [this](const std::string& s) {
        auto l = line(s, "acorn-status");
        l->set_valign(Gtk::Align::START);
        m_accounts_scroll.set_child(*l);
    };
    m_accounts_can = false;
    m_accounts_new.set_visible(false);
    if (!m_conn || !m_conn_engine) {
        m_accounts_editor.set_visible(false);
        m_accounts_where.set_text("Connect to a database first.");
        status("The server's accounts appear here once you connect.");
        return;
    }
    const auto d = m_conn_engine->dialect();
    auto r = db::read_accounts(*m_conn, d, m_schema);
    if (auto lg = log::get(log::Area::Shell))
        lg->info("accounts: {}", r ? std::to_string(r->accounts.size()) + " read" : r.error().message);
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
    const std::size_t n = r->accounts.size();
    std::string w = std::to_string(n) + (n == 1 ? " account" : " accounts") + " · what each may do with this database's "
                    "records -- read, add, change, delete";
    if (r->only_you) w += " · only yours is shown: seeing the others takes the right to read the server's account list";
    m_accounts_where.set_text(w);

    // s019b: may this connection change accounts -- and if not, why not.
    const db::Account* me = nullptr;
    for (const auto& a : r->accounts) if (a.you) me = &a;
    m_accounts_why = !m_journal || !m_writable
        ? "This connection is open read-only -- accounts can't be changed through it."
        : r->only_you ? "Acorn can't read the server's account list through your account, so it changes no accounts here."
        : !me || !(me->admin || me->super)
            ? "Your account " + (me ? db::account_words(me->name, me->host) : std::string("here")) +
                  " can't make accounts or give rights -- that takes an account that can (the server's admin's)."
            : std::string{};
    m_accounts_can = m_accounts_why.empty();
    m_accounts_new.set_visible(true);
    m_accounts_new.set_sensitive(m_accounts_can);
    m_accounts_new.set_tooltip_text(m_accounts_can ? "Make an account on the server -- a name, a password; rights after, per table"
                                                   : m_accounts_why);
    if (!m_accounts_can) m_accounts_editor.set_visible(false);
    else if (m_accounts_editor.get_visible()) account_editor_check();

    std::vector<std::string> tables;
    for (const auto& t : m_schema.tables) tables.push_back(t.name);
    const char q = m_conn->quote_ident("x").front();
    const std::string dbname = m_accounts_can ? m_journal->store().database_name() : std::string{};
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
        const std::string hold = a.you   ? "Your own account -- Acorn doesn't change it from under the connection."
                               : a.super ? "May do everything -- there's nothing to give it per table."
                                         : std::string{};
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

        // Its own rights -- the boxes. Read through the store (the picture undo uses).
        const auto id = db::Store::account_id(a.name, a.host);
        auto& st = m_journal->store();
        st.clear_failed();
        auto got = st.get(id);
        if (st.failed() || !got) {
            card->append(*line("Can't read what it was given here: " +
                               (st.failed() ? st.error().message : std::string("it's gone")) + ".", "acorn-status"));
            st.clear_failed();
            flow->append(*card);
            continue;
        }
        const auto pic = std::get<db::AccountDef>(*got);
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
                            paint_accounts();
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
