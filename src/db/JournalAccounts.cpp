#include "db/Journal.hpp"
#include "db/Accounts.hpp"

#include <algorithm>
#include <cctype>
#include <random>
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

// s021: MariaDB's secret is "plugin\nauthentication_string". A plugin name is
// a word (it goes into the SQL bare); anything else isn't put back.
std::pair<std::string, std::string> maria_secret(const std::string& s) {
    const auto nl = s.find('\n');
    if (nl == std::string::npos) return {};
    std::string plugin = s.substr(0, nl);
    for (unsigned char ch : plugin)
        if (!std::isalnum(ch) && ch != '_') return {};
    return {plugin, s.substr(nl + 1)};
}

// A password no one knows -- PostgreSQL's stand-in when the stored one can't
// be read (a role made again by Ctrl+Z). 32 hex digits, never shown or kept.
std::string unknowable() {
    std::random_device rd;
    std::string out;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) out += hex[rd() & 15u];
    return out;
}
}  // namespace

bool has_password(Dialect d, const AccountDef& a) {
    if (d == Dialect::MariaDB) return !maria_secret(a.secret).second.empty();
    return !a.secret.empty();
}

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
    const std::string masked = "'********'";
    // s021: the stored password put back as the server keeps it (a hash), or
    // "" when there's nothing to say. MariaDB: IDENTIFIED VIA <plugin> USING
    // '<hash>'. PostgreSQL: PASSWORD '<hash>' / NULL / one no one knows.
    auto restore = [&](const AccountDef& a) -> std::string {
        if (maria) {
            const auto [plugin, auth] = maria_secret(a.secret);
            if (plugin.empty()) return {};
            return " IDENTIFIED VIA " + plugin + " USING " + (hide_password && !auth.empty() ? masked : lit(d, auth));
        }
        if (a.secret.empty()) return " PASSWORD NULL";
        if (a.secret == kSecretHidden) return " PASSWORD " + (hide_password ? masked : lit(d, unknowable()));
        return " PASSWORD " + (hide_password ? masked : lit(d, a.secret));
    };

    if (!live && want) {   // made: then what it may do here
        if (maria) {
            // Typed: as typed. Made again (Ctrl+Z of Delete, redo of New):
            // with the hash it had and its lock. No hash known: locked, so no
            // one logs in with an empty password.
            const std::string how = password ? " IDENTIFIED BY " + pw : restore(*want);
            out.push_back("CREATE USER " + who + how + (want->locked || how.empty() ? " ACCOUNT LOCK" : ""));
        } else {
            std::string how = password ? " PASSWORD " + pw : restore(*want);
            if (how == " PASSWORD NULL") how.clear();
            out.push_back("CREATE ROLE " + who + (want->login ? " LOGIN" : " NOLOGIN") + how);
        }
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
    // s021: the password (typed, or the old one put back) and the lock -- one
    // statement on MariaDB, so it lands whole or not at all.
    const bool pw_changes = password || want->secret != live->secret;
    if (maria) {
        std::string how = password ? " IDENTIFIED BY " + pw : pw_changes ? restore(*want) : std::string{};
        if (want->locked != live->locked) how += want->locked ? " ACCOUNT LOCK" : " ACCOUNT UNLOCK";
        if (!how.empty()) out.push_back("ALTER USER " + who + how);
    } else if (pw_changes) {
        out.push_back("ALTER ROLE " + who + (password ? " PASSWORD " + pw : restore(*want)));
    }
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
bool Store::shows_passwords() const {
    if (m_dialect == Dialect::MariaDB) return true;
    if (m_dialect != Dialect::PostgreSQL) return false;
    auto r = m_conn.query("SELECT has_table_privilege('pg_catalog.pg_authid', 'SELECT')::int");
    return r && !r->rows.empty() && yes(r->rows[0][0]);
}

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
        // s021: how it logs in, too -- the stored hash and the lock.
        auto u = m_conn.query("SELECT u.plugin, u.authentication_string, JSON_VALUE(g.Priv, '$.account_locked') "
                              "FROM mysql.user u LEFT JOIN mysql.global_priv g ON g.User = u.User AND g.Host = u.Host "
                              "WHERE u.User = ? AND u.Host = ?", {db::Value(name), db::Value(host)});
        if (!u) { fail(u.error().code, "can't read the server's accounts: " + u.error().message); return std::nullopt; }
        if (u->rows.empty()) return std::nullopt;
        a.secret = text_of(u->rows[0][0]) + "\n" + text_of(u->rows[0][1]);
        a.locked = yes(u->rows[0][2]);
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
        a.secret = kSecretHidden;   // s021: the stored password, when this connection may see it
        if (shows_passwords()) {
            auto p = m_conn.query("SELECT rolpassword FROM pg_authid WHERE rolname = ?", {db::Value(name)});
            if (!p) { fail(p.error().code, p.error().message); return std::nullopt; }
            a.secret = p->rows.empty() ? std::string{} : text_of(p->rows[0][0]);
        }
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
                                 want ? pw : std::nullopt, false);
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

// Your own account, or one that makes accounts: Acorn leaves `what` to the
// server's own tools. "" = it may.
std::string held(Journal& j, const AccountDef& live, const char* what) {
    auto& s = j.store();
    if (s.is_own_account(live.name, live.host)) return kOwn;
    if (auto list = read_accounts(s.connection(), s.dialect(), Schema{}))
        for (const auto& a : list->accounts)
            if (a.name == live.name && a.host == live.host && (a.super || a.admin))
                return account_words(a.name, a.host) + " makes accounts itself -- Acorn leaves " + what +
                       " an account like that to the server's own tools";
    return {};
}

std::expected<void, Error> delete_account(Journal& j, const Store::Id& id) {
    auto live = live_account(j, id);
    if (!live) return std::unexpected(live.error());
    if (auto why = held(j, *live, "deleting"); !why.empty()) return std::unexpected(refused(why));
    return j.run("Delete Account " + account_words(live->name, live->host), {id},
                 [&](Store& st) { st.put(id, std::nullopt); });
}

std::string password_label(const AccountDef& a) {
    return "Set Password for " + account_words(a.name, a.host) + (a.locked ? " and Unlock" : "");
}

std::expected<void, Error> set_password(Journal& j, const Store::Id& id, const std::string& password, bool* stepped) {
    if (stepped) *stepped = false;
    auto live = live_account(j, id);
    if (!live) return std::unexpected(live.error());
    if (password.empty()) return std::unexpected(refused("type a password -- an empty one would let anyone in"));
    if (auto why = held(j, *live, "the password of"); !why.empty()) return std::unexpected(refused(why));
    auto& s = j.store();
    AccountDef want = *live;
    want.locked = false;   // setting a password lets it in
    const auto n = j.size();
    const bool redo_before = j.can_redo();
    s.set_password(password);
    auto r = j.run(password_label(*live), {id}, [&](Store& st) { st.put(id, want); });
    s.set_password(std::nullopt);   // a refusal before the put leaves nothing behind
    // A step was kept iff the list changed: it grew, or a redo was discarded.
    if (r && stepped) *stepped = j.size() != n || (redo_before && !j.can_redo());
    return r;
}

std::expected<void, Error> set_locked(Journal& j, const Store::Id& id, bool lock) {
    auto live = live_account(j, id);
    if (!live) return std::unexpected(live.error());
    if (j.store().dialect() != Dialect::MariaDB)
        return std::unexpected(refused("PostgreSQL has no account lock -- a role no one should log in as is one that "
                                       "can't log in"));
    if (auto why = held(j, *live, lock ? "locking" : "unlocking"); !why.empty()) return std::unexpected(refused(why));
    if (live->locked == lock) return {};   // already so: no step
    if (!lock && !has_password(Dialect::MariaDB, *live))
        return std::unexpected(refused(account_words(live->name, live->host) + " has no password -- unlocked, anyone "
                                       "could log in as it; Set Password unlocks it too"));
    AccountDef want = *live;
    want.locked = lock;
    return j.run((lock ? "Lock " : "Unlock ") + account_words(live->name, live->host), {id},
                 [&](Store& st) { st.put(id, want); });
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

// ── s020c: the Accounts area's read, in one go (off the UI thread) ───────────
namespace acorn::db {

std::string accounts_why(bool writable, const AccountList& l) {
    const Account* me = nullptr;
    for (const auto& a : l.accounts) if (a.you) me = &a;
    return !writable ? "This connection is open read-only -- accounts can't be changed through it."
         : l.only_you ? "Acorn can't read the server's account list through your account, so it changes no accounts here."
         : !me || !(me->admin || me->super)
             ? "Your account " + (me ? account_words(me->name, me->host) : std::string("here")) +
                   " can't make accounts or give rights -- that takes an account that can (the server's admin's)."
             : std::string{};
}

std::string account_hold(const Account& a) {
    return a.you   ? "Your own account -- Acorn doesn't change it from under the connection."
         : a.super ? "May do everything -- there's nothing to give it per table."
                   : std::string{};
}

AccountCards read_account_cards(Connection& c, Dialect d, const Schema& s, Store* st, bool writable,
                                const std::function<bool(std::int64_t)>& keep_going) {
    AccountCards out;
    out.list = read_accounts(c, d, s);
    if (!out.list || !out.list->supported) return out;
    out.why = accounts_why(writable && st, *out.list);
    if (!out.why.empty()) return out;
    out.database = st->database_name();
    out.passwords_kept = st->shows_passwords();   // s021
    std::int64_t n = 0;
    for (const auto& a : out.list->accounts) {
        if (!account_hold(a).empty()) continue;
        if (keep_going && !keep_going(n)) { out.stopped = true; break; }
        const auto id = Store::account_id(a.name, a.host);
        st->clear_failed();
        auto got = st->get(id);
        if (st->failed() || !got)
            out.given.emplace(id, std::unexpected(st->failed() ? st->error().message : std::string("it's gone")));
        else
            out.given.emplace(id, std::get<AccountDef>(*got));
        st->clear_failed();
        ++n;
    }
    return out;
}

}  // namespace acorn::db
