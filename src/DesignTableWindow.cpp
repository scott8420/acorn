#include "DesignTableWindow.hpp"
#include "Registry.hpp"

#include <glibmm/main.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>

#include <algorithm>
#include <cctype>

// DesignTableWindow.cpp -- see the header. Nothing here writes: Apply hands the
// picture to Shell, which runs it through db::Journal as one step.

namespace acorn {

namespace {

// The new-field DropDown's rows, in db::FieldType order (as New Table).
const char* const kTypeNames[] = {"Text", "Integer", "Number", "Date",
                                  "Time", "Timestamp", "Yes / No", "Container"};
constexpr unsigned kTypeCount = sizeof(kTypeNames) / sizeof(kTypeNames[0]);

Gtk::Label* label(const std::string& text, const char* css = nullptr) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    if (css) l->add_css_class(css);
    return l;
}

std::string curly(const std::string& s) { return "“" + s + "”"; }

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string sentence(std::string s) {   // the store's words start lower-case
    if (!s.empty() && std::islower(static_cast<unsigned char>(s[0])))
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (!s.empty() && s.back() != '.') s += ".";
    return s;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

}  // namespace

DesignTableWindow::DesignTableWindow(Gtk::Window& parent)
    : m_title("design.title"),
      m_field_box("design.fields", Gtk::Orientation::VERTICAL, 6),
      m_add_field("design.add_field"),
      m_index_box("design.indexes", Gtk::Orientation::VERTICAL, 6),
      m_ix_name("design.index_name"),
      m_ix_unique("design.index_unique"),
      m_ix_add("design.add_index"),
      m_losses("design.losses"),
      m_understand("design.understand"),
      m_reason("design.reason"),
      m_cancel("design.cancel"),
      m_ok("design.apply") {
    set_name("design");
    registry::add("design", this);
    set_title("Design Table");
    set_modal(true);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(640, 800);

    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    set_titlebar(*hb);

    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    page->set_margin(18);

    m_title.set_xalign(0.0f);
    m_title.add_css_class("title-3");
    page->append(m_title);

    // Fields.
    page->append(*label("Fields -- edit a name to rename it", "heading"));
    auto* fscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    fscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    // A fixed height: the controls below must not jump as rows come and go
    // (the s007b drive clicked a moved button).
    fscroll->set_vexpand(false);
    fscroll->set_min_content_height(210);
    fscroll->set_max_content_height(210);
    fscroll->set_child(m_field_box);
    page->append(*fscroll);
    m_add_field.set_label("New Field");
    m_add_field.set_halign(Gtk::Align::START);
    m_add_field.set_tooltip_text("Add a field at the end -- empty in every record");
    m_add_field.signal_clicked().connect([this] {
        add_field_row(nullptr);
        if (!m_fields.empty()) m_fields.back().name->grab_focus();
        refresh();
    });
    page->append(m_add_field);

    page->append(*Gtk::make_managed<Gtk::Separator>());

    // Indexes.
    page->append(*label("Indexes", "heading"));
    page->append(m_index_box);
    auto* form = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_ix_name.set_placeholder_text("index name");
    m_ix_name.set_hexpand(true);
    m_ix_name.signal_changed().connect([this] { m_ix_add.set_sensitive(m_ix_field && m_ix_field->get_selected() != GTK_INVALID_LIST_POSITION); });
    m_ix_field = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({}));
    m_ix_field->set_tooltip_text("The field the index is on");
    m_ix_unique.set_label("Unique");
    m_ix_unique.set_tooltip_text("No two records may hold the same value here");
    m_ix_add.set_label("Add Index");
    m_ix_add.signal_clicked().connect([this] {
        const unsigned sel = m_ix_field->get_selected();
        if (sel >= m_pick_names.size()) return;
        const std::string orig = m_pick_names[sel];
        std::string name = m_ix_name.get_text();
        if (name.empty()) name = m_live.name + "_" + renamed(orig);
        add_index_row({name, {orig}, m_ix_unique.get_active(), false}, true);
        m_ix_name.set_text("");
        m_ix_unique.set_active(false);
        refresh();
    });
    form->append(m_ix_name);
    form->append(*m_ix_field);
    form->append(m_ix_unique);
    form->append(m_ix_add);
    page->append(*form);

    page->append(*Gtk::make_managed<Gtk::Separator>());

    // The SQL, the words, the tick.
    page->append(*label("The SQL Acorn will run", "heading"));
    m_sql.set_editable(false);
    m_sql.set_cursor_visible(false);
    m_sql.set_monospace(true);
    m_sql.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    m_sql.set_top_margin(6);
    m_sql.set_left_margin(6);
    m_sql.set_name("design.sql");
    auto* sscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    sscroll->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    sscroll->set_min_content_height(150);
    sscroll->set_vexpand(true);
    sscroll->set_child(m_sql);
    page->append(*sscroll);

    m_losses.set_xalign(0.0f);
    m_losses.set_wrap(true);
    m_losses.add_css_class("warning");
    page->append(m_losses);
    m_understand.set_label("I understand -- Ctrl+Z brings them back");
    m_understand.signal_toggled().connect([this] { refresh(); });
    page->append(m_understand);

    auto* note = label("One Apply is one step: Ctrl+Z, or Undo in the menu, undoes all of it.", "dim-label");
    note->set_wrap(true);
    page->append(*note);

    m_reason.set_xalign(0.0f);
    m_reason.set_wrap(true);
    page->append(m_reason);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    buttons->set_halign(Gtk::Align::END);
    m_cancel.set_label("Cancel");
    m_cancel.signal_clicked().connect([this] { set_visible(false); });
    m_ok.set_label("Apply");
    m_ok.add_css_class("suggested-action");
    m_ok.signal_clicked().connect([this] { on_apply(); });
    buttons->append(m_cancel);
    buttons->append(m_ok);
    page->append(*buttons);

    set_child(*page);
}

DesignTableWindow::~DesignTableWindow() { registry::remove(this); }

void DesignTableWindow::start(db::Dialect d, const db::TableDef& live, Preview preview, Losses losses, Apply apply) {
    m_dialect = d;
    m_live = live;
    m_preview_fn = std::move(preview);
    m_losses_fn = std::move(losses);
    m_apply_fn = std::move(apply);
    m_resetting = true;
    set_title("Design " + curly(live.name));
    m_title.set_text(curly(live.name) + " -- " + std::to_string(live.rows) + " record" + (live.rows == 1 ? "" : "s"));
    for (auto& f : m_fields) m_field_box.remove(*f.box);
    m_fields.clear();
    for (auto& x : m_indexes) m_index_box.remove(*x.box);
    m_indexes.clear();
    for (const auto& c : live.columns) add_field_row(&c);
    for (const auto& ix : live.indexes) add_index_row(ix, false);
    m_ix_name.set_text("");
    m_ix_unique.set_active(false);
    m_understand.set_active(false);
    m_reason.set_text("");
    m_resetting = false;
    refresh();
    present();
    // After the window maps (GTK picks a first focus of its own on map -- it
    // chose the key field's name, one keystroke from a rename).
    Glib::signal_idle().connect_once([this] { m_cancel.grab_focus(); });
}

void DesignTableWindow::add_field_row(const db::ColumnDef* live) {
    FieldRow r;
    r.orig = live ? live->name : std::string{};
    r.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    r.name = Gtk::make_managed<Gtk::Entry>();
    r.name->set_hexpand(true);
    r.name->set_text(live ? live->name : "");
    r.name->set_placeholder_text("field name");
    r.name->set_name(live ? "design.field." + live->name : "design.field.new");
    r.name->signal_changed().connect([this] { rebuild_index_fields(); refresh(); });
    r.box->append(*r.name);
    if (live) {
        std::string what = db::to_string(db::map_type(m_dialect, live->decl_type));
        if (live->pk_position > 0) what += live->auto_increment ? " · key, counts up" : " · key";
        else if (!live->nullable) what += " · required";
        auto* t = label(what, "dim-label");
        t->set_size_request(150, -1);
        t->set_tooltip_text(live->decl_type + " -- changing a field's type, required or default comes next (s007c)");
        r.box->append(*t);
        r.del = Gtk::make_managed<Gtk::Button>();
        r.del->set_icon_name("user-trash-symbolic");
        r.del->set_has_frame(false);
        if (live->pk_position > 0) {
            r.del->set_sensitive(false);
            r.del->set_tooltip_text("The key is not changed here");
        } else {
            r.del->set_tooltip_text("Delete this field");
        }
        const std::string orig = r.orig;
        r.del->signal_clicked().connect([this, orig] {
            for (auto& f : m_fields)
                if (f.orig == orig) {
                    f.deleted = !f.deleted;
                    f.name->set_sensitive(!f.deleted);
                    if (f.deleted) f.name->set_text(orig);   // a field going is not also renamed
                    f.del->set_icon_name(f.deleted ? "edit-undo-symbolic" : "user-trash-symbolic");
                    f.del->set_tooltip_text(f.deleted ? "Keep this field" : "Delete this field");
                }
            rebuild_index_fields();
            refresh();
        });
        r.box->append(*r.del);
    } else {
        std::vector<Glib::ustring> types(std::begin(kTypeNames), std::end(kTypeNames));
        r.type = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(types));
        r.type->set_selected(0);
        r.type->property_selected().signal_changed().connect([this] { refresh(); });
        r.box->append(*r.type);
        r.del = Gtk::make_managed<Gtk::Button>();
        r.del->set_icon_name("list-remove-symbolic");
        r.del->set_has_frame(false);
        r.del->set_tooltip_text("Don't add this field");
        Gtk::Box* box = r.box;
        r.del->signal_clicked().connect([this, box] {
            for (auto it = m_fields.begin(); it != m_fields.end(); ++it)
                if (it->box == box) { m_field_box.remove(*box); m_fields.erase(it); break; }
            refresh();
        });
        r.box->append(*r.del);
        r.name->signal_activate().connect([this] { on_apply(); });
    }
    m_field_box.append(*r.box);
    m_fields.push_back(r);
    rebuild_index_fields();
}

void DesignTableWindow::add_index_row(const db::IndexDef& ix, bool made) {
    IndexRow r;
    r.ix = ix;
    r.made = made;
    r.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto* n = label(ix.name, "monospace");
    n->set_size_request(200, -1);
    n->set_ellipsize(Pango::EllipsizeMode::END);
    n->set_tooltip_text(ix.name);
    r.box->append(*n);
    r.fields = label("");
    r.fields->set_hexpand(true);
    r.box->append(*r.fields);
    if (ix.unique) r.box->append(*label("UNIQUE", "dim-label"));
    if (made) r.box->append(*label("new", "accent"));
    r.del = Gtk::make_managed<Gtk::Button>();
    r.del->set_has_frame(false);
    Gtk::Box* box = r.box;
    if (made) {
        r.del->set_icon_name("list-remove-symbolic");
        r.del->set_tooltip_text("Don't make this index");
        r.del->signal_clicked().connect([this, box] {
            for (auto it = m_indexes.begin(); it != m_indexes.end(); ++it)
                if (it->box == box) { m_index_box.remove(*box); m_indexes.erase(it); break; }
            refresh();
        });
    } else {
        r.del->set_icon_name("user-trash-symbolic");
        r.del->set_tooltip_text("Delete this index (the data is untouched)");
        r.del->signal_clicked().connect([this, box] {
            for (auto& x : m_indexes)
                if (x.box == box) {
                    x.deleted = !x.deleted;
                    x.del->set_icon_name(x.deleted ? "edit-undo-symbolic" : "user-trash-symbolic");
                    x.del->set_tooltip_text(x.deleted ? "Keep this index" : "Delete this index (the data is untouched)");
                }
            refresh();
        });
    }
    if (ix.partial) {   // Acorn reads these, never makes or drops them
        r.del->set_sensitive(false);
        r.del->set_tooltip_text("A partial index -- not changed here");
    }
    r.box->append(*r.del);
    m_index_box.append(*r.box);
    m_indexes.push_back(r);
}

void DesignTableWindow::rebuild_index_fields() {
    if (!m_ix_field) return;
    m_pick_names.clear();
    std::vector<Glib::ustring> shown;
    for (const auto& f : m_fields) {
        if (f.orig.empty() || f.deleted) continue;   // a new field: index it after Apply
        m_pick_names.push_back(f.orig);
        shown.push_back(std::string(f.name->get_text()));
    }
    m_ix_field->set_model(Gtk::StringList::create(shown));
    m_ix_field->set_selected(shown.empty() ? GTK_INVALID_LIST_POSITION : 0u);
    m_ix_add.set_sensitive(!shown.empty());
}

std::string DesignTableWindow::renamed(const std::string& orig) const {
    for (const auto& f : m_fields)
        if (f.orig == orig) return f.name->get_text();
    return orig;
}

bool DesignTableWindow::field_going(const std::string& orig) const {
    for (const auto& f : m_fields)
        if (f.orig == orig) return f.deleted;
    return true;   // not a field any more
}

db::TableDef DesignTableWindow::want() const {
    db::TableDef w;
    w.name = m_live.name;
    w.rows = m_live.rows;
    for (const auto& f : m_fields) {
        if (f.orig.empty()) {
            const std::string n = f.name->get_text();
            if (n.empty()) continue;   // an unused new line
            const unsigned sel = f.type->get_selected();
            const auto ft = static_cast<db::FieldType>(sel < kTypeCount ? sel : 0u);
            w.columns.push_back({n, db::decl_for(m_dialect, ft), true, std::nullopt, 0, false});
            continue;
        }
        if (f.deleted) continue;
        const db::ColumnDef* c = m_live.column(f.orig);
        if (!c) continue;
        db::ColumnDef x = *c;
        x.name = f.name->get_text();
        w.columns.push_back(x);
    }
    for (const auto& r : m_indexes) {
        if (r.deleted) continue;
        bool going = false;
        db::IndexDef ix = r.ix;
        for (auto& col : ix.columns) {
            if (field_going(col)) going = true;
            col = renamed(col);
        }
        if (going) continue;   // an index goes with its field
        w.indexes.push_back(ix);
    }
    std::sort(w.indexes.begin(), w.indexes.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return w;
}

std::string DesignTableWindow::why_not() const {
    std::vector<std::string> seen;
    int renames = 0, deleted = 0, added = 0;
    const db::ColumnDef* gone = nullptr;
    const FieldRow* fresh = nullptr;
    for (const auto& f : m_fields) {
        const std::string n = f.name->get_text();
        if (f.orig.empty() && n.empty()) continue;
        if (f.deleted) { ++deleted; gone = m_live.column(f.orig); continue; }
        if (f.orig.empty()) { ++added; fresh = &f; }
        else if (n != f.orig) ++renames;
        if (auto why = db::check_name("The field", n); !why.empty()) return sentence(why);
        for (const auto& s : seen)
            if (lower(s) == lower(n)) return "Two fields are named " + curly(n) + ".";
        seen.push_back(n);
    }
    if (renames > 1) return "Rename one field at a time -- Apply, then the next.";
    if (renames == 1 && (deleted || added)) return "A rename is a step of its own -- Apply it, then the rest.";
    // Pictures carry no field identity: one field gone + one of the same kind
    // come would read as a rename. Two steps.
    if (deleted == 1 && added == 1 && gone && fresh) {
        const unsigned sel = fresh->type->get_selected();
        const auto ft = static_cast<db::FieldType>(sel < kTypeCount ? sel : 0u);
        if (db::map_type(m_dialect, gone->decl_type) == ft && gone->nullable)
            return "Deleting " + curly(gone->name) + " and adding a field of the same kind in one step would read as a "
                   "rename -- Apply the delete first (or rename " + curly(gone->name) + " instead).";
    }
    for (const auto& r : m_indexes) {
        if (!r.made) continue;
        for (const auto& x : m_indexes)
            if (&x != &r && lower(x.ix.name) == lower(r.ix.name) && !(x.deleted && !x.made))
                return "Two indexes are named " + curly(r.ix.name) + ".";
    }
    return {};
}

void DesignTableWindow::refresh() {
    if (m_resetting) return;
    // Index rows say which fields, by their names in the window.
    for (auto& r : m_indexes) {
        std::vector<std::string> names;
        std::string goes_with;
        for (const auto& col : r.ix.columns) {
            names.push_back(renamed(col));
            if (field_going(col) && goes_with.empty()) goes_with = col;
        }
        std::string t = "(" + join(names, ", ") + ")";
        if (!goes_with.empty()) t += " -- goes with " + curly(goes_with);
        else if (r.deleted) t += " -- will be deleted";
        r.fields->set_text(t);
        r.box->set_opacity(!goes_with.empty() || r.deleted ? 0.55 : 1.0);
    }
    for (auto& f : m_fields) f.box->set_opacity(f.deleted ? 0.55 : 1.0);

    const auto w = want();
    std::string why = why_not();
    std::vector<std::string> plan;
    if (why.empty() && m_preview_fn) {
        auto p = m_preview_fn(w);
        if (!p) why = sentence(p.error().message);
        else plan = *p;
    }
    if (why.empty())
        m_sql.get_buffer()->set_text(plan.empty() ? "-- nothing to change yet" : join(plan, ";\n\n") + ";");
    else
        m_sql.get_buffer()->set_text("-- " + why);

    std::vector<std::string> losses;
    if (why.empty() && !plan.empty() && m_losses_fn) {
        auto l = m_losses_fn(w);
        if (!l) why = sentence(l.error().message);
        else losses = *l;
    }
    const std::string loss_text = losses.empty() ? std::string{} : join(losses, ".\n") + ".";
    if (loss_text != std::string(m_losses.get_text())) {
        // Different words: a tick given for other words doesn't count.
        m_understand.set_active(false);
        m_losses.set_text(loss_text);
    }
    m_losses.set_visible(!losses.empty());
    m_understand.set_visible(!losses.empty());

    const bool ok = why.empty() && !plan.empty() && (losses.empty() || m_understand.get_active());
    m_ok.set_sensitive(ok);
    m_ok.set_tooltip_text(ok ? "Run it -- one step" : !why.empty() ? why
                              : plan.empty() ? "Nothing to change yet" : "Tick “I understand” first");
    m_reason.set_text("");   // a refusal from the last Apply; any edit clears it
}

void DesignTableWindow::on_apply() {
    if (!m_ok.get_sensitive() || !m_apply_fn) return;
    auto r = m_apply_fn(want());
    if (!r) {
        m_reason.add_css_class("error");
        m_reason.set_text("Not changed: " + sentence(r.error().message));
        return;
    }
    set_visible(false);
}

}  // namespace acorn
