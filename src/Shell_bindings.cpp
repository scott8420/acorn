#include "Shell.hpp"
#include "Log.hpp"

#include <giomm/simpleactiongroup.h>
#include <glibmm/variant.h>

// Shell_bindings.cpp -- BINDINGS. What's wired to what: the win.* actions and
// the parameterised recents action group. No slot bodies here -- those are
// handlers; this file only wires.
//
// s004b: Cairn's demo actions are gone (pick-folder, the undo stone's verbs,
// win.undo / win.redo and the window's Ctrl+Z listener). The change journal
// (s005) brings undo back for real -- docs/undo.md keeps the window-key shape.

namespace acorn {

void Shell::bind_actions() {  // bindings: actions + recents group
    add_action("new-file",      sigc::mem_fun(*this, &Shell::on_new_file));
    add_action("new-address-book", sigc::mem_fun(*this, &Shell::on_new_address_book));   // s009b
    add_action("open-file",     sigc::mem_fun(*this, &Shell::on_open_file));
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
    add_action("about",         sigc::mem_fun(*this, &Shell::on_about));
    add_action("shortcuts",     sigc::mem_fun(*this, &Shell::on_shortcuts));
    add_action("quit",          [this]() { close(); });

    // Recents: a "recents" action group with PARAMETERISED verbs -- each menu
    // row / list row carries its connection's key as the target, so one action
    // serves every entry (the proven parameterised-action shape). clear is
    // held so rebuild_recents_menu can grey it when the list is empty.
    auto group = Gio::SimpleActionGroup::create();
    const auto keyed = [](auto fn) {
        return [fn](const Glib::VariantBase& param) {
            fn(std::string(
                Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param).get()));
        };
    };
    group->add_action_with_parameter("open", Glib::Variant<Glib::ustring>::variant_type(),
                                     keyed([this](const std::string& k) { on_recent_open(k); }));
    group->add_action_with_parameter("forget", Glib::Variant<Glib::ustring>::variant_type(),
                                     keyed([this](const std::string& k) { on_recent_forget(k); }));
    m_recents_clear = Gio::SimpleAction::create("clear");
    m_recents_clear->signal_activate().connect(
        [this](const Glib::VariantBase&) { on_recents_clear(); });
    group->add_action(m_recents_clear);
    insert_action_group("recents", group);

    // Now that the group exists, load the persisted list (drops vanished files).
    m_recents_file = connections_file();
    m_recents = core::load_connections(m_recents_file);
    if (auto lg = log::get(log::Area::Recents))
        lg->info("{} recent connection(s) from {}", m_recents.size(), m_recents_file);
}

}  // namespace acorn
