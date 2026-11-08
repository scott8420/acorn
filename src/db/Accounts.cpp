#include "db/Accounts.hpp"

#include <algorithm>
#include <cctype>

// db/Accounts.cpp -- the engine's accounts, read (s019a). See the header.

namespace acorn::db {

namespace {
std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
std::string text_of(const Value& v) {
    if (const auto* t = v.as_text()) return *t;
    if (auto i = v.as_int()) return std::to_string(*i);
    return {};
}
bool yes(const Value& v) {
    const std::string t = upper(text_of(v));
    return t == "1" || t == "T" || t == "TRUE" || t == "Y" || t == "YES";
}
std::vector<std::string> table_names(const Schema& s) {
    std::vector<std::string> out;
    for (const auto& t : s.tables) out.push_back(t.name);
    return out;
}
void sort_accounts(std::vector<Account>& v) {
    std::sort(v.begin(), v.end(), [](const Account& a, const Account& b) {
        return a.name != b.name ? a.name < b.name : a.host < b.host;
    });
}

// ── MariaDB ────────────────────────────────────────────────────────────────
std::expected<AccountList, Error> read_mariadb(Connection& c, const Schema& s) {
    AccountList out;
    auto me = c.query("SELECT CURRENT_USER()");
    if (!me) return std::unexpected(me.error());
    const auto you = parse_grantee(me->rows.empty() ? std::string{} : text_of(me->rows[0][0]));
    std::vector<Account> list;
    auto users = c.query("SELECT User, Host FROM mysql.user ORDER BY User, Host");
    if (users) {
        for (const auto& r : users->rows) {
            Account a;
            a.name = text_of(r[0]);
            a.host = text_of(r[1]);
            if (a.name.empty()) continue;   // the anonymous account
            list.push_back(a);
        }
    }
    if (!users || list.empty()) {
        out.only_you = true;
        list.clear();
        Account a;
        a.name = you.first;
        a.host = you.second;
        list.push_back(a);
    }
    auto find = [&](const std::pair<std::string, std::string>& who) -> Account* {
        for (auto& a : list) if (a.name == who.first && a.host == who.second) return &a;
        return nullptr;
    };
    Privs pub_every = 0;
    std::map<std::string, Privs> pub_tables;
    // Server-wide, then this database (a grant on a pattern counts when the name matches it).
    for (const char* sql : {"SELECT GRANTEE, PRIVILEGE_TYPE, IS_GRANTABLE FROM information_schema.USER_PRIVILEGES",
                            "SELECT GRANTEE, PRIVILEGE_TYPE, IS_GRANTABLE FROM information_schema.SCHEMA_PRIVILEGES "
                            "WHERE DATABASE() LIKE TABLE_SCHEMA"}) {
        auto r = c.query(sql);
        if (!r) return std::unexpected(r.error());
        const bool server = std::string(sql).find("USER_PRIVILEGES") != std::string::npos;
        for (const auto& row : r->rows) {
            const auto who = parse_grantee(text_of(row[0]));
            const std::string what = upper(text_of(row[1]));
            if (upper(who.first) == "PUBLIC") { pub_every |= priv_of(what); continue; }
            Account* a = find(who);
            if (!a) continue;
            a->every |= priv_of(what);
            if (what == "CREATE USER" || (server && yes(row[2]))) a->admin = true;
            if (server && what == "SUPER") a->super = true;
        }
    }
    auto tp = c.query("SELECT GRANTEE, TABLE_NAME, PRIVILEGE_TYPE FROM information_schema.TABLE_PRIVILEGES "
                      "WHERE TABLE_SCHEMA = DATABASE()");
    if (!tp) return std::unexpected(tp.error());
    for (const auto& row : tp->rows) {
        const auto who = parse_grantee(text_of(row[0]));
        const std::string table = text_of(row[1]);
        const Privs p = priv_of(text_of(row[2]));
        if (upper(who.first) == "PUBLIC") { pub_tables[table] |= p; continue; }
        if (Account* a = find(who)) a->tables[table] |= p;
    }
    for (auto& a : list) {
        a.you = a.name == you.first && a.host == you.second;
        a.every |= pub_every;
        for (const auto& t : table_names(s)) {
            Privs& p = a.tables[t];
            p |= a.every;
            if (auto it = pub_tables.find(t); it != pub_tables.end()) p |= it->second;
        }
    }
    sort_accounts(list);
    out.accounts = std::move(list);
    return out;
}

// ── PostgreSQL ─────────────────────────────────────────────────────────────
std::expected<AccountList, Error> read_postgres(Connection& c, const Schema& s) {
    AccountList out;
    auto roles = c.query("SELECT rolname, rolsuper::int, rolcanlogin::int, rolcreaterole::int, "
                         "has_database_privilege(oid, current_database(), 'CONNECT')::int, "
                         "(rolname = current_user)::int FROM pg_roles WHERE rolname !~ '^pg_' ORDER BY rolname");
    if (!roles) return std::unexpected(roles.error());
    for (const auto& r : roles->rows) {
        Account a;
        a.name = text_of(r[0]);
        a.super = yes(r[1]);
        a.login = yes(r[2]);
        a.admin = a.super || yes(r[3]);
        a.reaches = a.super || yes(r[4]);
        a.you = yes(r[5]);
        out.accounts.push_back(std::move(a));
    }
    auto grants = c.query(
        "SELECT r.rolname, c.relname, "
        "has_table_privilege(r.oid, c.oid, 'SELECT')::int, has_table_privilege(r.oid, c.oid, 'INSERT')::int, "
        "has_table_privilege(r.oid, c.oid, 'UPDATE')::int, has_table_privilege(r.oid, c.oid, 'DELETE')::int "
        "FROM pg_roles r CROSS JOIN pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = current_schema() AND c.relkind IN ('r','p','v','m') AND r.rolname !~ '^pg_'");
    if (!grants) return std::unexpected(grants.error());
    std::map<std::string, Account*> by;
    for (auto& a : out.accounts) by[a.name] = &a;
    for (const auto& row : grants->rows) {
        auto it = by.find(text_of(row[0]));
        if (it == by.end()) continue;
        Privs p = 0;
        if (yes(row[2])) p |= kRead;
        if (yes(row[3])) p |= kAdd;
        if (yes(row[4])) p |= kChange;
        if (yes(row[5])) p |= kDelete;
        it->second->tables[text_of(row[1])] = it->second->reaches ? p : 0;
    }
    for (auto& a : out.accounts) {
        Privs every = kAllPrivs;
        for (const auto& t : table_names(s)) every &= a.tables[t];
        a.every = a.super ? kAllPrivs : every;
        if (a.super) for (auto& [t, p] : a.tables) p = kAllPrivs;
    }
    sort_accounts(out.accounts);
    return out;
}
}  // namespace

std::string privs_says(Privs p) {
    std::string out;
    const std::pair<Privs, const char*> words[] = {{kRead, "read"}, {kAdd, "add"}, {kChange, "change"}, {kDelete, "delete"}};
    for (const auto& [bit, w] : words)
        if (p & bit) out += (out.empty() ? "" : " · ") + std::string(w);
    return out.empty() ? "nothing" : out;
}

Privs priv_of(const std::string& w) {
    const std::string u = upper(w);
    if (u == "SELECT") return kRead;
    if (u == "INSERT") return kAdd;
    if (u == "UPDATE") return kChange;
    if (u == "DELETE") return kDelete;
    return 0;
}

std::string account_name(const Account& a) { return a.host.empty() ? a.name : a.name + "@" + a.host; }

std::string account_tags(const Account& a) {
    std::vector<std::string> t;
    if (a.you) t.push_back("you");
    if (a.super) t.push_back("may do everything");
    else if (a.admin) t.push_back("makes accounts");
    if (!a.login) t.push_back("can't log in");
    if (!a.reaches) t.push_back("can't connect here");
    std::string out;
    for (const auto& s : t) out += (out.empty() ? "" : " · ") + s;
    return out;
}

std::vector<std::string> account_lines(const Account& a, const std::vector<std::string>& tables) {
    auto of = [&](const std::string& t) -> Privs {
        auto it = a.tables.find(t);
        return (it == a.tables.end() ? 0 : it->second) | a.every;
    };
    if (tables.empty()) return {"No tables here yet -- " + std::string(a.every ? "every table: " + privs_says(a.every) : "no rights here")};
    bool alike = true;
    for (const auto& t : tables) alike = alike && of(t) == of(tables.front());
    if (alike) return {(of(tables.front()) ? "Every table: " + privs_says(of(tables.front())) : std::string("Nothing in this database"))};
    std::vector<std::string> out;
    for (const auto& t : tables) out.push_back(t + ": " + privs_says(of(t)));
    return out;
}

std::pair<std::string, std::string> parse_grantee(const std::string& g) {
    const auto at = g.rfind('@');
    std::string user = at == std::string::npos ? g : g.substr(0, at);
    std::string host = at == std::string::npos ? std::string{} : g.substr(at + 1);
    auto unq = [](std::string s) {
        if (s.size() >= 2 && (s.front() == '\'' || s.front() == '`' || s.front() == '"') && s.back() == s.front())
            s = s.substr(1, s.size() - 2);
        return s;
    };
    return {unq(user), unq(host)};
}

std::expected<AccountList, Error> read_accounts(Connection& c, Dialect d, const Schema& s) {
    if (d == Dialect::SQLite) {
        AccountList out;
        out.supported = false;
        out.why = "A SQLite file has no accounts: whoever can open the file can read and change everything in it "
                  "-- the file's own permissions decide who that is.";
        return out;
    }
    return d == Dialect::MariaDB ? read_mariadb(c, s) : read_postgres(c, s);
}

}  // namespace acorn::db
