#include "core/Recents.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace acorn::core {

namespace {
std::string normalise(const std::string& path) {
    std::string s = path;
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}
}  // namespace

void recents_add(std::vector<std::string>& list, const std::string& path,
                 std::size_t max) {
    const std::string key = normalise(path);
    if (key.empty()) return;
    auto it = std::find(list.begin(), list.end(), key);
    if (it != list.end()) list.erase(it);         // move-to-front
    list.insert(list.begin(), key);
    if (max >= 1 && list.size() > max) list.resize(max);
}

void recents_remove(std::vector<std::string>& list, const std::string& path) {
    const std::string key = normalise(path);
    auto it = std::find(list.begin(), list.end(), key);
    if (it != list.end()) list.erase(it);
}

std::vector<std::string> load_recents(const std::string& file) {
    std::vector<std::string> out;
    std::ifstream f(file);
    if (!f) return out;                            // first run / unreadable -> empty
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("paths") && j["paths"].is_array()) {
            for (const auto& e : j["paths"]) {
                if (!e.is_string()) continue;
                std::string p = normalise(e.get<std::string>());
                if (p.empty()) continue;
                std::error_code ec;
                if (!fs::exists(p, ec)) continue;  // prune stale (single point)
                if (std::find(out.begin(), out.end(), p) != out.end()) continue;  // dedupe
                out.push_back(std::move(p));
            }
        }
    } catch (const std::exception&) {
        out.clear();                               // unparseable -> empty, never throw across the seam
    }
    return out;
}

bool save_recents(const std::string& file, const std::vector<std::string>& list) {
    try {
        fs::create_directories(fs::path(file).parent_path());
    } catch (const std::exception&) {
        return false;
    }
    nlohmann::json j;
    j["paths"] = list;
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace acorn::core
