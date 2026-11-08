#include "db/Journal.hpp"
#include "db/Accounts.hpp"

#include <algorithm>
#include <cctype>
#include <set>

// ─────────────────────────────────────────────────────────────────────────────
// db/JournalAccounts.cpp -- the door's accounts (s019b). The account picture
// (AccountDef: the account and the four record rights it was GIVEN here),
// how the store reads it and turns two pictures into SQL, and the three verbs
// the Accounts area offers: New Account, Give / Take rights, Delete Account.
// Accounts are the SERVER's: every statement here is seen by every database
// on it, so the store refuses the connection's own account outright, and the
// selftest only ever makes acorn_selftest_<pid>_* accounts.
//
// A password is sent once (Store::set_password, consumed by the put that
// makes the account) and kept nowhere: not in a picture, a label, a log.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::db {

namespace {
std::string curly(const std::string& s) { return "“" + s + "”"; }

std::string text_of(const Value& v) {
    if (const auto* t = v.as_text()) return *t;
    if (auto i = v.as_int()) return std::to_string(*i);
    return {};
}
bool yes(const Value& v) {
    std::string t = text_of(v);
    for (auto& ch : t) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return t == "1" || t == "T" || t == "TRUE" || t == "Y" || t == "YES";
}

// A string literal. MariaDB reads a backslash as an escape (unless
// NO_BACKSLASH_ESCAPES), so it is doubled there too; PostgreSQL's standard
// strings take it as itself.
std::string lit(Dialect d, const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        out += ch;
        if (ch == '\'') out += '\'';
        else if (ch == '\\' && d == Dialect::MariaDB) out += '\\';
    }
    return out + "'";
}

// SELECT, INSERT ... in the four rights' order.
std::string privs_sql(unsigned p) {
    std::string out;
    const std::pair<unsigned, const char*> w[] = {{kRead, "SELECT"}, {kAdd, "INSERT"}, {kChange, "UPDATE"}, {kDelete, "DELETE"}};
    for (const auto& [bit, s] : w)
        if (p & bit) out += (out.empty() ? "" : ", ") + std::string(s);
    return out;
}

// MariaDB reads a database name in a database-level grant as a LIKE pattern
// (`my_db`.* would also cover `myXdb`): its _ and % are escaped there, so the
// grant names this database and no other. Table grants name it literally.
std::string db_pattern(const std::string& db) {
    std::string out;
    for (char ch : db) { if (ch == '_' || ch == '%') out += '\\'; out += ch; }
    return out;
}

Error refused(std::string m) { return Error{Error::Code::Unsupported, std::move(m)}; }
const char* kNoAccounts = "a SQLite file has no accounts -- the file's own permissions decide who may read and change it";
const char* kOwn = "that's the account this connection uses -- Acorn doesn't change its own account from under it";
}  // namespace

// ── ids, names ──────────────────────────────────────────────────────────────
Store::Id Store::account_id(const std::string& name, const std::string& host) {
    return "account:" + std::to_string(name.size()) + ":" + name + "@" + host;
}

bool Store::parse_account_id(const Id& id, std::string& name, std::string& host) {
    if (!is_account(id)) return false;
    std::size_t i = 8, n = 0;
    if (i >= id.size() || !std::isdigit(static_cast<unsigned char>(id[i]))) return false;
    while (i < id.size() && std::isdigit(static_cast<unsigned char>(id[i]))) {
        n = n * 10 + static_cast<std::size_t>(id[i] - '0');
        if (n > id.size()) return false;
        ++i;
    }
    if (i >= id.size() || id[i] != ':' || i + 1 + n >= id.size() || id[i + 1 + n] != '@') return false;
    name = id.substr(i + 1, n);
    host = id.substr(i + 2 + n);
    return !name.empty();
}

std::string account_words(const std::string& name, const std::string& host) {
    return curly(host.empty() ? name : name + "@" + host);
}

std::string check_account_name(Dialect d, const std::string& name, const std::string& host) {
    auto bad = [](const std::string& what, const std::string& s, std::size_t most) -> std::string {
        if (s.empty()) return what + " can't be empty";
        if (std::isspace(static_cast<unsigned char>(s.front())) || std::isspace(static_cast<unsigned char>(s.back())))
            return what + " can't start or end with a space";
        for (unsigned char ch : s) {
            if (ch < 0x20 || ch == 0x7f) return what + " can't hold a control character";
            if (ch == '\'' || ch == '"' || ch == '`' || ch == '\\' || ch == '@')
                return what + " can't hold ' \" ` \\ or @";
        }
        if (s.size() > most) return what + " can be at most " + std::to_string(most) + " letters";
        return {};
    };
    if (auto w = bad("The account's name", name, d == Dialect::MariaDB ? 32 : 63); !w.empty()) return w;
    if (d == Dialect::MariaDB)
        if (auto w = bad("The host (% is any host)", host, 255); !w.empty()) return w;
    return {};
}

// ── the SQL: one place, shared by the preview and put() ─────────────────────
std::vector<std::string> account_sql(Dialect d, char q, const std::string& database,
                                     const std::optional<AccountDef>& live, const std::optional<AccountDef>& want,
                                     const std::optional<std::string>& password, bool hide_password) {
    std::vector<std::string> out;
    const AccountDef* a = want ? &*want : live ? &*live : nullptr;
    if (!a || d == Dialect::SQLite) return out;
    const bool maria = d == Dialect::MariaDB;
    const std::string who = maria ? lit(d, a->name) + "@" + lit(d, a->host) : quote_ident_with(q, a->name);
    auto on = [&](const std::string& table) {
        if (!maria) return quote_ident_with(q, table);
        return table.empty() ? quote_ident_with(q, db_pattern(database)) + ".*"
                             : quote_ident_with(q, database) + "." + quote_ident_with(q, table);
    };
    auto grant  = [&](unsigned p, const std::string& t) { if (p) out.push_back("GRANT " + privs_sql(p) + " ON " + on(t) + " TO " + who); };
    auto revoke = [&](unsigned p, const std::string& t) { if (p) out.push_back("REVOKE " + privs_sql(p) + " ON " + on(t) + " FROM " + who); };
    const std::string pw = !password ? std::string{} : hide_password ? std::string("'********'") : lit(d, *password);

    if (!live && want) {   // made: then what it may do here
        if (maria) out.push_back("CREATE USER " + who + (password ? " IDENTIFIED BY " + pw : std::string(" ACCOUNT LOCK")));
        else out.push_back("CREATE ROLE " + who + (want->login ? " LOGIN" : " NOLOGIN") + (password ? " PASSWORD " + pw : std::string{}));
        if (maria) grant(want->every, "");
        for (const auto& [t, p] : want->tables) grant(p, t);
        return out;
    }
    if (live && !want) {   // gone: PostgreSQL won't drop a role that still holds grants
        if (maria) { out.push_back("DROP USER " + who); return out; }
        for (const auto& [t, p] : live->tables) revoke(p, t);
        out.push_back("DROP ROLE " + who);
        return out;
    }
    if (!maria && live->login != want->login) out.push_back("ALTER ROLE " + who + (want->login ? " LOGIN" : " NOLOGIN"));
    if (maria) {
        revoke(live->every & ~want->every, "");
        grant(want->every & ~live->every, "");
    }
    std::set<std::string> tables;
    for (const auto& [t, p] : live->tables) tables.insert(t);
    for (const auto& [t, p] : want->tables) tables.insert(t);
    for (const auto& t : tables) {
        auto lp = live->tables.find(t), wp = want->tables.find(t);
        const unsigned l = lp == live->tables.end() ? 0 : lp->second, w = wp == want->tables.end() ? 0 : wp->second;
        revoke(l & ~w, t);
        grant(w & ~l, t);
    }
    return out;
}

// ── the store ───────────────────────────────────────────────────────────────
bool Store::is_own_account(const std::string& name, const std::string& host) const {
    if (m_dialect == Dialect::MariaDB) {
        auto r = m_conn.query("SELECT CURRENT_USER()");
        if (!r || r->rows.empty()) return true;   // can't tell: refuse (fail closed)
        const auto me = parse_grantee(text_of(r->rows[0][0]));
        return me.first == name && me.second == host;
    }
    if (m_dialect == Dialect::PostgreSQL) {
        auto r = m_conn.query("SELECT current_user::text, session_user::text");
        if (!r || r->rows.empty()) return true;
        return text_of(r->rows[0][0]) == name || text_of(r->rows[0][1]) == name;
    }
    return false;
}

std::string Store::database_name() const {
    auto r = m_conn.query(m_dialect == Dialect::MariaDB ? "SELECT DATABASE()" : "SELECT current_database()");
    return r && !r->rows.empty() ? text_of(r->rows[0][0]) : std::string{};
}

std::optional<Record> Store::get_account(const Id& id) const {
    std::string name, host;
    if (!parse_account_id(id, name, host) || m_dialect == Dialect::SQLite) return std::nullopt;
    AccountDef a;
    a.name = name;
    a.host = host;
    if (m_dialect == Dialect::MariaDB) {
        auto u = m_conn.query("SELECT 1 FROM mysql.user WHERE User = ? AND Host = ?", {db::Value(name), db::Value(host)});
        if (!u) { fail(u.error().code, "can't read the server's accounts: " + u.error().message); return std::nullopt; }
        if (u->rows.empty()) return std::nullopt;
        const std::string grantee = "'" + name + "'@'" + host + "'";
        auto s = m_conn.query("SELECT PRIVILEGE_TYPE FROM information_schema.SCHEMA_PRIVILEGES "
                              "WHERE GRANTEE = ? AND TABLE_SCHEMA = ?", {db::Value(grantee), db::Value(db_pattern(database_name()))});
        if (!s) { fail(s.error().code, s.error().message); return std::nullopt; }
        for (const auto& r : s->rows) a.every |= priv_of(text_of(r[0]));
        auto t = m_conn.query("SELECT TABLE_NAME, PRIVILEGE_TYPE FROM information_schema.TABLE_PRIVILEGES "
                              "WHERE GRANTEE = ? AND TABLE_SCHEMA = DATABASE()", {db::Value(grantee)});
        if (!t) { fail(t.error().code, t.error().message); return std::nullopt; }
        for (const auto& r : t->rows) a.tables[text_of(r[0])] |= priv_of(text_of(r[1]));
    } else {
        auto u = m_conn.query("SELECT rolcanlogin::int FROM pg_roles WHERE rolname = ?", {db::Value(name)});
        if (!u) { fail(u.error().code, "can't read the server's roles: " + u.error().message); return std::nullopt; }
        if (u->rows.empty()) return std::nullopt;
        a.login = yes(u->rows[0][0]);
        auto t = m_conn.query(
            "SELECT c.relname, a.privilege_type FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
            "CROSS JOIN LATERAL aclexplode(c.relacl) a JOIN pg_roles r ON r.oid = a.grantee "
            "WHERE n.nspname = current_schema() AND c.relkind IN ('r','p','v','m') AND r.rolname = ?", {db::Value(name)});
        if (!t) { fail(t.error().code, t.error().message); return std::nullopt; }
        for (const auto& r : t->rows) a.tables[text_of(r[0])] |= priv_of(text_of(r[1]));
    }
    std::erase_if(a.tables, [](const auto& kv) { return kv.second == 0; });
    return a;
}

void Store::put_account(const Id& id, const AccountDef* v) {
    const auto pw = std::exchange(m_password, std::nullopt);   // one put, then gone
    if (m_dialect == Dialect::SQLite) { fail(Error::Code::Unsupported, kNoAccounts); return; }
    std::string name, host;
    if (!parse_account_id(id, name, host)) { fail(Error::Code::Internal, "not an account: " + id); return; }
    if (v && (v->name != name || v->host != host)) { fail(Error::Code::Internal, id + " given another account"); return; }
    if (is_own_account(name, host)) { fail(Error::Code::Unsupported, kOwn); return; }
    const auto got = get_account(id);
    if (failed()) return;
    const std::optional<AccountDef> live = got ? std::optional<AccountDef>(std::get<AccountDef>(*got)) : std::nullopt;
    const std::optional<AccountDef> want = v ? std::optional<AccountDef>(*v) : std::nullopt;
    const bool making = !live && want;
    const auto sql = account_sql(m_dialect, m_conn.quote_ident("x").front(), database_name(), live, want,
                                 making ? pw : std::nullopt, false);
    for (std::size_t i = 0; i < sql.size(); ++i) {
        if (exec(sql[i])) continue;
        // MariaDB can't roll an account back: one it just made goes again.
        if (making && i > 0 && m_dialect == Dialect::MariaDB)
            (void)m_conn.query("DROP USER " + lit(m_dialect, name) + "@" + lit(m_dialect, host));
        return;
    }
}

// ── the verbs ───────────────────────────────────────────────────────────────
namespace verbs {

namespace {
std::expected<AccountDef, Error> live_account(Journal& j, const Store::Id& id) {
    auto& s = j.store();
    if (s.dialect() == Dialect::SQLite) return std::unexpected(refused(kNoAccounts));
    std::string name, host;
    if (!Store::parse_account_id(id, name, host)) return std::unexpected(Error{Error::Code::Internal, "not an account: " + id});
    s.clear_failed();
    auto got = s.get(id);
    if (s.failed()) { Error e = s.error(); s.clear_failed(); return std::unexpected(e); }
    if (!got) return std::unexpected(Error{Error::Code::NotFound, "there's no account " + account_words(name, host) + " now"});
    return std::get<AccountDef>(*got);
}
}  // namespace

std::expected<void, Error> new_account(Journal& j, const std::string& name, const std::string& host,
                                       const std::string& password) {
    auto& s = j.store();
    const Dialect d = s.dialect();
    if (d == Dialect::SQLite) return std::unexpected(refused(kNoAccounts));
    const std::string h = d == Dialect::MariaDB ? host : std::string{};
    if (auto why = check_account_name(d, name, h); !why.empty()) return std::unexpected(refused(why));
    if (password.empty()) return std::unexpected(refused("a new account needs a password"));
    const auto id = Store::account_id(name, h);
    s.clear_failed();
    auto got = s.get(id);
    if (s.failed()) { Error e = s.error(); s.clear_failed(); return std::unexpected(e); }
    if (got) return std::unexpected(Error{Error::Code::Exists, "there's already an account " + account_words(name, h)});
    AccountDef a;
    a.name = name;
    a.host = h;
    s.set_password(password);
    auto r = j.run("New Account " + account_words(name, h), {id}, [&](Store& st) { st.put(id, a); });
    s.set_password(std::nullopt);   // a refusal before the put leaves nothing behind
    return r;
}

std::string rights_label(const AccountDef& a, const std::string& table, unsigned privs, bool on) {
    const std::string where = table.empty() ? std::string("every table") : curly(table);
    const std::string who = account_words(a.name, a.host);
    return on ? "Give " + who + " " + privs_says(privs) + " on " + where
              : "Take " + privs_says(privs) + " on " + where + " from " + who;
}

std::expected<AccountDef, Error> rights_wanted(Journal& j, const Store::Id& id, const std::string& table,
                                               unsigned privs, bool on) {
    auto live = live_account(j, id);
    if (!live) return live;
    privs &= kAllPrivs;
    if (!privs) return std::unexpected(Error{Error::Code::Internal, "no right named"});
    auto& s = j.store();
    if (s.is_own_account(live->name, live->host)) return std::unexpected(refused(kOwn));
    auto sch = s.connection().introspect();
    if (!sch) return std::unexpected(sch.error());
    AccountDef w = *live;
    auto set = [&](const std::string& t) {
        unsigned& p = w.tables[t];
        p = on ? (p | privs) : (p & ~privs);
    };
    if (table.empty()) {
        if (s.dialect() == Dialect::MariaDB) w.every = on ? (w.every | privs) : (w.every & ~privs);
        else for (const auto& t : sch->tables) set(t.name);
    } else {
        if (!sch->table(table)) return std::unexpected(Error{Error::Code::NotFound, "there's no table " + curly(table) + " now"});
        set(table);
    }
    std::erase_if(w.tables, [](const auto& kv) { return kv.second == 0; });
    return w;
}

std::expected<void, Error> set_rights(Journal& j, const Store::Id& id, const std::string& table, unsigned privs,
                                      bool on) {
    auto live = live_account(j, id);
    if (!live) return std::unexpected(live.error());
    auto w = rights_wanted(j, id, table, privs, on);
    if (!w) return std::unexpected(w.error());
    if (*w == *live) return {};   // already so: no step
    const AccountDef want = *w;
    return j.run(rights_label(*live, table, privs & kAllPrivs, on), {id}, [&](Store& st) { st.put(id, want); });
}

std::expected<void, Error> delete_account(Journal& j, const Store::Id& id) {
    auto live = live_account(j, id);
    if (!live) return std::unexpected(live.error());
    auto& s = j.store();
    if (s.is_own_account(live->name, live->host)) return std::unexpected(refused(kOwn));
    if (auto list = read_accounts(s.connection(), s.dialect(), Schema{}))
        for (const auto& a : list->accounts)
            if (a.name == live->name && a.host == live->host && (a.super || a.admin))
                return std::unexpected(refused(account_words(a.name, a.host) + " makes accounts itself -- Acorn leaves "
                                               "deleting an account like that to the server's own tools"));
    return j.run("Delete Account " + account_words(live->name, live->host), {id},
                 [&](Store& st) { st.put(id, std::nullopt); });
}

std::expected<std::vector<std::string>, Error> account_preview(Journal& j, const Store::Id& id,
                                                               const std::optional<AccountDef>& want) {
    auto& s = j.store();
    if (s.dialect() == Dialect::SQLite) return std::unexpected(refused(kNoAccounts));
    s.clear_failed();
    auto got = s.get(id);
    if (s.failed()) { Error e = s.error(); s.clear_failed(); return std::unexpected(e); }
    const std::optional<AccountDef> live = got ? std::optional<AccountDef>(std::get<AccountDef>(*got)) : std::nullopt;
    const bool making = !live && want;
    return account_sql(s.dialect(), s.connection().quote_ident("x").front(), s.database_name(), live, want,
                       making ? std::optional<std::string>("x") : std::nullopt, true);
}

}  // namespace verbs
}  // namespace acorn::db
