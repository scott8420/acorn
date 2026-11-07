#pragma once
// core::Ground -- the skin's base colour, chosen by the user (s005c).
//
// The skin (resources/acorn.css) runs off a handful of named colours. Five of
// them come from the GROUND: the window gradient's top and bottom, the calm
// panel under the data, and the two inks (text, dim text), each tinted toward
// the ground's hue. The amber accent stays amber on every ground.
//
// The presets are GNOME's own accent colours (Settings -> Appearance), so a
// ground can match the desktop; Custom is any colour from a picker. Every
// palette comes out of ONE recipe -- derive() -- taken from the original
// Forest skin's lightness / saturation, so each ground is as calm and as
// legible as Forest was, and a grey base (Slate) stays grey.
//
// GTK-free: the UI turns css() into a style provider and resolves the XDG
// path; the core does the colour arithmetic and the file.
#include <optional>
#include <string>
#include <vector>

namespace acorn::core {

struct Rgb {
    int r = 0, g = 0, b = 0;   // 0..255
    bool operator==(const Rgb&) const = default;
};

std::optional<Rgb> parse_hex(const std::string& s);   // "#3584e4" / "3584e4"
std::string        to_hex(const Rgb& c);              // "#3584e4"

struct GroundPreset {
    std::string id;     // "slate" -- what the file and the menu target hold
    std::string name;   // "Slate"
    Rgb         base;
};
const std::vector<GroundPreset>& ground_presets();    // GNOME's accent order
const GroundPreset*              ground_preset(const std::string& id);
inline constexpr const char*     kDefaultGround = "slate";

// The five derived colours.
struct Palette {
    Rgb hi, lo, panel, ink, ink_dim;
};
Palette     derive(const Rgb& base);
// "@define-color acorn_forest_hi #..; ..." -- overrides the skin's names.
std::string ground_css(const Palette& p);

// The saved choice: a preset id, or "custom" + the colour.
struct GroundChoice {
    std::string id = kDefaultGround;
    Rgb         custom{0x6f, 0x83, 0x96};
    Rgb base() const;   // the preset's colour, the custom one, or the default's
    bool operator==(const GroundChoice&) const = default;
};
GroundChoice load_ground(const std::string& file);    // missing / garbage -> default
bool         save_ground(const std::string& file, const GroundChoice& g);

}  // namespace acorn::core
