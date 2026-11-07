#include "Shell.hpp"
#include "Log.hpp"

#include <filesystem>
#include <gtkmm/colordialog.h>
#include <glibmm/miscutils.h>

// Shell_ground.cpp -- the GROUND category (s005c). The skin's base colour:
// GNOME's accent presets or a custom colour, saved, applied live. The core
// (core/Ground) owns the colour recipe and the file; this file owns the
// radio action, the picker and the style provider.
//
// How it repaints: the skin (acorn.css, APPLICATION priority) names its
// colours with @define-color; a second provider ONE step higher redefines
// five of them. Reloading that provider repaints every window.acorn at once.

namespace acorn {

std::string Shell::ground_file() const {  // ground: ~/.config/acorn/ground.json
    return (std::filesystem::path(Glib::get_user_config_dir()) / "acorn" / "ground.json").string();
}

void Shell::bind_ground() {  // ground: win.ground (radio) + the saved choice + the provider
    m_ground = core::load_ground(ground_file());
    m_ground_css = Gtk::CssProvider::create();
    if (auto display = Gdk::Display::get_default())
        Gtk::StyleContext::add_provider_for_display(display, m_ground_css,
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    m_ground_action = add_action_radio_string(
        "ground", [this](const Glib::ustring& id) { on_ground(id); }, m_ground.id);
    apply_ground();
}

void Shell::apply_ground() {  // ground: the choice -> the style provider
    const auto css = core::ground_css(core::derive(m_ground.base()));
    m_ground_css->load_from_data(css);
    if (m_ground_action) m_ground_action->change_state(Glib::ustring(m_ground.id));
    if (auto lg = log::get(log::Area::Shell))
        lg->info("ground: {} ({})", m_ground.id, core::to_hex(m_ground.base()));
}

void Shell::on_ground(const std::string& id) {  // ground: a preset, or "custom" -> the picker
    if (id != "custom") {
        if (!core::ground_preset(id)) return;
        m_ground.id = id;
        apply_ground();
        if (!core::save_ground(ground_file(), m_ground))
            if (auto lg = log::get(log::Area::Shell)) lg->warn("ground: could not save {}", ground_file());
        return;
    }
    // Custom: the picker starts on the colour showing now. The radio moves
    // only when a colour is chosen -- cancelling leaves everything as it was.
    auto dialog = Gtk::ColorDialog::create();
    dialog->set_title("Base Color");
    dialog->set_with_alpha(false);
    const auto b = m_ground.base();
    Gdk::RGBA start;
    start.set_rgba(b.r / 255.0, b.g / 255.0, b.b / 255.0, 1.0);
    if (m_ground_action) m_ground_action->change_state(Glib::ustring(m_ground.id));
    dialog->choose_rgba(*this, start, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& res) {
        Gdk::RGBA c;
        try {
            c = dialog->choose_rgba_finish(res);
        } catch (const Glib::Error&) {
            apply_ground();   // cancelled: the radio back where it was
            return;
        }
        const auto ch = [](double v) { return std::clamp(static_cast<int>(v * 255 + 0.5), 0, 255); };
        m_ground.id = "custom";
        m_ground.custom = {ch(c.get_red()), ch(c.get_green()), ch(c.get_blue())};
        apply_ground();
        if (!core::save_ground(ground_file(), m_ground))
            if (auto lg = log::get(log::Area::Shell)) lg->warn("ground: could not save {}", ground_file());
    });
}

}  // namespace acorn
