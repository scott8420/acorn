#include "core/Solution.hpp"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

// core/Solution.cpp -- the .acorn pump: encode + decode adjacent (CANON).

namespace acorn::core {

namespace fs = std::filesystem;

namespace {
// A layout's numbers / words, leniently: a wrong type reads as none (the
// layout then fails check_layout and is skipped -- the rest of the file reads).
int num_or_0(const nlohmann::json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? static_cast<int>(it->get<double>()) : 0;
}
std::string str_or_empty(const nlohmann::json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
}  // namespace

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

const Layout* Solution::layout(const std::string& table, const std::string& name) const {
    auto t = layouts.find(table);
    if (t == layouts.end()) return nullptr;
    auto n = t->second.find(name);
    return n == t->second.end() ? nullptr : &n->second;
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
        if (auto it = j.find("layouts"); it != j.end() && it->is_object())
            for (auto& [table, named] : it->items()) {
                if (!named.is_object()) continue;
                for (auto& [name, lj] : named.items()) {
                    if (!lj.is_object()) continue;
                    Layout l;
                    l.table = table;
                    l.name = name;
                    l.width = num_or_0(lj, "width");
                    l.height = num_or_0(lj, "height");
                    if (auto pj = lj.find("parts"); pj != lj.end() && pj->is_object()) {   // s010d
                        l.header = num_or_0(*pj, "header");
                        l.footer = num_or_0(*pj, "footer");
                    }
                    if (auto ob = lj.find("objects"); ob != lj.end() && ob->is_array())
                        for (const auto& oj : *ob) {
                            if (!oj.is_object()) continue;
                            LayoutObject o;
                            const std::string kind = str_or_empty(oj, "kind");
                            if (kind == "field") o.kind = ObjectKind::Field;
                            else if (kind == "text") o.kind = ObjectKind::Text;
                            else if (kind == "line") o.kind = ObjectKind::Line;   // s010c
                            else if (kind == "box")  o.kind = ObjectKind::Box;
                            else if (kind == "portal") o.kind = ObjectKind::Portal;   // s011a
                            else if (kind == "related") o.kind = ObjectKind::Related; // s011d
                            else continue;   // a kind this Acorn doesn't know: not drawn, not invented
                            o.field = str_or_empty(oj, "field");
                            o.text = str_or_empty(oj, "text");
                            o.align = parse_align(str_or_empty(oj, "align"));   // s012b: missing -> right
                            o.x = num_or_0(oj, "x");
                            o.y = num_or_0(oj, "y");
                            o.w = num_or_0(oj, "w");
                            o.h = num_or_0(oj, "h");
                            if (o.kind == ObjectKind::Related) {
                                o.from = str_or_empty(oj, "from");
                                o.shows = str_or_empty(oj, "shows");
                            }
                            if (o.kind == ObjectKind::Portal) {
                                o.from = str_or_empty(oj, "from");
                                if (auto cj = oj.find("columns"); cj != oj.end() && cj->is_array())
                                    for (const auto& c : *cj) if (c.is_string()) o.columns.push_back(c.get<std::string>());
                            }
                            l.objects.push_back(std::move(o));
                        }
                    if (!check_layout(l).empty()) continue;   // never half a layout
                    s.layouts[table][name] = std::move(l);
                }
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
    nlohmann::json layouts = nlohmann::json::object();
    for (const auto& [table, named] : s.layouts)
        for (const auto& [name, l] : named) {
            nlohmann::json objects = nlohmann::json::array();
            for (const auto& o : l.objects) {
                nlohmann::json oj;
                if (o.kind == ObjectKind::Field)     { oj["kind"] = "field"; oj["field"] = o.field; }
                else if (o.kind == ObjectKind::Text) {
                    oj["kind"] = "text";  oj["text"] = o.text;
                    if (o.align != Align::Right) oj["align"] = align_name(o.align);   // s012b: only when not the default
                }
                else if (o.kind == ObjectKind::Portal) {   // s011a
                    oj["kind"] = "portal"; oj["from"] = o.from; oj["field"] = o.field; oj["columns"] = o.columns;
                }
                else if (o.kind == ObjectKind::Related) {   // s011d
                    oj["kind"] = "related"; oj["field"] = o.field; oj["from"] = o.from; oj["shows"] = o.shows;
                }
                else oj["kind"] = o.kind == ObjectKind::Line ? "line" : "box";   // s010c: drawing has only a place
                oj["x"] = o.x; oj["y"] = o.y; oj["w"] = o.w; oj["h"] = o.h;
                objects.push_back(std::move(oj));
            }
            layouts[table][name] = {{"width", l.width}, {"height", l.height}, {"objects", std::move(objects)}};
            if (l.header || l.footer)   // s010d: only when there are parts (an older Acorn reads it as all body)
                layouts[table][name]["parts"] = {{"header", l.header}, {"footer", l.footer}};
        }
    j["layouts"] = std::move(layouts);
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
