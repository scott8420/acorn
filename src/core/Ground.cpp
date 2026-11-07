#include "core/Ground.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

// core/Ground.cpp -- see the header.

namespace acorn::core {

namespace {

struct Hsl { double h = 0, s = 0, l = 0; };   // h 0..360, s / l 0..1

Hsl to_hsl(const Rgb& c) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    Hsl o;
    o.l = (mx + mn) / 2;
    const double d = mx - mn;
    if (d < 1e-9) return o;
    o.s = o.l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r)      o.h = std::fmod((g - b) / d + 6, 6);
    else if (mx == g) o.h = (b - r) / d + 2;
    else              o.h = (r - g) / d + 4;
    o.h *= 60;
    return o;
}

Rgb from_hsl(const Hsl& x) {
    const double c = (1 - std::fabs(2 * x.l - 1)) * x.s;
    const double hp = x.h / 60;
    const double xx = c * (1 - std::fabs(std::fmod(hp, 2) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1)      { r = c; g = xx; }
    else if (hp < 2) { r = xx; g = c; }
    else if (hp < 3) { g = c; b = xx; }
    else if (hp < 4) { g = xx; b = c; }
    else if (hp < 5) { r = xx; b = c; }
    else             { r = c; b = xx; }
    const double m = x.l - c / 2;
    const auto ch = [&](double v) { return std::clamp(static_cast<int>(std::lround((v + m) * 255)), 0, 255); };
    return {ch(r), ch(g), ch(b)};
}

int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<Rgb> parse_hex(const std::string& in) {
    std::string s = in;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6) return std::nullopt;
    int v[6];
    for (int i = 0; i < 6; ++i)
        if ((v[i] = hexval(s[i])) < 0) return std::nullopt;
    return Rgb{v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]};
}

std::string to_hex(const Rgb& c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r & 0xff, c.g & 0xff, c.b & 0xff);
    return buf;
}

const std::vector<GroundPreset>& ground_presets() {
    // GNOME's accent colours (libadwaita 1.6 / GNOME 47), in Settings' order.
    static const std::vector<GroundPreset> kPresets = {
        {"blue",   "Blue",   {0x35, 0x84, 0xe4}},
        {"teal",   "Teal",   {0x21, 0x90, 0xa4}},
        {"green",  "Green",  {0x3a, 0x94, 0x4a}},
        {"yellow", "Yellow", {0xc8, 0x88, 0x00}},
        {"orange", "Orange", {0xed, 0x5b, 0x00}},
        {"red",    "Red",    {0xe6, 0x2d, 0x42}},
        {"pink",   "Pink",   {0xd5, 0x61, 0x99}},
        {"purple", "Purple", {0x91, 0x41, 0xac}},
        {"slate",  "Slate",  {0x6f, 0x83, 0x96}},
    };
    return kPresets;
}

const GroundPreset* ground_preset(const std::string& id) {
    for (const auto& p : ground_presets())
        if (p.id == id) return &p;
    return nullptr;
}

Palette derive(const Rgb& base) {
    // The recipe is the s004 Forest skin, measured: gradient top L .24 / S .49,
    // bottom L .088 / S .51, panel L .115 / S .32, ink L .935 / S .25, dim ink
    // L .70 / S .12 -- all on the base's hue. Saturation scales with the base's
    // own, so a grey base gives a grey skin and a vivid one stays calm.
    const Hsl b = to_hsl(base);
    const double k = std::clamp(b.s / 0.6, 0.0, 1.0);
    const auto at = [&](double s, double l) { return from_hsl({b.h, s * k, l}); };
    return {at(0.49, 0.24), at(0.51, 0.088), at(0.32, 0.115), at(0.25, 0.935), at(0.12, 0.70)};
}

std::string ground_css(const Palette& p) {
    return "@define-color acorn_forest_hi " + to_hex(p.hi) + ";\n"
           "@define-color acorn_forest_lo " + to_hex(p.lo) + ";\n"
           "@define-color acorn_panel " + to_hex(p.panel) + ";\n"
           "@define-color acorn_ink " + to_hex(p.ink) + ";\n"
           "@define-color acorn_ink_dim " + to_hex(p.ink_dim) + ";\n";
}

Rgb GroundChoice::base() const {
    if (id == "custom") return custom;
    if (auto* p = ground_preset(id)) return p->base;
    return ground_preset(kDefaultGround)->base;
}

GroundChoice load_ground(const std::string& file) {
    GroundChoice g;
    std::ifstream in(file);
    if (!in) return g;
    try {
        nlohmann::json j;
        in >> j;
        const std::string id = j.value("ground", std::string{});
        const auto custom = parse_hex(j.value("custom", std::string{}));
        if (custom) g.custom = *custom;
        if (id == "custom" ? custom.has_value() : ground_preset(id) != nullptr) g.id = id;
    } catch (...) {
        return GroundChoice{};
    }
    return g;
}

bool save_ground(const std::string& file, const GroundChoice& g) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(file);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        nlohmann::json j;
        j["ground"] = g.id;
        j["custom"] = to_hex(g.custom);
        out << j.dump(2) << "\n";
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);   // whole file or the old one, never half
    return !ec;
}

}  // namespace acorn::core
