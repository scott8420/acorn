#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>

#include <filesystem>

// Shell_helpers.cpp -- HELPERS. Reusable orchestration called from zones,
// bindings, handlers and the Connect area: the recent-connections seam (path
// resolution, persist, and the two-surface repaint) and the keyring's key.

namespace acorn {

std::string Shell::connections_file() const {  // helper: XDG path (UI-side resolution)
    // The UI resolves the XDG dir (Glib here) and hands the core a plain path --
    // the core stays GTK-free (docs/two-layer-seam.md). Cairn's folder list
    // lived in recent.json; connections get their own file.
    return (std::filesystem::path(Glib::get_user_data_dir()) / "acorn" / "connections.json")
        .string();
}

void Shell::remember_connection(const core::SavedConnection& c) {  // helper: push + persist + repaint
    core::connections_add(m_recents, c);
    if (!core::save_connections(m_recents_file, m_recents))
        if (auto lg = log::get(log::Area::Recents))
            lg->warn("could not write {}", m_recents_file);
    rebuild_recents_menu();
    rebuild_recents_list();
}

std::string Shell::engine_name(const std::string& id) const {  // helper
    for (auto* e : m_engines)
        if (e->id() == id) return e->name();
    return id;   // a recent for an engine this machine no longer has
}

keyring::Account Shell::account_for(const core::SavedConnection& c) const {  // helper
    keyring::Account a;
    a.engine = c.engine;
    a.host = c.host;
    a.port = c.port;
    a.database = c.database;
    a.user = c.user;
    a.label = "Acorn — " + engine_name(c.engine) + ": " + c.label() +
              (c.user.empty() ? "" : " (" + c.user + ")");
    return a;
}

void Shell::rebuild_recents_menu() {  // helper: repaint the submenu
    if (!m_recents_menu) return;
    m_recents_menu->remove_all();
    for (const auto& c : m_recents) {
        auto item = Gio::MenuItem::create(engine_name(c.engine) + " — " + c.label(), "");
        item->set_action_and_target("recents.open", Glib::Variant<Glib::ustring>::create(c.key()));
        m_recents_menu->append_item(item);
    }
    auto clear_section = Gio::Menu::create();
    clear_section->append("Forget All Recent Connections", "recents.clear");
    m_recents_menu->append_section("", clear_section);
    if (m_recents_clear) m_recents_clear->set_enabled(!m_recents.empty());
}

void Shell::rebuild_recents_list() {  // helper: repaint the Connect page's list
    // Rows are in m_recents order; on_connect_page's row-activated handler
    // maps the row index back to the entry.
    while (auto* row = m_recent_list.get_row_at_index(0)) m_recent_list.remove(*row);

    if (m_recents.empty()) {
        auto* empty = Gtk::make_managed<Gtk::Label>("Connections you open appear here.");
        empty->add_css_class("acorn-status");
        empty->set_xalign(0.0f);
        empty->set_wrap(true);
        empty->set_margin(8);
        m_recent_list.append(*empty);
        if (auto* r = m_recent_list.get_row_at_index(0)) r->set_activatable(false);
        return;
    }
    for (const auto& c : m_recents) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        box->set_margin(6);
        auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
        text->set_hexpand(true);
        auto* name = Gtk::make_managed<Gtk::Label>(c.label());
        name->set_xalign(0.0f);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        auto* detail = Gtk::make_managed<Gtk::Label>(engine_name(c.engine) + " · " + c.detail());
        detail->set_xalign(0.0f);
        detail->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        detail->add_css_class("acorn-status");
        detail->add_css_class("acorn-small");
        text->append(*name);
        text->append(*detail);
        auto* forget = Gtk::make_managed<Gtk::Button>();
        forget->set_icon_name("window-close-symbolic");
        forget->set_has_frame(false);
        forget->set_valign(Gtk::Align::CENTER);
        forget->add_css_class("acorn-forget");
        forget->set_tooltip_text(c.is_file() ? "Forget this connection"
                                             : "Forget this connection and its stored password");
        forget->set_action_name("recents.forget");
        forget->set_action_target_value(Glib::Variant<Glib::ustring>::create(c.key()));
        box->append(*text);
        box->append(*forget);
        m_recent_list.append(*box);
    }
}

}  // namespace acorn
