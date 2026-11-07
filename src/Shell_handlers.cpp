#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"


#include <filesystem>

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action actually does.
// Each is small and single-concern; orchestration it shares lives in helpers.

namespace acorn {

void Shell::on_dump_registry() {  // handler: print the live widget tree
    // The debuggability payoff, on demand. Same call works from a gdb prompt.
    registry::dump();
}

void Shell::on_shortcuts() {  // handler: open the keyboard reference
    // Same lifetime stone as About: build once, re-present. The window's CONTENT
    // is not written here -- it renders by walking core::shortcut_registry(),
    // the same list bind_accelerators() wires from, so the two can't drift.
    if (!m_shortcuts) m_shortcuts = std::make_unique<ShortcutsDialog>();
    m_shortcuts->show(*this);
}

void Shell::on_about() {  // handler: open the About window (dialog-lifetime exemplar)
    // Lazily build once, then re-present. Hide-on-close singleton (CANON:
    // "Lifetime shape is design") -- the X hides it, so one instance lives for
    // the session and its named children never re-register.
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

// ─────────────────────────────────────────────────────────────────────────────
// s004b -- recent connections. Opening one fills the form and connects (the
// password, if any, comes from the keyring on the way). Forgetting one drops
// it from the list AND clears its keyring entry, so nothing is left behind.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::on_recent_open(const std::string& key) {  // handler: connect to a recent
    const auto* c = core::connections_find(m_recents, key);
    if (!c) return;
    if (auto lg = log::get(log::Area::Recents)) lg->info("open recent '{}'", key);
    if (c->is_file() && !std::filesystem::exists(c->path)) {
        set_status("That file is gone: " + c->path + ". Removed from the recent list.", "error");
        on_recent_forget(key);
        return;
    }
    m_stack.set_visible_child("connect");
    fill_form(*c);
    on_connect();
}

void Shell::on_recent_forget(const std::string& key) {  // handler: drop one + its password
    const auto* c = core::connections_find(m_recents, key);
    if (!c) return;
    if (!c->is_file()) {
        keyring::clear(account_for(*c), [key](bool removed, std::string err) {
            if (auto lg = log::get(log::Area::Recents)) {
                if (!err.empty()) lg->warn("keyring clear '{}': {}", key, err);
                else lg->info("keyring clear '{}': {}", key, removed ? "removed" : "none stored");
            }
        });
    }
    core::connections_remove(m_recents, key);
    core::save_connections(m_recents_file, m_recents);
    rebuild_recents_menu();
    rebuild_recents_list();
}

void Shell::on_recents_clear() {  // handler: drop them all + their passwords
    std::vector<std::string> keys;
    for (auto& c : m_recents) keys.push_back(c.key());
    for (auto& k : keys) on_recent_forget(k);
}

}  // namespace acorn
