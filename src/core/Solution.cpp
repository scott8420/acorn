#include "core/Solution.hpp"

#include "json.hpp"

#include <algorithm>
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

const Calc* Solution::calc(const std::string& table, const std::string& name) const {
    auto t = calcs.find(table);
    if (t == calcs.end()) return nullptr;
    auto n = t->second.find(name);
    return n == t->second.end() ? nullptr : &n->second;
}

const std::map<std::string, Calc>& Solution::calcs_of(const std::string& table) const {
    static const std::map<std::string, Calc> none;
    auto t = calcs.find(table);
    return t == calcs.end() ? none : t->second;
}

const Summary* Solution::summary(const std::string& table, const std::string& name) const {
    auto t = summaries.find(table);
    if (t == summaries.end()) return nullptr;
    auto n = t->second.find(name);
    return n == t->second.end() ? nullptr : &n->second;
}

const std::map<std::string, Summary>& Solution::summaries_of(const std::string& table) const {
    static const std::map<std::string, Summary> none;
    auto t = summaries.find(table);
    return t == summaries.end() ? none : t->second;
}

const ValueList* Solution::list_exact(const std::string& table, const std::string& name) const {
    auto t = lists.find(table);
    if (t == lists.end()) return nullptr;
    auto n = t->second.find(name);
    return n == t->second.end() ? nullptr : &n->second;
}

const ValueList* Solution::list(const std::string& table, const std::string& name) const {
    if (const auto* v = list_exact(table, name)) return v;
    return table.empty() ? nullptr : list_exact("", name);   // s025b: shared by every table
}

std::map<std::string, ValueList> Solution::lists_for(const std::string& table) const {
    std::map<std::string, ValueList> out = lists_of("");
    for (const auto& [n, v] : lists_of(table)) out[n] = v;   // its own first, were a name ever both
    return out;
}

std::vector<std::string> Solution::list_names_taken(const std::string& table) const {
    std::vector<std::string> out;
    for (const auto& [t, named] : lists)
        if (table.empty() || t.empty() || t == table)
            for (const auto& [n, v] : named) out.push_back(n);
    return out;
}

const std::map<std::string, ValueList>& Solution::lists_of(const std::string& table) const {
    static const std::map<std::string, ValueList> none;
    auto t = lists.find(table);
    return t == lists.end() ? none : t->second;
}

const Script* Solution::script(const std::string& table, const std::string& name) const {
    auto t = scripts.find(table);
    if (t == scripts.end()) return nullptr;
    auto n = t->second.find(name);
    return n == t->second.end() ? nullptr : &n->second;
}

const std::map<std::string, Script>& Solution::scripts_of(const std::string& table) const {
    static const std::map<std::string, Script> none;
    auto t = scripts.find(table);
    return t == scripts.end() ? none : t->second;
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
                        if (auto sj = pj->find("subs"); sj != pj->end() && sj->is_array())   // s015b
                            for (const auto& x : *sj) {
                                if (!x.is_object()) continue;
                                SubSummary sub;
                                sub.by = str_or_empty(x, "by");
                                sub.leading = str_or_empty(x, "when") != "trailing";
                                sub.height = num_or_0(x, "height");
                                l.subs.push_back(std::move(sub));
                            }
                    }
                    if (auto lb = lj.find("labels"); lb != lj.end() && lb->is_object()) {   // s016
                        l.across = std::max(1, num_or_0(*lb, "across"));
                        l.gap = num_or_0(*lb, "gap");
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
                            else if (kind == "button") o.kind = ObjectKind::Button;   // s017b
                            else continue;   // a kind this Acorn doesn't know: not drawn, not invented
                            o.field = str_or_empty(oj, "field");
                            if (o.kind == ObjectKind::Field) o.list = str_or_empty(oj, "list");   // s014c
                            if (o.kind == ObjectKind::Button) o.script = str_or_empty(oj, "script");   // s017b
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
        if (auto it = j.find("calcs"); it != j.end() && it->is_object())   // s014a
            for (auto& [table, named] : it->items()) {
                if (!named.is_object()) continue;
                for (auto& [name, cj] : named.items()) {
                    if (!cj.is_object() || !cj.contains("expr") || !cj["expr"].is_string()) continue;
                    if (!check_calc_name(name).empty()) continue;   // never a name Acorn wouldn't make
                    s.calcs[table][name] = Calc{table, name, cj["expr"].get<std::string>()};
                }
            }
        if (auto it = j.find("summaries"); it != j.end() && it->is_object())   // s014b
            for (auto& [table, named] : it->items()) {
                if (!named.is_object()) continue;
                for (auto& [name, sj] : named.items()) {
                    if (!sj.is_object() || !check_calc_name(name).empty()) continue;
                    const auto op = parse_summary_op(str_or_empty(sj, "op"));
                    const std::string of = str_or_empty(sj, "of");
                    if (!op || of.empty()) continue;   // never half a summary
                    s.summaries[table][name] = Summary{table, name, *op, of};
                }
            }
        if (auto it = j.find("value_lists"); it != j.end() && it->is_object())   // s014c
            for (auto& [table, named] : it->items()) {
                if (!named.is_object()) continue;
                for (auto& [name, vj] : named.items()) {
                    if (!vj.is_object() || !check_value_list_name(name).empty()) continue;
                    ValueList v{table, name, {}, str_or_empty(vj, "from"), str_or_empty(vj, "field"), str_or_empty(vj, "shows")};
                    if (auto it = vj.find("only_from_list"); it != vj.end() && it->is_boolean()) v.strict = it->get<bool>();   // s025a
                    if (auto vs = vj.find("values"); vs != vj.end() && vs->is_array())
                        for (const auto& x : *vs) if (x.is_string() && !x.get<std::string>().empty()) v.values.push_back(x.get<std::string>());
                    if (v.typed() ? v.values.empty() || !v.from.empty() : v.from.empty() || !v.values.empty())
                        continue;   // never half a list (or two kinds at once)
                    s.lists[table][name] = std::move(v);
                }
            }
        if (auto it = j.find("scripts"); it != j.end() && it->is_object())   // s017a
            for (auto& [table, named] : it->items()) {
                if (!named.is_object()) continue;
                for (auto& [name, sj] : named.items()) {
                    if (!sj.is_array() || !check_script_name(name).empty()) continue;
                    Script sc{table, name, {}};
                    bool broken = false;
                    for (const auto& stj : sj) {
                        if (!stj.is_object()) { broken = true; break; }
                        const auto k = parse_step_kind(str_or_empty(stj, "step"));
                        if (!k) { broken = true; break; }   // a step this Acorn doesn't know: never half a script
                        ScriptStep st{*k, str_or_empty(stj, "field"), str_or_empty(stj, "text"), false};
                        if (auto d = stj.find("descending"); d != stj.end() && d->is_boolean()) st.descending = d->get<bool>();
                        sc.steps.push_back(std::move(st));
                    }
                    if (broken || sc.steps.empty()) continue;
                    s.scripts[table][name] = std::move(sc);
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
                if (o.kind == ObjectKind::Field) {
                    oj["kind"] = "field"; oj["field"] = o.field;
                    if (!o.list.empty()) oj["list"] = o.list;   // s014c: only when it has one
                }
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
                else if (o.kind == ObjectKind::Button) {   // s017b
                    oj["kind"] = "button"; oj["text"] = o.text;
                    if (!o.script.empty()) oj["script"] = o.script;   // only when it runs one
                }
                else oj["kind"] = o.kind == ObjectKind::Line ? "line" : "box";   // s010c: drawing has only a place
                oj["x"] = o.x; oj["y"] = o.y; oj["w"] = o.w; oj["h"] = o.h;
                objects.push_back(std::move(oj));
            }
            layouts[table][name] = {{"width", l.width}, {"height", l.height}, {"objects", std::move(objects)}};
            if (l.across > 1)   // s016: only for labels
                layouts[table][name]["labels"] = {{"across", l.across}, {"gap", l.gap}};
            if (l.header || l.footer || !l.subs.empty()) {   // s010d: only when there are parts (an older Acorn reads it as all body)
                layouts[table][name]["parts"] = {{"header", l.header}, {"footer", l.footer}};
                if (!l.subs.empty()) {   // s015b: only when there are some
                    nlohmann::json subs = nlohmann::json::array();
                    for (const auto& x : l.subs)
                        subs.push_back({{"by", x.by}, {"when", x.leading ? "leading" : "trailing"}, {"height", x.height}});
                    layouts[table][name]["parts"]["subs"] = std::move(subs);
                }
            }
        }
    j["layouts"] = std::move(layouts);
    if (!s.calcs.empty()) {   // s014a: only when there are some (an older Acorn never sees the key)
        nlohmann::json calcs = nlohmann::json::object();
        for (const auto& [table, named] : s.calcs)
            for (const auto& [name, c] : named) calcs[table][name] = {{"expr", c.expr}};
        j["calcs"] = std::move(calcs);
    }
    if (!s.summaries.empty()) {   // s014b: only when there are some
        nlohmann::json sums = nlohmann::json::object();
        for (const auto& [table, named] : s.summaries)
            for (const auto& [name, x] : named) sums[table][name] = {{"op", summary_op_name(x.op)}, {"of", x.field}};
        j["summaries"] = std::move(sums);
    }
    if (!s.lists.empty()) {   // s014c: only when there are some
        nlohmann::json lists = nlohmann::json::object();
        for (const auto& [table, named] : s.lists)
            for (const auto& [name, v] : named) {
                nlohmann::json vj = nlohmann::json::object();
                if (v.typed()) vj["values"] = v.values;
                else {
                    vj["from"] = v.from; vj["field"] = v.field;
                    if (!v.shows.empty()) vj["shows"] = v.shows;
                }
                if (v.strict) vj["only_from_list"] = true;   // s025a: only when it says something
                lists[table][name] = std::move(vj);
            }
        j["value_lists"] = std::move(lists);
    }
    if (!s.scripts.empty()) {   // s017a: only when there are some
        nlohmann::json all = nlohmann::json::object();
        for (const auto& [table, named] : s.scripts)
            for (const auto& [name, sc] : named) {
                nlohmann::json steps = nlohmann::json::array();
                for (const auto& st : sc.steps) {
                    nlohmann::json stj = {{"step", step_name(st.kind)}};
                    if (!st.field.empty()) stj["field"] = st.field;
                    if (!st.text.empty()) stj["text"] = st.text;
                    if (st.descending) stj["descending"] = true;
                    steps.push_back(std::move(stj));
                }
                all[table][name] = std::move(steps);
            }
        j["scripts"] = std::move(all);
    }
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
