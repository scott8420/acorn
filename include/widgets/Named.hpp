#pragma once
#include "Registry.hpp"

#include <gtkmm/widget.h>
#include <string>
#include <string_view>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
// Named<W> -- the mandatory-name widget wrapper. THE substrate everything sits
// on. (Cairn: distilled from Optik's widgets::Named, which is Pole A of the
// widget-naming fork -- see docs/widget-naming.md.)
//
// Discipline through inheritance (CANON): a GTK widget's name is optional and
// easy to skip, so over hundreds of edits the Inspector tree becomes a forest
// of anonymous nodes and a gdb backtrace frame is a bare 0x55f... . Named<W>
// makes the name MANDATORY in the only constructor and registers the widget in
// the live address book by construction -- the wrong thing (an unnamed widget)
// simply isn't expressible in app code.
//
// The payoff is debuggability, NOT scripting: dotted-readable names
// (`shell.sidebar`, `shell.pages`) so a backtrace frame reads itself and
// registry::dump() prints the live tree. This is deliberately the *light* half
// of the naming lesson -- Cairn has nothing that addresses widgets by name
// programmatically, so it does NOT carry Curvz's abbrev/long-name resolver DB.
// The fork, and when you'd want the other half, is docs/widget-naming.md.
//
// One template, every wrapper an alias -> new wrappers are one-line additions
// as demand appears (CANON: substrate widening follows demand, not symmetry).
// Classes that carry real logic (Shell) name+register themselves directly.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::widgets {

// Opt-out tag for transient per-row content (list rows, thumbnails): named for
// the Inspector, but NOT entered in the diagnosis registry -- the registry is a
// live address book of meaningful surfaces, not a dumping ground for hundreds
// of rebuilt-on-reorder row internals (CANON: the unregistered_t primitive).
struct unregistered_t { explicit unregistered_t() = default; };
inline constexpr unregistered_t unregistered{};

template <class W>
class Named : public W {
public:
    template <class... Args>
    explicit Named(std::string_view name, Args&&... args)
        : W(std::forward<Args>(args)...) {
        this->set_name(std::string(name));
        registry::add(name, this);
        m_registered = true;
    }

    // Parallel ctor: name for the Inspector, skip the registry.
    template <class... Args>
    explicit Named(unregistered_t, std::string_view name, Args&&... args)
        : W(std::forward<Args>(args)...) {
        this->set_name(std::string(name));
    }

    ~Named() override {
        if (m_registered) registry::remove(this);
    }

private:
    bool m_registered = false;
};

}  // namespace acorn::widgets
