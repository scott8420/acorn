#include "Shell.hpp"
#include "Log.hpp"
#include "db/Script.hpp"

#include <gtkmm/alertdialog.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/textview.h>
#include <glibmm/main.h>

// Shell_scripts.cpp -- SCRIPTS (s017a). A script lives in the solution
// (core::Script), under the table it runs on: a named list of steps.
//   * the Tables area grows a Scripts section: each script with what it does,
//     a Run button, the pencil and the trash; New Script... / the pencil open
//     an editor in place -- a name, the steps (each a kind and what it needs,
//     moved up / down, removed), a live line that says what's wrong in words;
//     Add / Save is one step (verbs::save_script); the trash is one step.
//   * Browse's bar has a Scripts menu: the shown table's scripts; a pick runs
//     it there.
//   * a run (db::ScriptRun) walks the steps on Browse's own state -- layout,
//     found set, sort, picked record -- and every write is in ONE journal batch:
//     the whole run is one step on Ctrl+Z ("Script “tidy”"). Delete Record
//     asks first; Show Message waits for OK.

namespace acorn {

namespace {

std::string curly(const std::string& s) { return "“" + s + "”"; }

Gtk::Label* cell(const std::string& text, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    if (css) l->add_css_class(css);
    return l;
}

// A drop-down over `names`, `pick` selected (appended, marked, when it's not
// one of them -- a field or layout that went away stays visible).
Gtk::DropDown* names_dd(std::vector<std::string> names, const std::string& pick, const std::string& missing_word) {
    std::vector<Glib::ustring> shown(names.begin(), names.end());
    guint sel = 0;
    bool found = false;
    for (guint i = 0; i < names.size(); ++i) if (names[i] == pick) { sel = i; found = true; }
    if (!found && !pick.empty()) {
        shown.push_back(pick + " (" + missing_word + ")");
        sel = static_cast<guint>(shown.size() - 1);
    }
    auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(shown));
    dd->set_selected(sel);
    return dd;
}

}  // namespace

void Shell::script_section(const db::Table& t) {  // scripts: the Scripts section (list or editor)
    if (m_sc_table != t.name) {   // another table: the editor closes
        m_sc_table = t.name;
        m_sc_editing.clear();
        m_sc_new = false;
    }
    if (!m_sc_editing.empty() && !m_solution.script(t.name, m_sc_editing)) m_sc_editing.clear();   // undone away
    const bool editing = m_sc_new || !m_sc_editing.empty();
    const bool can = m_journal != nullptr;
    const auto& scripts = m_solution.scripts_of(t.name);
    const std::string table = t.name;

    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_name("tables.scripts");
    Gtk::Widget* focus = nullptr;

    if (!scripts.empty()) {
        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(14);
        grid->set_row_spacing(5);
        int r = 0;
        for (int c = 0; const char* h : {"Script", "Steps"}) grid->attach(*cell(h, "acorn-th"), c++, r);
        ++r;
        for (const auto& [name, s] : scripts) {
            if (name == m_sc_editing) continue;   // its editor is below
            grid->attach(*cell(name, "acorn-calc-name"), 0, r);
            auto says = cell(core::script_says(s));
            says->set_ellipsize(Pango::EllipsizeMode::END);
            says->set_max_width_chars(48);
            says->set_hexpand(true);
            std::string tip;
            for (std::size_t i = 0; i < s.steps.size(); ++i)
                tip += (i ? "\n" : "") + std::to_string(i + 1) + ". " + core::step_says(s.steps[i]);
            says->set_tooltip_text(tip);
            grid->attach(*says, 1, r);
            auto run = Gtk::make_managed<Gtk::Button>();
            run->set_icon_name("media-playback-start-symbolic");
            run->add_css_class("flat");
            run->set_name("tables.script.run." + name);
            run->set_tooltip_text("Run " + curly(name) + " in Browse -- Ctrl+Z undoes what it wrote, in one step");
            run->set_sensitive(can && !editing && !m_script_run);
            run->signal_clicked().connect([this, table, name] { run_script(table, name); });
            grid->attach(*run, 2, r);
            auto edit = Gtk::make_managed<Gtk::Button>();
            edit->set_icon_name("document-edit-symbolic");
            edit->add_css_class("flat");
            edit->set_name("tables.script.edit." + name);
            edit->set_tooltip_text("Change " + curly(name));
            edit->set_sensitive(can && !editing);
            edit->signal_clicked().connect([this, name, s, table] {
                m_sc_editing = name;
                m_sc_new = false;
                m_sc_typed = s;
                if (const db::Table* tt = m_schema.table(table)) script_section(*tt);
            });
            grid->attach(*edit, 3, r);
            auto del = Gtk::make_managed<Gtk::Button>();
            del->set_icon_name("user-trash-symbolic");
            del->add_css_class("flat");
            del->set_name("tables.script.delete." + name);
            del->set_tooltip_text("Delete " + curly(name) + " -- Ctrl+Z brings it back");
            del->set_sensitive(can && !editing);
            del->signal_clicked().connect([this, table, name] { drop_script(table, name); });
            grid->attach(*del, 4, r);
            ++r;
        }
        if (r > 1) body->append(*grid);
    }

    if (!editing) {
        if (scripts.empty()) {
            auto l = cell("None yet. A script is a list of steps Acorn does for you in Browse -- go to a layout, find, "
                          "sort, make or change records, preview, print, show a message. Run it from here or Browse's "
                          "Scripts menu; whatever it writes is one step on Ctrl+Z.", "acorn-status");
            l->set_wrap(true);
            l->set_max_width_chars(70);
            body->append(*l);
        }
        auto add = Gtk::make_managed<Gtk::Button>("New Script…");
        add->set_name("tables.script.new");
        add->set_halign(Gtk::Align::START);
        add->set_sensitive(can);
        add->set_tooltip_text(can ? "A named list of steps" : "Open a database first");
        add->signal_clicked().connect([this, table] {
            m_sc_new = true;
            m_sc_editing.clear();
            m_sc_typed = core::Script{table, "", {core::ScriptStep{core::StepKind::ShowAll}}};
            if (const db::Table* tt = m_schema.table(table)) script_section(*tt);
        });
        body->append(*add);
    } else {
        const std::string was = m_sc_editing;
        std::vector<std::string> fields, layouts;
        for (const auto& c : t.columns) fields.push_back(c.name);
        if (auto ls = m_solution.layouts.find(table); ls != m_solution.layouts.end())
            for (const auto& [n, l] : ls->second) layouts.push_back(n);
        std::vector<std::string> taken;
        for (const auto& [n, x] : scripts) if (n != (was.empty() ? m_sc_typed.name : was)) taken.push_back(n);

        auto ed = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        ed->add_css_class("acorn-calc-editor");
        auto name_e = Gtk::make_managed<Gtk::Entry>();
        name_e->set_name("tables.script.name");
        name_e->set_placeholder_text("name");
        name_e->set_width_chars(24);
        name_e->set_halign(Gtk::Align::START);
        name_e->set_text(m_sc_typed.name);
        ed->append(*name_e);

        auto live = cell("", "acorn-status");
        live->set_name("tables.script.live");
        live->set_wrap(true);
        live->set_max_width_chars(80);
        auto save = Gtk::make_managed<Gtk::Button>(m_sc_new ? "Add" : "Save");
        save->set_name("tables.script.save");
        save->add_css_class("acorn-primary");
        // The live line: what's wrong, in words -- or what it will do.
        auto check = [this, live, save, taken, fields, layouts] {
            const auto why = core::check_script(m_sc_typed, taken, fields, layouts);
            live->remove_css_class("acorn-calc-error");
            live->remove_css_class("acorn-status");
            live->add_css_class(why.empty() ? "acorn-status" : "acorn-calc-error");
            live->set_text(why.empty() ? "Ready -- " + core::script_says(m_sc_typed) : why);
            save->set_sensitive(why.empty());
        };
        name_e->signal_changed().connect([this, name_e, check] { m_sc_typed.name = name_e->get_text(); check(); });

        auto repaint = [this, table] {
            if (const db::Table* tt = m_schema.table(table)) script_section(*tt);
        };
        // The kinds' labels, in the editor's order.
        std::vector<Glib::ustring> kinds;
        for (auto k : core::step_kinds()) kinds.push_back(core::step_label(k));

        auto steps = Gtk::make_managed<Gtk::Grid>();
        steps->set_column_spacing(6);
        steps->set_row_spacing(4);
        const int n = static_cast<int>(m_sc_typed.steps.size());
        for (int i = 0; i < n; ++i) {
            const auto& st = m_sc_typed.steps[static_cast<std::size_t>(i)];
            const std::string id = "tables.script.step" + std::to_string(i + 1);
            steps->attach(*cell(std::to_string(i + 1) + ".", "acorn-status"), 0, i);
            auto kind_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(kinds));
            kind_dd->set_name(id + ".kind");
            const auto& all = core::step_kinds();
            kind_dd->set_selected(static_cast<guint>(std::find(all.begin(), all.end(), st.kind) - all.begin()));
            if (m_sc_focus == id + ".kind") focus = kind_dd;
            kind_dd->property_selected().signal_changed().connect([this, kind_dd, i, repaint, id] {
                const auto& ks = core::step_kinds();
                const auto sel = kind_dd->get_selected();
                if (sel >= ks.size()) return;
                auto& s = m_sc_typed.steps[static_cast<std::size_t>(i)];
                if (s.kind == ks[sel]) return;
                s = core::ScriptStep{ks[sel], s.field, {}, false};   // the field may carry over (Find -> Sort)
                m_sc_focus = id + ".kind";   // the repaint gives the focus back here, not to the name
                Glib::signal_idle().connect_once(repaint);
            });
            kind_dd->set_margin_start(18 * core::step_depth(m_sc_typed.steps, static_cast<std::size_t>(i)));   // s023a: nested Ifs indent
            steps->attach(*kind_dd, 1, i);

            // What the step needs.
            auto what = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            what->set_hexpand(true);
            auto field_pick = [&, i](const std::vector<std::string>& from, const char* missing) {
                auto dd = names_dd(from, st.field, missing);
                dd->set_name(id + ".field");
                // A step with no field yet takes the first.
                if (st.field.empty() && !from.empty()) m_sc_typed.steps[static_cast<std::size_t>(i)].field = from[0];
                auto names = std::make_shared<std::vector<std::string>>(from);
                dd->property_selected().signal_changed().connect([this, dd, i, names, check] {
                    const auto sel = dd->get_selected();
                    if (sel < names->size()) m_sc_typed.steps[static_cast<std::size_t>(i)].field = (*names)[sel];
                    check();
                });
                what->append(*dd);
            };
            auto text_box = [&, i](const char* hint, int chars) {
                auto e = Gtk::make_managed<Gtk::Entry>();
                e->set_name(id + ".text");
                e->set_placeholder_text(hint);
                e->set_width_chars(chars);
                e->set_hexpand(true);
                e->set_text(st.text);
                e->signal_changed().connect([this, e, i, check] {
                    m_sc_typed.steps[static_cast<std::size_t>(i)].text = e->get_text();
                    check();
                });
                what->append(*e);
            };
            using K = core::StepKind;
            switch (st.kind) {
            case K::GoToLayout:
                if (layouts.empty()) what->append(*cell("(this table has no layouts kept yet -- make one in Layouts)", "acorn-calc-error"));
                else field_pick(layouts, "missing");
                break;
            case K::Find:
                field_pick(fields, "missing");
                text_box("what to find: a, =, =Thorne, <1970-01-01", 22);
                break;
            case K::Sort: {
                field_pick(fields, "missing");
                auto order = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"ascending", "descending"}));
                order->set_name(id + ".order");
                order->set_selected(st.descending ? 1 : 0);
                order->property_selected().signal_changed().connect([this, order, i, check] {
                    m_sc_typed.steps[static_cast<std::size_t>(i)].descending = order->get_selected() == 1;
                    check();
                });
                what->append(*order);
                break;
            }
            case K::SetField:
                field_pick(fields, "missing");
                what->append(*cell("to", "acorn-status"));
                text_box("a calculation: \"Ada\", Upper(last_name), Today", 26);
                break;
            case K::NewRecord: {
                auto tv = Gtk::make_managed<Gtk::TextView>();
                tv->set_name(id + ".text");
                tv->get_buffer()->set_text(st.text);
                tv->set_hexpand(true);
                tv->set_size_request(-1, 44);
                tv->set_top_margin(3);
                tv->set_left_margin(4);
                tv->add_css_class("acorn-list-values");
                tv->set_tooltip_text("One field a line: first_name = \"Ada\" -- or nothing, for a blank record");
                tv->get_buffer()->signal_changed().connect([this, tv, i, check] {
                    m_sc_typed.steps[static_cast<std::size_t>(i)].text = tv->get_buffer()->get_text();
                    check();
                });
                what->append(*tv);
                break;
            }
            case K::Message:
                text_box("the words to show", 30);
                break;
            case K::ShowAll:      what->append(*cell("every record again", "acorn-status")); break;
            case K::DeleteRecord: what->append(*cell("the picked record -- asks first; No stops the script", "acorn-status")); break;
            case K::Preview:      what->append(*cell("Browse -> Preview of the picked layout", "acorn-status")); break;
            case K::Print:        what->append(*cell("the print dialog over the preview's pages", "acorn-status")); break;
            // s023a -- steps that decide.
            case K::If:
            case K::ElseIf:
                text_box("a condition: last_name = \"Thorne\", $n > 3, Length(email) = 0", 28);
                break;
            case K::Else:  what->append(*cell("when nothing above it was true", "acorn-status")); break;
            case K::EndIf: what->append(*cell("the end of the If", "acorn-status")); break;
            case K::SetVariable: {
                if (st.field.empty() || st.field[0] != '$') m_sc_typed.steps[static_cast<std::size_t>(i)].field = "$n";
                auto nm = Gtk::make_managed<Gtk::Entry>();
                nm->set_name(id + ".field");
                nm->set_width_chars(10);
                nm->set_text(m_sc_typed.steps[static_cast<std::size_t>(i)].field);
                nm->set_tooltip_text("$ then a name -- kept for this run; later steps read it as $name");
                nm->signal_changed().connect([this, nm, i, check] {
                    m_sc_typed.steps[static_cast<std::size_t>(i)].field = nm->get_text();
                    check();
                });
                what->append(*nm);
                what->append(*cell("to", "acorn-status"));
                text_box("a calculation: 0, $n + 1, Upper(last_name)", 24);
                break;
            }
            // s023b
            case K::Loop:    what->append(*cell("the steps down to End Loop, again and again", "acorn-status")); break;
            case K::EndLoop: what->append(*cell("back to the Loop", "acorn-status")); break;
            case K::ExitLoopIf:
                text_box("a condition: $n > 10, Length(notes) > 0", 26);
                break;
            case K::GoToRecord: {
                const auto& whats = core::go_to_record_whats();
                if (std::find(whats.begin(), whats.end(), st.field) == whats.end())
                    m_sc_typed.steps[static_cast<std::size_t>(i)].field = "next";
                std::vector<Glib::ustring> ws(whats.begin(), whats.end());
                auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(ws));
                dd->set_name(id + ".field");
                const auto& cur = m_sc_typed.steps[static_cast<std::size_t>(i)].field;
                dd->set_selected(static_cast<guint>(std::find(whats.begin(), whats.end(), cur) - whats.begin()));
                dd->set_tooltip_text("In the found set, in its sort -- next / previous off the end leaves the Loop it's in");
                dd->property_selected().signal_changed().connect([this, dd, i, repaint, id] {
                    const auto& w = core::go_to_record_whats();
                    if (dd->get_selected() >= w.size()) return;
                    m_sc_typed.steps[static_cast<std::size_t>(i)].field = w[dd->get_selected()];
                    m_sc_focus = id + ".field";
                    Glib::signal_idle().connect_once(repaint);   // number shows its box
                });
                what->append(*dd);
                if (cur == "number") text_box("which: 1, $n", 10);
                break;
            }
            case K::PerformScript: {
                std::vector<std::string> names = taken;
                if (!m_sc_typed.name.empty() && std::find(names.begin(), names.end(), m_sc_typed.name) == names.end())
                    names.push_back(m_sc_typed.name);
                if (names.empty()) what->append(*cell("(this table has no other scripts yet)", "acorn-calc-error"));
                else field_pick(names, "missing");
                break;
            }
            }
            steps->attach(*what, 2, i);

            auto mover = [&, i](const char* icon, const char* tip, const char* suffix, int delta, bool on) {
                auto b = Gtk::make_managed<Gtk::Button>();
                b->set_icon_name(icon);
                b->add_css_class("flat");
                b->set_name(id + "." + suffix);
                b->set_tooltip_text(tip);
                b->set_sensitive(on);
                if (m_sc_focus == id + "." + suffix) focus = b;
                b->signal_clicked().connect([this, i, delta, repaint, suffix] {
                    auto& v = m_sc_typed.steps;
                    if (delta == 0) v.erase(v.begin() + i);
                    else std::swap(v[static_cast<std::size_t>(i)], v[static_cast<std::size_t>(i + delta)]);
                    // The focus follows the step it moved (or to Add Step when it went).
                    const int to = i + delta;
                    const bool edge = delta != 0 && (to == 0 || to + 1 == static_cast<int>(v.size()));
                    m_sc_focus = delta == 0 ? std::string("tables.script.add_step")
                               : "tables.script.step" + std::to_string(to + 1) + (edge ? ".kind" : std::string(".") + suffix);
                    Glib::signal_idle().connect_once(repaint);
                });
                return b;
            };
            steps->attach(*mover("go-up-symbolic", "Earlier", "up", -1, i > 0), 3, i);
            steps->attach(*mover("go-down-symbolic", "Later", "down", 1, i + 1 < n), 4, i);
            steps->attach(*mover("list-remove-symbolic", "Take this step out", "remove", 0, true), 5, i);
        }
        ed->append(*steps);

        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto add_step = Gtk::make_managed<Gtk::Button>("Add Step");
        add_step->set_name("tables.script.add_step");
        add_step->set_sensitive(n < core::kScriptStepsMax);
        if (m_sc_focus == "tables.script.add_step") focus = add_step;
        add_step->signal_clicked().connect([this, repaint] {
            m_sc_typed.steps.push_back(core::ScriptStep{core::StepKind::ShowAll});
            m_sc_focus = "tables.script.step" + std::to_string(m_sc_typed.steps.size()) + ".kind";
            Glib::signal_idle().connect_once(repaint);
        });
        row->append(*add_step);
        auto spacer = Gtk::make_managed<Gtk::Box>();
        spacer->set_hexpand(true);
        row->append(*spacer);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->set_name("tables.script.cancel");
        row->append(*cancel);
        row->append(*save);
        ed->append(*live);
        ed->append(*row);
        body->append(*ed);
        check();

        auto close = [this, table] {
            m_sc_new = false;
            m_sc_editing.clear();
            if (const db::Table* tt = m_schema.table(table)) script_section(*tt);
        };
        cancel->signal_clicked().connect(close);
        auto commit = [this, save, was] {
            if (!save->get_sensitive()) return;
            keep_script(m_sc_typed, was);
        };
        save->signal_clicked().connect(commit);
        name_e->signal_activate().connect(commit);
        auto keys = Gtk::EventControllerKey::create();   // Esc: put it away; Ctrl+Enter: keep it
        keys->signal_key_pressed().connect([close, commit](guint key, guint, Gdk::ModifierType mods) {
            if (key == GDK_KEY_Escape) { close(); return true; }
            if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) &&
                (mods & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK) {
                commit();
                return true;
            }
            return false;
        }, false);
        keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        ed->add_controller(keys);
        if (!focus && m_sc_focus.empty() && m_sc_new && m_sc_typed.name.empty()) focus = name_e;
        m_sc_focus.clear();
    }

    if (m_sc_box && m_sc_box->get_parent()) {   // repaint in place: keep the heading
        while (auto* c = m_sc_box->get_last_child()) {
            if (c == m_sc_box->get_first_child()) break;
            m_sc_box->remove(*c);
        }
        m_sc_box->append(*body);
    }
    if (focus) focus->grab_focus();
}

void Shell::keep_script(const core::Script& s, const std::string& was) {  // scripts: verbs::save_script, the note, a repaint
    if (!m_journal) return;
    const auto before = m_journal->size();
    const bool could_redo = m_journal->can_redo();
    auto r = db::verbs::save_script(*m_journal, s, was);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: script '{}' of '{}' = {}: {}", s.name, s.table, core::script_says(s),
                 r ? m_journal->undo_label() : r.error().message);
    if (!r) { journal_note("Not kept: " + r.error().message + ".", "error"); return; }
    m_sc_new = false;
    m_sc_editing.clear();
    if (m_table_sel == s.table) show_table(s.table);
    sync_undo();
    if (m_journal->size() != before || could_redo != m_journal->can_redo())
        journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
}

void Shell::drop_script(const std::string& table, const std::string& name) {  // scripts: verbs::delete_script
    if (!m_journal) return;
    auto r = db::verbs::delete_script(*m_journal, table, name);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: delete script '{}' of '{}': {}", name, table, r ? "done" : r.error().message);
    if (!r) { journal_note("Not deleted: " + r.error().message + ".", "error"); return; }
    if (m_table_sel == table) show_table(table);
    sync_undo();
    journal_note("Deleted the script " + curly(name) + ". Ctrl+Z brings it back.", "ok");
}

// ── Browse's Scripts menu ───────────────────────────────────────────────────
void Shell::fill_scripts_menu() {  // scripts: the shown table's scripts, each a button that runs it
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    box->set_name("shell.browse.scripts.list");
    const auto& scripts = m_solution.scripts_of(m_browse_table);
    if (scripts.empty()) {
        auto l = cell("No scripts for “" + m_browse_table + "” yet --\nTables -> " + m_browse_table + " -> Scripts -> New Script…",
                      "acorn-status");
        l->set_margin(6);
        box->append(*l);
    }
    const std::string table = m_browse_table;
    for (const auto& [name, s] : scripts) {
        auto b = Gtk::make_managed<Gtk::Button>(name);
        b->set_name("shell.browse.scripts." + name);
        b->add_css_class("flat");
        b->set_tooltip_text(core::script_says(s));
        if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) l->set_xalign(0.0f);
        b->signal_clicked().connect([this, table, name] {
            m_browse_scripts_pop.popdown();
            Glib::signal_idle().connect_once([this, table, name] { run_script(table, name); });
        });
        box->append(*b);
    }
    m_browse_scripts_pop.set_child(*box);
}

// ── running ─────────────────────────────────────────────────────────────────
void Shell::run_script(const std::string& table, const std::string& name) {  // scripts: Browse, that table, the steps in order
    if (!m_journal || m_script_run) return;
    const core::Script* s = m_solution.script(table, name);
    if (!s) return;
    if (m_journal->in_batch()) return;
    open_browse(table);
    db::ScriptState st;
    st.table = table;
    if (auto it = m_layout_pick.find(table); it != m_layout_pick.end()) st.layout = it->second;
    if (found_here()) st.found = m_found;
    st.sort = m_browse_req.sort;
    st.descending = m_browse_req.descending;
    st.record = m_browse_sel;
    m_finding = false;
    if (auto lg = log::get(log::Area::App)) lg->info("script: run '{}' of '{}' ({} steps)", name, table, s->steps.size());
    m_script_run = std::make_unique<db::ScriptRun>(*m_journal, m_solution, *s, st);
    browse_note("Running " + curly(name) + "…", "");
    script_continue();
}

void Shell::script_apply() {  // scripts: the run's state -> Browse
    if (!m_script_run) return;
    const auto& st = m_script_run->state();
    bool layout_changed = false;
    if (!st.layout.empty() && m_layout_pick[st.table] != st.layout) {
        m_layout_pick[st.table] = st.layout;
        layout_changed = true;
    }
    if (st.found.empty()) {
        m_found.clear();
        m_found_reqs.clear();
        m_found_table.clear();
    } else {
        m_found = st.found;
        m_found_table = st.table;
        m_found_reqs.clear();   // s022: back into requests, as typed
        for (const auto& c : st.found) {
            const auto r = static_cast<std::size_t>(std::max(c.request, 0));
            if (m_found_reqs.size() <= r) m_found_reqs.resize(r + 1);
            m_found_reqs[r].omit = c.omit;
            if (c.table.empty()) m_found_reqs[r].typed["field|" + c.field] = c.typed;
        }
    }
    m_browse_req.sort = st.sort;
    m_browse_req.descending = st.descending;
    if (st.record != m_browse_sel) m_browse_req.offset = 0;
    m_browse_sel = st.record;
    m_record_new = false;
    // Go to Layout shows it: Cards / Grid become the Form.
    if (layout_changed && (m_browse_view == BrowseView::Cards || m_browse_view == BrowseView::Grid))
        m_browse_form_btn.set_active(true);
    load_browse_page();
    // The picked record may be on a later page: walk to it (a few pages at most).
    for (int guard = 0; guard < 40 && !m_browse_sel.empty() && m_browse_sel_index < 0 &&
                        m_browse_req.offset + m_browse_req.limit < m_browse_data.total; ++guard) {
        m_browse_req.offset += m_browse_req.limit;
        load_browse_page();
    }
    if (m_browse_sel_index < 0 && !m_browse_sel.empty() && m_browse_req.offset > 0) {
        m_browse_req.offset = 0;
        load_browse_page();
    }
}

void Shell::script_continue() {  // scripts: on to the next pause
    if (!m_script_run) return;
    auto& run = *m_script_run;
    const auto p = run.go();
    script_apply();
    const std::string name = run.script().name;
    auto finish = [this, name](const std::string& words, const char* kind) {
        if (auto lg = log::get(log::Area::App)) lg->info("script: '{}' -- {}", name, words);
        m_script_run.reset();
        sync_undo();
        m_browse_scripts.set_sensitive(browse_info() != nullptr && m_journal != nullptr);
        browse_note(words, kind);
        if (!m_table_sel.empty()) show_table(m_table_sel);   // the Run buttons wake up
    };
    switch (p) {
    case db::ScriptPause::Done: {
        const int w = run.writes();
        finish("Ran " + curly(name) + (w == 0 ? " -- it wrote nothing." :
               " -- " + std::to_string(w) + (w == 1 ? " change" : " changes") + "; Ctrl+Z undoes " +
               (w == 1 ? "it." : "them all, in one step.")), "ok");
        return;
    }
    case db::ScriptPause::Stopped:
        finish(curly(name) + " stopped at " + run.words() + ".", "error");
        return;
    case db::ScriptPause::Preview:
        m_browse_preview_btn.set_active(true);
        Glib::signal_idle().connect_once([this] { script_continue(); });
        return;
    case db::ScriptPause::Print:
        m_browse_preview_btn.set_active(true);
        print_preview();
        Glib::signal_idle().connect_once([this] { script_continue(); });
        return;
    case db::ScriptPause::Message:
    case db::ScriptPause::Ask: {
        const bool ask = p == db::ScriptPause::Ask;
        auto dlg = Gtk::AlertDialog::create(ask ? run.words() : run.words());
        dlg->set_detail(ask ? "The script " + curly(name) + " deletes it next. Ctrl+Z puts back the whole run."
                            : "From the script " + curly(name) + ".");
        if (ask) {
            dlg->set_buttons({"Stop the Script", "Delete"});
            dlg->set_cancel_button(0);
            dlg->set_default_button(1);
        } else {
            dlg->set_buttons({"OK"});
            dlg->set_cancel_button(0);
            dlg->set_default_button(0);
        }
        dlg->set_modal(true);
        dlg->choose(*this, [this, dlg, ask](Glib::RefPtr<Gio::AsyncResult>& res) {
            int pick = 0;
            try { pick = dlg->choose_finish(res); } catch (const Glib::Error&) { pick = 0; }
            if (!m_script_run) return;
            if (ask) m_script_run->answer(pick == 1);
            script_continue();
        });
        return;
    }
    }
}

}  // namespace acorn
