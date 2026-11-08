#include "Shell.hpp"
#include "Log.hpp"
#include "db/Browse.hpp"

#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <gtkmm/expression.h>
#include <gtkmm/textview.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <glibmm/main.h>

// Shell_lists.cpp -- VALUE LISTS (s014c). A value list lives in the solution
// (core::ValueList), under the table whose layouts offer it: typed values, or
// every value of a field (optionally showing another field of that table).
//   * the Tables area grows a Value lists section: each list with what it
//     offers and how many; New / the pencil open an editor in place (a name,
//     typed or from a field, a live line that says what's wrong in words or
//     what it offers); Add / Save is one step (verbs::save_value_list) on the
//     same Ctrl+Z list; the trash is one step.
//   * a layout's field box offers one as a drop-down (s014d).

namespace acorn {

namespace {

std::string curly(const std::string& s) { return "“" + s + "”"; }

Gtk::Label* cell(const std::string& text, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    if (css) l->add_css_class(css);
    return l;
}

// "3 values: home · work · mobile" -- what a list offers, read now.
std::string offers_text(const db::ListItems& li) {
    const auto n = li.items.size();
    if (n == 0) return "Nothing to offer yet -- the field is empty in every record.";
    std::string s = std::to_string(n) + (li.more ? "+" : "") + (n == 1 ? " value: " : " values: ");
    for (std::size_t i = 0; i < n && i < 6; ++i) s += (i ? " · " : "") + li.items[i].shown;
    if (n > 6) s += " …";
    if (li.more) s += "  (the first " + std::to_string(core::kValueListMax) + ")";
    return s;
}

}  // namespace

void Shell::list_section(const db::Table& t) {  // lists: the Value lists section (list or editor)
    if (m_vl_table != t.name) {   // another table: the editor closes
        m_vl_table = t.name;
        m_vl_editing.clear();
        m_vl_new = false;
    }
    // An undo took away the list being changed: the editor closes.
    if (!m_vl_editing.empty() && !m_solution.list(t.name, m_vl_editing)) m_vl_editing.clear();
    const bool editing = m_vl_new || !m_vl_editing.empty();
    const bool can = m_journal != nullptr;
    const auto& lists = m_solution.lists_of(t.name);
    const std::string table = t.name;

    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_name("tables.lists");
    Gtk::Widget* focus = nullptr;

    if (!lists.empty()) {
        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(14);
        grid->set_row_spacing(5);
        int r = 0;
        for (int c = 0; const char* h : {"Value list", "Offers", "Now"}) grid->attach(*cell(h, "acorn-th"), c++, r);
        ++r;
        for (const auto& [name, v] : lists) {
            if (name == m_vl_editing) continue;   // its editor is below
            grid->attach(*cell(name, "acorn-calc-name"), 0, r);
            auto says = cell(core::value_list_says(v));
            says->set_ellipsize(Pango::EllipsizeMode::END);
            says->set_max_width_chars(48);
            says->set_hexpand(true);
            says->set_tooltip_text(v.typed() ? core::list_values_text(v.values) : core::value_list_says(v));
            grid->attach(*says, 1, r);
            std::string now;
            const char* css = "acorn-status";
            if (v.typed()) now = std::to_string(v.values.size()) + (v.values.size() == 1 ? " value" : " values");
            else if (m_conn) {
                auto li = db::read_value_list(*m_conn, m_schema, v);
                if (li) now = std::to_string(li->items.size()) + (li->more ? "+" : "") + (li->items.size() == 1 ? " value" : " values");
                else { now = "?"; css = "acorn-calc-error"; }
                if (!li) says->set_tooltip_text("Can't read it: " + li.error().message);
            }
            grid->attach(*cell(now, css), 2, r);
            auto edit = Gtk::make_managed<Gtk::Button>();
            edit->set_icon_name("document-edit-symbolic");
            edit->add_css_class("flat");
            edit->set_name("tables.list.edit." + name);
            edit->set_tooltip_text("Change " + curly(name));
            edit->set_sensitive(can && !editing);
            edit->signal_clicked().connect([this, name, v, table] {
                m_vl_editing = name;
                m_vl_new = false;
                m_vl_typed = v;
                m_vl_typed_text = core::list_values_text(v.values);
                if (const db::Table* tt = m_schema.table(table)) list_section(*tt);
            });
            grid->attach(*edit, 3, r);
            auto del = Gtk::make_managed<Gtk::Button>();
            del->set_icon_name("user-trash-symbolic");
            del->add_css_class("flat");
            del->set_name("tables.list.delete." + name);
            del->set_tooltip_text("Delete " + curly(name) + " -- Ctrl+Z brings it back");
            del->set_sensitive(can && !editing);
            del->signal_clicked().connect([this, table, name] { drop_list(table, name); });
            grid->attach(*del, 4, r);
            ++r;
        }
        if (r > 1) body->append(*grid);
    }

    if (!editing) {
        if (lists.empty()) {
            auto l = cell("None yet. A value list is the choices a field's box offers as a drop-down -- typed (home, work, "
                          "mobile) or every value of a field (contacts' cities, or their ids showing last names). It lives "
                          "in the solution; a layout's field picks one in its inspector.", "acorn-status");
            l->set_wrap(true);
            l->set_max_width_chars(70);
            body->append(*l);
        }
        auto add = Gtk::make_managed<Gtk::Button>("New Value List…");
        add->set_name("tables.list.new");
        add->set_halign(Gtk::Align::START);
        add->set_sensitive(can);
        add->set_tooltip_text(can ? "Typed values, or the values of a field" : "Open a database first");
        add->signal_clicked().connect([this, table] {
            m_vl_new = true;
            m_vl_editing.clear();
            m_vl_typed = core::ValueList{table, ""};
            m_vl_typed_text.clear();
            if (const db::Table* tt = m_schema.table(table)) list_section(*tt);
        });
        body->append(*add);
    } else {
        const std::string was = m_vl_editing;
        auto ed = Gtk::make_managed<Gtk::Grid>();
        ed->set_column_spacing(8);
        ed->set_row_spacing(6);
        ed->add_css_class("acorn-calc-editor");
        auto name_e = Gtk::make_managed<Gtk::Entry>();
        name_e->set_name("tables.list.name");
        name_e->set_placeholder_text("name");
        name_e->set_width_chars(18);
        name_e->set_text(m_vl_typed.name);
        auto kind_dd = Gtk::make_managed<Gtk::DropDown>(
            Gtk::StringList::create({"Values typed here", "Values of a field"}));
        kind_dd->set_name("tables.list.kind");
        const bool typed = m_vl_typed.typed();
        kind_dd->set_selected(typed ? 0 : 1);
        ed->attach(*name_e, 0, 0);
        ed->attach(*kind_dd, 1, 0, 3, 1);

        // Whatever is typed so far into the members, before a repaint.
        Gtk::TextView* text = nullptr;
        Gtk::DropDown* from_dd = nullptr;
        Gtk::DropDown* field_dd = nullptr;
        Gtk::DropDown* shows_dd = nullptr;
        auto tables = std::make_shared<std::vector<std::string>>();
        auto fields = std::make_shared<std::vector<std::string>>();   // `from`'s, binary left out
        if (typed) {
            text = Gtk::make_managed<Gtk::TextView>();
            text->set_name("tables.list.values");
            text->get_buffer()->set_text(m_vl_typed_text);
            text->set_wrap_mode(Gtk::WrapMode::NONE);
            text->set_top_margin(4);
            text->set_left_margin(6);
            auto sw = Gtk::make_managed<Gtk::ScrolledWindow>();
            sw->set_child(*text);
            sw->set_min_content_height(112);
            sw->set_hexpand(true);
            sw->add_css_class("acorn-list-values");
            ed->attach(*sw, 0, 1, 4, 1);
            auto hint = cell("One value per line, in the order the drop-down offers them.", "acorn-small");
            ed->attach(*hint, 0, 2, 4, 1);
        } else {
            std::vector<Glib::ustring> tw;
            guint t_at = 0;
            for (const auto& tb : m_schema.tables) {
                if (tb.name == m_vl_typed.from) t_at = static_cast<guint>(tables->size());
                tables->push_back(tb.name);
                tw.push_back(tb.name);
            }
            from_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(tw));
            from_dd->set_name("tables.list.from");
            if (!tables->empty()) from_dd->set_selected(t_at);
            std::vector<Glib::ustring> fw;
            std::vector<Glib::ustring> sw = {"the value itself"};
            guint f_at = 0, s_at = 0;
            if (const db::Table* ft = m_schema.table(m_vl_typed.from))
                for (const auto& c : ft->columns) {
                    if (c.type == db::FieldType::Container) continue;   // binary: nothing to offer
                    if (c.name == m_vl_typed.field) f_at = static_cast<guint>(fields->size());
                    if (c.name == m_vl_typed.shows) s_at = static_cast<guint>(fields->size() + 1);
                    fields->push_back(c.name);
                    fw.push_back(c.name + "  (" + db::to_string(c.type) + ")");
                    sw.push_back(c.name);
                }
            field_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(fw));
            field_dd->set_name("tables.list.field");
            field_dd->set_hexpand(true);
            if (!fields->empty()) field_dd->set_selected(f_at);
            shows_dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(sw));
            shows_dd->set_name("tables.list.shows");
            shows_dd->set_selected(s_at);
            ed->attach(*cell("every value of"), 0, 1);
            ed->attach(*from_dd, 1, 1);
            ed->attach(*cell("·"), 2, 1);
            ed->attach(*field_dd, 3, 1);
            ed->attach(*cell("showing"), 0, 2);
            ed->attach(*shows_dd, 1, 2, 3, 1);
        }
        auto live = cell("", "acorn-status");
        live->set_name("tables.list.live");
        live->set_wrap(true);
        ed->attach(*live, 0, 3, 4, 1);
        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_halign(Gtk::Align::END);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->set_name("tables.list.cancel");
        auto save = Gtk::make_managed<Gtk::Button>(was.empty() ? "Add" : "Save");
        save->set_name("tables.list.save");
        save->add_css_class("acorn-primary");
        buttons->append(*cancel);
        buttons->append(*save);
        ed->attach(*buttons, 0, 4, 4, 1);
        body->append(*ed);

        // Read the widgets into m_vl_typed (and the text as typed).
        auto gather = [this, name_e, text, from_dd, field_dd, shows_dd, tables, fields, table] {
            m_vl_typed.table = table;
            m_vl_typed.name = name_e->get_text();
            if (text) {
                m_vl_typed_text = text->get_buffer()->get_text();
                m_vl_typed.values = core::parse_list_values(m_vl_typed_text);
                m_vl_typed.from.clear();
                m_vl_typed.field.clear();
                m_vl_typed.shows.clear();
            } else {
                m_vl_typed.values.clear();
                const auto ti = from_dd->get_selected();
                m_vl_typed.from = ti < tables->size() ? (*tables)[ti] : std::string{};
                const auto fi = field_dd->get_selected();
                m_vl_typed.field = fi < fields->size() ? (*fields)[fi] : std::string{};
                const auto si = shows_dd->get_selected();
                m_vl_typed.shows = si > 0 && si - 1 < fields->size() ? (*fields)[si - 1] : std::string{};
            }
        };
        // The live line: what's wrong, or what it offers now.
        auto check = [this, gather, live, save, table, was] {
            gather();
            const auto& v = m_vl_typed;
            std::vector<std::string> taken, from_fields;
            for (const auto& [n, x] : m_solution.lists_of(table)) if (n != (was.empty() ? v.name : was)) taken.push_back(n);
            const db::Table* ft = v.typed() ? nullptr : m_schema.table(v.from);
            if (ft) for (const auto& c : ft->columns) from_fields.push_back(c.name);
            std::string why = v.name.empty() ? "Give it a name." : core::check_value_list(v, taken, ft ? &from_fields : nullptr);
            if (why.empty() && !v.typed() && v.field.empty()) why = "Pick the field whose values it offers.";
            live->remove_css_class("error");
            if (!why.empty()) {
                live->set_text(why);
                // Not red until something is there to be wrong.
                if (!v.name.empty() && !(v.typed() && m_vl_typed_text.find_first_not_of(" \t\r\n") == std::string::npos))
                    live->add_css_class("error");
                save->set_sensitive(false);
                return;
            }
            save->set_sensitive(true);
            if (v.typed()) { live->set_text(offers_text({core::typed_items(v), false})); return; }
            if (!m_conn) { live->set_text("Fine."); return; }
            auto li = db::read_value_list(*m_conn, m_schema, v);
            live->set_text(li ? offers_text(*li) : "? -- " + li.error().message);
        };
        check();
        name_e->signal_changed().connect(check);
        if (text) text->get_buffer()->signal_changed().connect(check);
        if (field_dd) field_dd->property_selected().signal_changed().connect(check);
        if (shows_dd) shows_dd->property_selected().signal_changed().connect(check);
        // Another kind, or another table: the editor is drawn again for it --
        // after the drop-down's own signal is done (it goes with the repaint).
        auto redraw = [this, table] {
            Glib::signal_idle().connect_once([this, table] {
                if (const db::Table* tt = m_schema.table(table)) list_section(*tt);
            });
        };
        kind_dd->property_selected().signal_changed().connect([this, kind_dd, gather, redraw, table] {
            gather();
            const bool want_typed = kind_dd->get_selected() == 0;
            if (want_typed == m_vl_typed.typed()) return;
            if (want_typed) {
                m_vl_typed.field.clear();
                m_vl_typed.from.clear();
                m_vl_typed.shows.clear();
            } else {   // from a field: this table, its first field that isn't binary
                m_vl_typed.from = table;
                m_vl_typed.field.clear();
                if (const db::Table* tt = m_schema.table(table))
                    for (const auto& c : tt->columns)
                        if (c.type != db::FieldType::Container) { m_vl_typed.field = c.name; break; }
                if (m_vl_typed.field.empty()) m_vl_typed.field = "?";
            }
            redraw();
        });
        if (from_dd)
            from_dd->property_selected().signal_changed().connect([this, from_dd, tables, redraw] {
                const auto ti = from_dd->get_selected();
                if (ti >= tables->size() || (*tables)[ti] == m_vl_typed.from) return;
                m_vl_typed.from = (*tables)[ti];
                m_vl_typed.field.clear();
                m_vl_typed.shows.clear();
                if (const db::Table* ft = m_schema.table(m_vl_typed.from))
                    for (const auto& c : ft->columns)
                        if (c.type != db::FieldType::Container) { m_vl_typed.field = c.name; break; }
                if (m_vl_typed.field.empty()) m_vl_typed.field = "?";
                redraw();
            });
        auto close = [this, table] {
            m_vl_new = false;
            m_vl_editing.clear();
            if (const db::Table* tt = m_schema.table(table)) list_section(*tt);
        };
        cancel->signal_clicked().connect(close);
        auto commit = [this, gather, save, was] {
            gather();
            if (!save->get_sensitive()) return;
            keep_list(m_vl_typed, was);
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
        ed->add_controller(keys);
        keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        if (m_vl_new && m_vl_typed.name.empty()) focus = name_e;
        else if (text) focus = text;
    }

    // Repaint in place: keep the heading, the rest is the new body.
    if (m_vl_box && m_vl_box->get_parent()) {
        while (auto* c = m_vl_box->get_last_child()) {
            if (c == m_vl_box->get_first_child()) break;
            m_vl_box->remove(*c);
        }
        m_vl_box->append(*body);
    }
    if (focus) focus->grab_focus();
}

// ── s014d: a field's box offering its value list (Form / List) ─────────────

const db::ListItems* Shell::form_list(const std::string& table, const std::string& name) {  // lists: read once per view
    if (m_form_lists_gen != m_view_gen) {
        m_form_lists.clear();
        m_form_lists_error.clear();
        m_form_lists_gen = m_view_gen;
    }
    const std::string key = table + "/" + name;
    if (auto it = m_form_lists.find(key); it != m_form_lists.end()) return &it->second;
    if (m_form_lists_error.contains(key)) return nullptr;
    const core::ValueList* v = m_solution.list(table, name);
    if (!v || !m_conn) return nullptr;
    auto r = db::read_value_list(*m_conn, m_schema, *v);
    if (auto lg = log::get(log::Area::App))
        lg->debug("form: value list '{}' of '{}': {}", name, table,
                  r ? std::to_string(r->items.size()) + " items" : r.error().message);
    if (!r) { m_form_lists_error[key] = r.error().message; return nullptr; }
    return &(m_form_lists[key] = std::move(*r));
}

Gtk::Widget* Shell::list_pick(const core::ValueList& v, const db::ListItems& li, const std::string& field,
                              const std::string& shown, const std::string& row_id, unsigned gen) {  // lists: pick-only drop-down
    auto values = std::make_shared<std::vector<std::string>>();
    std::vector<Glib::ustring> words;
    values->push_back({});
    words.push_back("—");   // empty
    guint at = 0;
    for (const auto& it : li.items) {
        if (it.value == shown) at = static_cast<guint>(values->size());
        values->push_back(it.value);
        words.push_back(it.shown);
    }
    if (!shown.empty() && at == 0) {   // a value the list doesn't offer: still shown, as it is
        at = static_cast<guint>(values->size());
        values->push_back(shown);
        words.push_back(shown + "  (not in the list)");
    }
    auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(words));
    dd->set_name("form.list." + field);
    dd->add_css_class("acorn-form-pick");
    dd->set_selected(at);
    if (words.size() > 12) {   // a long list: type to narrow it
        dd->set_expression(Gtk::PropertyExpression<Glib::ustring>::create(Gtk::StringObject::get_type(), "string"));
        dd->set_enable_search(true);
    }
    dd->set_tooltip_text("From " + curly(v.name) + ": a pick saves the " + curly(v.field) + " -- you see the " + curly(v.shows) +
                         (li.more ? " (the first " + std::to_string(core::kValueListMax) + ")" : std::string{}));
    dd->property_selected().signal_changed().connect([this, dd, values, field, row_id, gen] {
        if (m_browse_painting || gen != m_view_gen) return;
        const auto i = dd->get_selected();
        if (i == GTK_INVALID_LIST_POSITION || i >= values->size()) return;
        const std::string value = (*values)[i];
        // After the drop-down's own signal: the save repaints the view it is in.
        Glib::signal_idle().connect_once([this, row_id, field, value, gen] {
            if (gen == m_view_gen) commit_field(row_id, field, value);
        });
    });
    return dd;
}

Gtk::Widget* Shell::list_arrow(const db::ListItems& li, Gtk::Entry* e, const std::string& field,
                               const std::string& row_id, unsigned gen) {  // lists: a typed box's drop arrow
    auto pick = Gtk::make_managed<Gtk::MenuButton>();
    pick->set_icon_name("pan-down-symbolic");
    pick->add_css_class("flat");
    pick->add_css_class("acorn-form-arrow");
    pick->set_name("form.list." + field);
    pick->set_tooltip_text(li.items.empty() ? "The value list offers nothing yet" : "Pick a value -- or type one");
    auto pop = Gtk::make_managed<Gtk::Popover>();
    auto sw = Gtk::make_managed<Gtk::ScrolledWindow>();
    sw->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    sw->set_propagate_natural_height(true);
    sw->set_max_content_height(280);
    auto box = Gtk::make_managed<Gtk::ListBox>();
    box->set_selection_mode(Gtk::SelectionMode::NONE);
    box->set_activate_on_single_click(true);
    box->set_name("form.list_items." + field);
    auto values = std::make_shared<std::vector<std::string>>();
    for (const auto& it : li.items) {
        auto l = Gtk::make_managed<Gtk::Label>(it.shown);
        l->set_xalign(0.0f);
        l->set_margin_start(6);
        l->set_margin_end(12);
        l->set_margin_top(3);
        l->set_margin_bottom(3);
        box->append(*l);
        values->push_back(it.value);
    }
    if (li.items.empty()) box->append(*cell("Nothing to offer yet.", "acorn-status"));
    if (li.more) box->append(*cell("(the first " + std::to_string(core::kValueListMax) + ")", "acorn-status"));
    box->signal_row_activated().connect([this, values, e, pop, field, row_id, gen](Gtk::ListBoxRow* r) {
        if (!r || gen != m_view_gen) return;
        const int i = r->get_index();
        if (i < 0 || static_cast<std::size_t>(i) >= values->size()) return;
        const std::string value = (*values)[static_cast<std::size_t>(i)];
        e->set_text(value);
        pop->popdown();
        Glib::signal_idle().connect_once([this, row_id, field, value, gen] {
            if (gen == m_view_gen) commit_field(row_id, field, value);
        });
    });
    sw->set_child(*box);
    pop->set_child(*sw);
    pick->set_popover(*pop);
    return pick;
}

void Shell::keep_list(const core::ValueList& v, const std::string& was) {  // lists: verbs::save_value_list, the note, a repaint
    if (!m_journal) return;
    const auto before = m_journal->size();
    const bool could_redo = m_journal->can_redo();
    auto r = db::verbs::save_value_list(*m_journal, v, was);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: value list '{}' of '{}' = {}: {}", v.name, v.table, core::value_list_says(v),
                 r ? m_journal->undo_label() : r.error().message);
    if (!r) { journal_note("Not kept: " + r.error().message + ".", "error"); return; }
    m_vl_new = false;
    m_vl_editing.clear();
    if (m_table_sel == v.table) show_table(v.table);
    paint_layouts();
    sync_undo();
    if (m_journal->size() != before || could_redo != m_journal->can_redo())
        journal_note("Did " + m_journal->undo_label() + ". Ctrl+Z undoes it.", "ok");
}

void Shell::drop_list(const std::string& table, const std::string& name) {  // lists: verbs::delete_value_list
    if (!m_journal) return;
    auto r = db::verbs::delete_value_list(*m_journal, table, name);
    if (auto lg = log::get(log::Area::App))
        lg->info("solution: delete value list '{}' of '{}': {}", name, table, r ? "done" : r.error().message);
    if (!r) { journal_note("Not deleted: " + r.error().message + ".", "error"); return; }
    if (m_table_sel == table) show_table(table);
    paint_layouts();
    sync_undo();
    journal_note("Deleted the value list " + curly(name) + ". Ctrl+Z brings it back.", "ok");
}

}  // namespace acorn
