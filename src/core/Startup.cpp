#include "core/Startup.hpp"

#include "json.hpp"

#include <filesystem>
#include <fstream>

// core/Startup.cpp -- the startup choice and its little JSON file.

namespace acorn::core {

StartupChoice load_startup(const std::string& file) {
    StartupChoice c;
    std::ifstream in(file);
    if (!in) return c;
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("open_sample") && j["open_sample"].is_boolean()) c.open_sample = j["open_sample"].get<bool>();
    } catch (...) {
        return StartupChoice{};
    }
    return c;
}

bool save_startup(const std::string& file, const StartupChoice& c) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(file);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        nlohmann::json j;
        j["open_sample"] = c.open_sample;
        out << j.dump(2) << "\n";
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);   // whole file or the old one, never half
    return !ec;
}

}  // namespace acorn::core
