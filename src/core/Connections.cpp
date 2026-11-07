#include "core/Connections.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace acorn::core {

std::string SavedConnection::key() const {
    if (is_file()) return engine + "|" + path;
    return engine + "|" + user + "@" + (host.empty() ? "localhost" : host) + ":" +
           std::to_string(port) + "/" + database;
}

std::string SavedConnection::label() const {
    if (is_file()) return fs::path(path).filename().string();
    return (database.empty() ? std::string("(server)") : database) + " on " +
           (host.empty() ? std::string("localhost") : host);
}

std::string SavedConnection::detail() const {
    if (is_file()) return path;
    std::string s = user.empty() ? std::string{} : user + "@";
    s += host.empty() ? "localhost" : host;
    if (port > 0) s += ":" + std::to_string(port);
    return s;
}

void connections_add(std::vector<SavedConnection>& list, const SavedConnection& c,
                     std::size_t max) {
    if (c.engine.empty() || (c.path.empty() && c.database.empty() && c.host.empty())) return;
    connections_remove(list, c.key());            // move-to-front
    list.insert(list.begin(), c);
    if (max >= 1 && list.size() > max) list.resize(max);
}

void connections_remove(std::vector<SavedConnection>& list, const std::string& key) {
    std::erase_if(list, [&](const SavedConnection& e) { return e.key() == key; });
}

const SavedConnection* connections_find(const std::vector<SavedConnection>& list,
                                        const std::string& key) {
    for (auto& e : list)
        if (e.key() == key) return &e;
    return nullptr;
}

std::vector<SavedConnection> load_connections(const std::string& file) {
    std::vector<SavedConnection> out;
    std::ifstream f(file);
    if (!f) return out;                            // first run / unreadable -> empty
    try {
        nlohmann::json j;
        f >> j;
        if (!j.contains("connections") || !j["connections"].is_array()) return out;
        for (const auto& e : j["connections"]) {
            if (!e.is_object()) continue;
            SavedConnection c;
            const auto str = [&](const char* k) {
                return e.contains(k) && e[k].is_string() ? e[k].get<std::string>() : std::string{};
            };
            c.engine = str("engine");
            c.path = str("path");
            c.host = str("host");
            c.database = str("database");
            c.user = str("user");
            if (e.contains("port") && e["port"].is_number_integer()) c.port = e["port"].get<int>();
            if (c.engine.empty()) continue;
            if (c.is_file()) {
                std::error_code ec;
                if (!fs::exists(c.path, ec)) continue;   // prune a vanished file
            } else if (c.database.empty() && c.host.empty()) {
                continue;
            }
            if (connections_find(out, c.key())) continue;   // dedupe
            out.push_back(std::move(c));
        }
    } catch (const std::exception&) {
        out.clear();                               // unparseable -> empty, never throw across the seam
    }
    return out;
}

bool save_connections(const std::string& file, const std::vector<SavedConnection>& list) {
    try {
        fs::create_directories(fs::path(file).parent_path());
    } catch (const std::exception&) {
        return false;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : list) {
        nlohmann::json e;
        e["engine"] = c.engine;
        if (c.is_file()) {
            e["path"] = c.path;
        } else {
            e["host"] = c.host;
            e["port"] = c.port;
            e["database"] = c.database;
            e["user"] = c.user;
        }
        arr.push_back(std::move(e));
    }
    nlohmann::json j;
    j["connections"] = std::move(arr);
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace acorn::core
