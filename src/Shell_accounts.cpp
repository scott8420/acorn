#include "Shell.hpp"
#include "Log.hpp"
#include "db/Accounts.hpp"

#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>

// ─────────────────────────────────────────────────────────────────────────────
// Shell_accounts.cpp -- the Accounts area (s019a). The engine's own accounts
// and what each may do with this database's tables -- read / add / change /
// delete, in words -- one card per account, yours marked. SQLite has none and
// says so. Reads only (db::read_accounts: SELECTs through the Gate); painted
// when the area comes into view and after a connect.
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
    m_accounts_scroll.set_vexpand(true);
    m_accounts_scroll.set_hexpand(true);
    m_accounts_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_accounts_page.append(m_accounts_scroll);
    m_stack.add(m_accounts_page, "accounts", "Accounts");
    paint_accounts();
}

void Shell::paint_accounts() {  // accounts: one card per account, what it may do per table
    auto status = [this](const std::string& s) {
        auto l = line(s, "acorn-status");
        l->set_valign(Gtk::Align::START);
        m_accounts_scroll.set_child(*l);
    };
    if (!m_conn || !m_conn_engine) {
        m_accounts_where.set_text("Connect to a database first.");
        status("The server's accounts appear here once you connect.");
        return;
    }
    auto r = db::read_accounts(*m_conn, m_conn_engine->dialect(), m_schema);
    if (auto lg = log::get(log::Area::Shell))
        lg->info("accounts: {}", r ? std::to_string(r->accounts.size()) + " read" : r.error().message);
    if (!r) {
        m_accounts_where.set_text(m_conn_label);
        status("Couldn't read the accounts: " + r.error().message + ".");
        return;
    }
    if (!r->supported) {
        m_accounts_where.set_text(m_conn_label);
        status(r->why);
        return;
    }
    const std::size_t n = r->accounts.size();
    std::string w = std::to_string(n) + (n == 1 ? " account" : " accounts") + " · what each may do with this database's "
                    "records -- read, add, change, delete";
    if (r->only_you) w += " · only yours is shown: seeing the others takes the right to read the server's account list";
    m_accounts_where.set_text(w);
    std::vector<std::string> tables;
    for (const auto& t : m_schema.tables) tables.push_back(t.name);
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
        card->set_size_request(260, -1);
        card->append(*line(db::account_name(a), "acorn-card-title"));
        if (const auto tags = db::account_tags(a); !tags.empty()) card->append(*line(tags, "acorn-field-name"));
        for (const auto& l : db::account_lines(a, tables)) card->append(*line(l));
        flow->append(*card);
    }
    m_accounts_scroll.set_child(*flow);
}

}  // namespace acorn
