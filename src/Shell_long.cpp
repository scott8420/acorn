#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/eventcontrollerkey.h>

// Shell_long.cpp -- LONG READS (s020b). Preview and Export read every record
// they show, which on a big server table is seconds. They run here on a
// worker instead of the UI thread:
//
//   * the worker has the connection to itself: while it runs the window takes
//     no input -- no clicks reach the areas or the header, keys are swallowed,
//     the window's actions are off -- so nothing else can reach m_conn (a
//     connection is not shared between threads);
//   * nothing changes on screen for a quarter second (a quick read stays
//     quick-looking); after that the areas dim and a line says what it's
//     doing, how far it's got, and Stop (or Esc);
//   * the work returns what to do back on the UI thread; long_done runs it.
//
// The worker never touches GTK. Stop is a flag the work reads between its
// reads; a single slow query still finishes before it can stop.

namespace acorn {

void Shell::build_busy() {  // long: the line and Stop, over the window
    m_busy_bar.set_name("shell.busy");
    m_busy_bar.add_css_class("acorn-busy-bar");
    m_busy_bar.set_halign(Gtk::Align::CENTER);
    m_busy_bar.set_valign(Gtk::Align::END);
    m_busy_bar.set_margin_bottom(22);
    m_busy_says.set_name("shell.busy.says");
    m_busy_stop.set_name("shell.busy.stop");
    m_busy_stop.set_tooltip_text("Stop after the read that's running (Esc)");
    m_busy_stop.signal_clicked().connect([this] { stop_long(); });
    m_busy_bar.append(m_busy_spin);
    m_busy_bar.append(m_busy_says);
    m_busy_bar.append(m_busy_stop);
    m_busy_bar.set_visible(false);
    m_frame.set_child(m_body);
    m_frame.add_overlay(m_busy_bar);

    // Keys while busy: swallowed (a typed key must not reach a box whose
    // save would need the connection); Esc stops.
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint key, guint, Gdk::ModifierType) {
        if (!m_busy) return false;
        if (key == GDK_KEY_Escape && m_busy_stop.get_sensitive()) m_busy_stop.activate();
        return true;
    }, false);
    add_controller(keys);

    m_long_done.connect(sigc::mem_fun(*this, &Shell::long_done));
}

void Shell::stop_long() {  // long: Stop (the pill's, Esc, or a window of the work's own -- Import's)
    if (!m_long_ticket) return;
    m_long_ticket->stop = true;
    m_busy_stop.set_sensitive(false);
    m_busy_says.set_text("Stopping…");
}

void Shell::shut(bool on) {  // long: no input while the worker has the connection
    m_body.set_can_target(!on);
    if (auto* tb = get_titlebar()) tb->set_can_target(!on);
    if (on) {
        m_busy_actions.clear();
        for (const auto& name : list_actions())
            if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(lookup_action(name))) {
                m_busy_actions.emplace_back(a, a->get_enabled());
                a->set_enabled(false);
            }
        m_busy_stop.set_sensitive(true);
        m_busy_timer = Glib::signal_timeout().connect([this] {
            if (!m_busy) return false;
            if (!m_busy_bar.get_visible()) {   // a quarter second on: say so
                m_body.add_css_class("acorn-busy");
                m_busy_bar.set_visible(true);
                m_busy_spin.start();
            }
            if (m_busy_stop.get_sensitive()) {
                const auto n = m_long_ticket ? m_long_ticket->count.load() : 0;
                m_busy_says.set_text(n > 0 ? m_long_says + " -- " + std::to_string(n) + " so far" : m_long_says);
            }
            return true;
        }, 250);
    } else {
        m_busy_timer.disconnect();
        for (auto& [a, was] : m_busy_actions) a->set_enabled(was);
        m_busy_actions.clear();
        m_busy_spin.stop();
        m_busy_bar.set_visible(false);
        m_body.remove_css_class("acorn-busy");
    }
}

bool Shell::run_long(const std::string& says, LongWork work) {  // long: start the worker
    if (m_busy || m_long.joinable()) return false;
    m_busy = true;
    m_long_says = says;
    m_long_ticket = std::make_shared<LongTicket>();
    m_busy_says.set_text(says);
    shut(true);
    if (auto lg = log::get(log::Area::Shell)) lg->info("long: {}", says);
    m_long = std::thread([this, work = std::move(work), ticket = m_long_ticket]() mutable {
        std::function<void()> then;
        try {
            then = work(*ticket);
        } catch (const std::exception& e) {
            if (auto lg = log::get(log::Area::Shell)) lg->error("long: the work threw: {}", e.what());
        }
        {
            std::lock_guard lk(m_long_mx);
            m_long_then = std::move(then);
        }
        m_long_done.emit();
    });
    return true;
}

void Shell::long_done() {  // long: back on the UI thread
    if (m_long.joinable()) m_long.join();
    std::function<void()> then;
    {
        std::lock_guard lk(m_long_mx);
        then = std::move(m_long_then);
        m_long_then = nullptr;
    }
    const bool stopped = m_long_ticket && m_long_ticket->stop;
    m_busy = false;
    shut(false);
    m_long_ticket.reset();
    if (auto lg = log::get(log::Area::Shell)) lg->info("long: done{}", stopped ? " (stopped)" : "");
    sync_undo();   // s020c: a worker's journal changes weren't synced off the UI thread
    if (then) then();
    // Accounts asked for while this ran: read them now.
    if (std::exchange(m_accounts_again, false) && m_stack.get_visible_child_name() == "accounts") paint_accounts(true);
    // Preview asked for while this ran (a table picked in the same repaint):
    // paint it now, with what Browse shows now.
    if (std::exchange(m_preview_again, false) && m_browse_view == BrowseView::Preview) paint_preview();
}

}  // namespace acorn
