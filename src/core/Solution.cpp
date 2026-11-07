#include "core/Solution.hpp"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

// core/Solution.cpp -- the .acorn pump: encode + decode adjacent (CANON).

namespace acorn::core {

namespace fs = std::filesystem;

std::string solution_path(const SavedConnection& c, const std::string& app_dir, bool beside_ok) {
    if (c.is_file() && beside_ok) {
        fs::path p(c.path);
        p.replace_extension(".acorn");
        return p.string();
    }
    // One file per connection, named from its key (safe characters only).
    std::string name;
    const std::string base = c.is_file() ? fs::path(c.path).stem().string() + "-" + c.key() : c.key();
    for (unsigned char ch : base)
        name += (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') ? static_cast<char>(ch) : '_';
    if (name.size() > 120) name = name.substr(0, 120);
    return (fs::path(app_dir) / "solutions" / (name + ".acorn")).string();
}

bool folder_writable(const std::string& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && ::access(dir.c_str(), W_OK) == 0;
}

std::optional<Solution> load_solution(const std::string& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        if (!j.is_object() || !j.contains("acorn") || !j["acorn"].is_number_integer() ||
            j["acorn"].get<int>() > kSolutionVersion)
            return std::nullopt;
        Solution s;
        if (auto it = j.find("points_at"); it != j.end() && it->is_object()) {
            auto str = [&](const char* k) { return it->contains(k) && (*it)[k].is_string() ? (*it)[k].get<std::string>() : std::string{}; };
            s.points_at.engine = str("engine");
            s.points_at.path = str("path");
            s.points_at.host = str("host");
            s.points_at.database = str("database");
            s.points_at.user = str("user");
            if (it->contains("port") && (*it)["port"].is_number_integer()) s.points_at.port = (*it)["port"].get<int>();
        }
        if (auto it = j.find("boxes"); it != j.end() && it->is_object())
            for (auto& [name, b] : it->items()) {
                if (!b.is_object() || !b.contains("x") || !b.contains("y") || !b["x"].is_number() || !b["y"].is_number())
                    continue;
                s.boxes[name] = {b["x"].get<double>(), b["y"].get<double>()};
            }
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool save_solution(const std::string& file, const Solution& s) {
    nlohmann::json j;
    j["acorn"] = kSolutionVersion;
    nlohmann::json pa;
    pa["engine"] = s.points_at.engine;
    if (s.points_at.is_file()) {
        pa["path"] = s.points_at.path;
    } else {
        pa["host"] = s.points_at.host;
        pa["port"] = s.points_at.port;
        pa["database"] = s.points_at.database;
        pa["user"] = s.points_at.user;
    }
    j["points_at"] = std::move(pa);
    nlohmann::json boxes = nlohmann::json::object();
    for (const auto& [name, b] : s.boxes) boxes[name] = {{"x", b.x}, {"y", b.y}};
    j["boxes"] = std::move(boxes);
    std::error_code ec;
    const fs::path p(file);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << j.dump(2) << "\n";
        if (!out) { fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, p, ec);   // whole file or the old one, never half
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

}  // namespace acorn::core
