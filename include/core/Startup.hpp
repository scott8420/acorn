#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// core/Startup -- what Acorn opens when it starts (s010f). By default the
// sample Address Book (made in Acorn's own folder the first time), so a first
// look isn't an empty window. A menu check box turns it off; the choice lives
// in ~/.config/acorn/startup.json. GTK-free, headless-testable.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::core {

struct StartupChoice {
    bool open_sample = true;   // open the sample Address Book at startup
    bool operator==(const StartupChoice&) const = default;
};

StartupChoice load_startup(const std::string& file);   // missing / garbage -> the default (on)
bool          save_startup(const std::string& file, const StartupChoice& c);

}  // namespace acorn::core
