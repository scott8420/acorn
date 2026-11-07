#include "DesignTableWindow.hpp"
#include "Registry.hpp"
#include "db/Convert.hpp"

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
    set_default_size(860, 820);

    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    set_titlebar(*hb);

    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    page->set_margin(18);

    m_title.set_xalign(0.0f);
    m_title.add_css_class("title-3");
    page->append(m_title);

    // Fields.
    page->append(*label("Fields -- edit a name to rename it; pick a kind, Required, a default to change one", "heading"));
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
    m_add_field.signal_clicked().connect([this, fscroll] {
        add_field_row(nullptr);
        if (!m_fields.empty()) m_fields.back().name->grab_focus();
        refresh();
        // The list is a fixed height: show the new row (Scott, s007c -- with
        // five fields the sixth landed out of sight, looking like nothing).
        Glib::signal_idle().connect_once([fscroll] {
            auto adj = fscroll->get_vadjustment();
            adj->set_value(adj->get_upper() - adj->get_page_size());
        });
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

    m_note = label("One Apply is one step: Ctrl+Z, or Undo in the menu, undoes all of it.", "dim-label");
    m_note->set_wrap(true);
    page->append(*m_note);

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
    const bool key = live && live->pk_position > 0;
    const db::FieldType kind = live ? db::map_type(m_dialect, live->decl_type) : db::FieldType::Text;
    if (key || kind == db::FieldType::Unknown) {
        // A key field (its kind, required, default stay -- s007d), or a type
        // Acorn doesn't map: shown, not changed.
        std::string what = key ? std::string(db::to_string(kind)) : live->decl_type;
        auto* t = label(what, "dim-label");
        m_mid->add_widget(*t);   // as wide as the kind + Required + default of the other rows
        t->set_tooltip_text(key ? live->decl_type + " -- a key field keeps its kind; move the key off it to change that"
                                : live->decl_type + " -- a type Acorn doesn't map; not changed here");
        r.box->append(*t);
    } else {
        auto* mid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        m_mid->add_widget(*mid);
        std::vector<Glib::ustring> types(std::begin(kTypeNames), std::end(kTypeNames));
        r.type = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(types));
        r.type->set_selected(static_cast<unsigned>(kind));
        r.type->set_size_request(120, -1);
        r.type->set_tooltip_text(live ? live->decl_type + " -- pick another kind to convert its values" : "The kind of field");
        r.type->property_selected().signal_changed().connect([this] { refresh(); });
        mid->append(*r.type);
        r.required = Gtk::make_managed<Gtk::CheckButton>("Required");
        r.required->set_active(live && !live->nullable);
        r.required->set_tooltip_text("Every record must hold a value here");
        r.required->signal_toggled().connect([this] { refresh(); });
        mid->append(*r.required);
        r.dflt = Gtk::make_managed<Gtk::Entry>();
        r.dflt->set_placeholder_text("no default");
        r.dflt->set_text(live ? db::default_text(live->default_sql) : "");
        r.dflt->set_width_chars(10);
        r.dflt->set_tooltip_text("What a new record holds here when nothing is given");
        r.dflt->set_name(live ? "design.default." + live->name : "design.default.new");
        r.dflt->signal_changed().connect([this] { refresh(); });
        mid->append(*r.dflt);
        r.box->append(*mid);
    }
    // s007d: the key.
    r.key = Gtk::make_managed<Gtk::CheckButton>("Key");
    r.key->set_name(live ? "design.key." + live->name : "design.key.new");
    r.key->set_active(key);
    if (live) {
        r.key->set_tooltip_text("Records are found by the key -- filled and different in every record");
    } else {
        r.key->set_sensitive(false);
        r.key->set_tooltip_text("Add the field first, then make it the key");
    }
    r.key->signal_toggled().connect([this] { follow_key(); refresh(); });
    r.box->append(*r.key);
    r.counts = Gtk::make_managed<Gtk::CheckButton>("counts up");
    r.counts->set_name(live ? "design.counts." + live->name : "design.counts.new");
    r.counts->set_active(live && live->auto_increment);
    r.counts->set_opacity(0.0);   // its slot is kept on every row, so the columns line up
    r.counts->signal_toggled().connect([this] { refresh(); });
    r.box->append(*r.counts);

    r.del = Gtk::make_managed<Gtk::Button>();
    r.del->set_has_frame(false);
    if (live) {
        r.del->set_icon_name("user-trash-symbolic");
        if (key) {
            r.del->set_sensitive(false);
            r.del->set_tooltip_text("Part of the key -- move the key off it first");
        } else {
            r.del->set_tooltip_text("Delete this field");
        }
        const std::string orig = r.orig;
        r.del->signal_clicked().connect([this, orig] {
            for (auto& f : m_fields)
                if (f.orig == orig) {
                    f.deleted = !f.deleted;
                    f.name->set_sensitive(!f.deleted);
                    if (f.type) f.type->set_sensitive(!f.deleted);
                    if (f.required) f.required->set_sensitive(!f.deleted);
                    if (f.dflt) f.dflt->set_sensitive(!f.deleted);
                    f.key->set_sensitive(!f.deleted);
                    if (f.deleted) {   // a field going is not also renamed or changed
                        const db::ColumnDef* c = m_live.column(orig);
                        m_resetting = true;
                        f.name->set_text(orig);
                        if (c && f.type) f.type->set_selected(static_cast<unsigned>(db::map_type(m_dialect, c->decl_type)));
                        if (c && f.required) f.required->set_active(!c->nullable);
                        if (c && f.dflt) f.dflt->set_text(db::default_text(c->default_sql));
                        f.key->set_active(false);
                        m_resetting = false;
                    }
                    f.del->set_icon_name(f.deleted ? "edit-undo-symbolic" : "user-trash-symbolic");
                    f.del->set_tooltip_text(f.deleted ? "Keep this field" : "Delete this field");
                }
            rebuild_index_fields();
            refresh();
        });
    } else {
        r.del->set_icon_name("list-remove-symbolic");
        r.del->set_tooltip_text("Don't add this field");
        Gtk::Box* box = r.box;
        r.del->signal_clicked().connect([this, box] {
            for (auto it = m_fields.begin(); it != m_fields.end(); ++it)
                if (it->box == box) { m_field_box.remove(*box); m_fields.erase(it); break; }
            refresh();
        });
        r.name->signal_activate().connect([this] { on_apply(); });
    }
    r.box->append(*r.del);
    m_field_box.append(*r.box);
    m_fields.push_back(r);
    rebuild_index_fields();
    follow_key();
}

db::FieldType DesignTableWindow::kind_of(const FieldRow& f) const {
    if (f.type) {
        const unsigned sel = f.type->get_selected();
        return static_cast<db::FieldType>(sel < kTypeCount ? sel : 0u);
    }
    const db::ColumnDef* c = f.orig.empty() ? nullptr : m_live.column(f.orig);
    return c ? db::map_type(m_dialect, c->decl_type) : db::FieldType::Text;
}

std::vector<std::string> DesignTableWindow::key_wanted(bool& counting) const {
    std::vector<std::string> k;
    const FieldRow* lone = nullptr;
    for (const auto& f : m_fields) {
        if (f.deleted || f.orig.empty() || !f.key->get_active()) continue;
        k.push_back(f.name->get_text());
        lone = &f;
    }
    counting = k.size() == 1 && lone && kind_of(*lone) == db::FieldType::Integer && lone->counts->get_active();
    return k;
}

// The "counts up" toggle shows on the one row that is the key alone, when it
// is a whole number. SQLite: a lone INTEGER key always counts (its rule) --
// shown ticked, not changeable.
void DesignTableWindow::follow_key() {
    if (m_following) return;
    m_following = true;
    int keys = 0;
    for (const auto& f : m_fields) if (!f.deleted && !f.orig.empty() && f.key->get_active()) ++keys;
    for (auto& f : m_fields) {
        const bool lone = keys == 1 && !f.deleted && !f.orig.empty() && f.key->get_active() &&
                          kind_of(f) == db::FieldType::Integer;
        f.counts->set_opacity(lone ? 1.0 : 0.0);
        f.counts->set_can_target(lone);
        f.counts->set_can_focus(lone);
        const db::ColumnDef* c = f.orig.empty() ? nullptr : m_live.column(f.orig);
        const bool rowid = m_dialect == db::Dialect::SQLite && c && lower(c->decl_type) == "integer";
        if (lone && rowid) {
            f.counts->set_active(true);
            f.counts->set_sensitive(false);
            f.counts->set_tooltip_text("On SQLite a key of one INTEGER field always counts up -- the engine's own rule");
        } else {
            f.counts->set_sensitive(lone);
            f.counts->set_tooltip_text("A new record gets the next number by itself");
        }
    }
    m_following = false;
}

std::expected<db::ColumnDef, std::string> DesignTableWindow::field_of(const FieldRow& f) const {
    const std::string n = f.name->get_text();
    const db::ColumnDef* c = f.orig.empty() ? nullptr : m_live.column(f.orig);
    db::ColumnDef x = c ? *c : db::ColumnDef{n, db::decl_for(m_dialect, db::FieldType::Text), true, std::nullopt, 0, false};
    x.name = n;
    if (!f.type) return x;   // the key, or a type Acorn doesn't map: as it is
    const unsigned sel = f.type->get_selected();
    const auto ft = static_cast<db::FieldType>(sel < kTypeCount ? sel : 0u);
    // The same kind keeps the engine's own spelling ("VARCHAR(40)" stays).
    if (!c || db::map_type(m_dialect, c->decl_type) != ft) x.decl_type = db::decl_for(m_dialect, ft);
    x.nullable = !f.required->get_active();
    const std::string typed = f.dflt->get_text();
    if (c && typed == db::default_text(c->default_sql) && db::map_type(m_dialect, c->decl_type) == ft) {
        x.default_sql = c->default_sql;   // untouched: the engine's own words
    } else {
        auto d = db::default_sql_for(m_dialect, ft, n.empty() ? f.orig : n, typed);
        if (!d) return std::unexpected(d.error());
        x.default_sql = *d;
    }
    return x;
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
        if (f.orig.empty() && f.name->get_text().empty()) continue;   // an unused new line
        if (f.deleted) continue;
        if (!f.orig.empty() && !m_live.column(f.orig)) continue;
        auto x = field_of(f);
        if (x) { w.columns.push_back(*x); continue; }
        // A default that won't do: why_not() says so and Apply stays grey.
        db::ColumnDef keep = f.orig.empty() ? db::ColumnDef{f.name->get_text(), db::decl_for(m_dialect, db::FieldType::Text),
                                                            true, std::nullopt, 0, false}
                                            : *m_live.column(f.orig);
        keep.name = f.name->get_text();
        w.columns.push_back(keep);
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
    // s008: the table's links stay as they are (the graph changes them); a
    // renamed field takes its link along, a deleted one is refused in words.
    w.links = m_live.links;
    for (auto& l : w.links)
        for (auto& col : l.columns) col = renamed(col);
    // s007d: the key as the Key ticks have it (the verb's own rules: fields
    // joining become required, leaving stay required).
    bool counting = false;
    const auto kf = key_wanted(counting);
    std::vector<std::pair<int, std::string>> lk;
    bool lc = false;
    for (const auto& c : m_live.columns) {
        if (c.pk_position > 0) lk.push_back({c.pk_position, c.name});
        if (c.auto_increment) lc = true;
    }
    std::sort(lk.begin(), lk.end());
    std::vector<std::string> live_key;
    for (const auto& [p, n] : lk) live_key.push_back(n);
    if (kf != live_key || counting != lc)
        if (auto k = db::verbs::keyed(w, kf, counting)) w = *k;
    return w;
}

// Pictures carry no field identity: one field gone + one of the same kind come
// IS a rename to the planner (and the store refuses it as one step). So Apply
// runs it as two: the delete (and any other change), then the new field.
bool DesignTableWindow::two_steps() const {
    const db::ColumnDef* gone = nullptr;
    const FieldRow* fresh = nullptr;
    int deleted = 0, added = 0;
    for (const auto& f : m_fields) {
        if (f.orig.empty() && f.name->get_text().empty()) continue;
        if (f.deleted) { ++deleted; gone = m_live.column(f.orig); }
        else if (f.orig.empty()) { ++added; fresh = &f; }
    }
    if (deleted != 1 || added != 1 || !gone || !fresh) return false;
    auto fx = field_of(*fresh);
    return fx && db::map_type(m_dialect, gone->decl_type) == db::map_type(m_dialect, fx->decl_type) &&
           gone->nullable == fx->nullable && gone->default_sql == fx->default_sql;
}

db::TableDef DesignTableWindow::first_step() const {
    std::vector<std::string> fresh;   // the new fields' names
    for (const auto& f : m_fields)
        if (f.orig.empty()) fresh.push_back(f.name->get_text());
    db::TableDef w = want();
    std::erase_if(w.columns, [&](const db::ColumnDef& c) {
        return std::find(fresh.begin(), fresh.end(), c.name) != fresh.end();
    });
    return w;
}

std::string DesignTableWindow::why_not() const {
    std::vector<std::string> seen;
    int renames = 0, deleted = 0, added = 0, changed = 0;
    const db::ColumnDef* gone = nullptr;
    const FieldRow* fresh = nullptr;
    for (const auto& f : m_fields) {
        const std::string n = f.name->get_text();
        if (f.orig.empty() && n.empty()) continue;
        if (f.deleted) { ++deleted; gone = m_live.column(f.orig); continue; }
        if (f.orig.empty()) { ++added; fresh = &f; }
        else if (n != f.orig) ++renames;
        if (auto why = db::check_name("The field", n); !why.empty()) return sentence(why);
        auto x = field_of(f);
        if (!x) return sentence(x.error());
        if (!f.orig.empty()) {
            if (const db::ColumnDef* c = m_live.column(f.orig)) {
                db::ColumnDef same = *x;
                same.name = c->name;
                if (!(same == *c)) ++changed;
            }
        }
        for (const auto& s : seen)
            if (lower(s) == lower(n)) return "Two fields are named " + curly(n) + ".";
        seen.push_back(n);
    }
    if (renames > 1) return "Rename one field at a time -- Apply, then the next.";
    if (renames == 1 && (deleted || added || changed))
        return "A rename is a step of its own -- Apply it, then the rest.";
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
    const bool two = why.empty() && two_steps();
    m_note->set_text(two ? "This Apply is two steps (the delete, then the new field): two Ctrl+Z undo all of it."
                         : "One Apply is one step: Ctrl+Z, or Undo in the menu, undoes all of it.");
    const db::TableDef first = two ? first_step() : w;
    if (why.empty() && m_preview_fn) {
        auto p = m_preview_fn(first);
        if (!p) why = sentence(p.error().message);
        else plan = *p;
    }
    if (why.empty() && two) {   // the second step, read off the first's picture
        auto p2 = db::plan_alter(m_dialect, m_dialect == db::Dialect::MariaDB ? '`' : '"', first, w);
        if (!p2) why = sentence(p2.error());
        else {
            plan.push_back("-- then a second step (two Ctrl+Z undo it all):");
            plan.insert(plan.end(), p2->begin(), p2->end());
        }
    }
    // The words before the SQL box: a refusal about the DATA (required with
    // empty records and no default ...) shows there like any other (s007c).
    std::vector<std::string> losses;
    if (why.empty() && !plan.empty() && m_losses_fn) {
        auto l = m_losses_fn(first);
        if (!l) why = sentence(l.error().message);
        else losses = *l;
    }
    if (why.empty()) {
        std::vector<std::string> shown;   // a comment line (a write-back) takes no ';'
        for (const auto& st : plan) shown.push_back(st.starts_with("--") ? st : st + ";");
        m_sql.get_buffer()->set_text(plan.empty() ? "-- nothing to change yet" : join(shown, "\n\n"));
    } else {
        m_sql.get_buffer()->set_text("-- " + why);
    }
    const std::string loss_text = losses.empty() ? std::string{} : join(losses, ".\n") + ".";
    if (loss_text != std::string(m_losses.get_text())) {
        // Different words: a tick given for other words doesn't count.
        m_understand.set_active(false);
        m_losses.set_text(loss_text);
    }
    m_losses.set_visible(!losses.empty());
    m_understand.set_visible(!losses.empty());
    {   // s007d: a key change loses nothing -- the tick says what undoes it
        bool keyed = false;
        for (const auto& f : m_fields)
            if (!f.orig.empty() && m_live.column(f.orig) &&
                ((m_live.column(f.orig)->pk_position > 0) != (f.key->get_active() && !f.deleted)))
                keyed = true;
        for (const auto& f : m_fields)
            if (f.counts->get_can_target() && m_live.column(f.orig) && m_live.column(f.orig)->auto_increment != f.counts->get_active())
                keyed = true;
        m_understand.set_label(keyed ? "I understand -- Ctrl+Z puts the key back" : "I understand -- Ctrl+Z brings them back");
    }

    const bool ok = why.empty() && !plan.empty() && (losses.empty() || m_understand.get_active());
    m_ok.set_sensitive(ok);
    m_ok.set_tooltip_text(ok ? (two ? "Run it -- two steps" : "Run it -- one step") : !why.empty() ? why
                              : plan.empty() ? "Nothing to change yet" : "Tick “I understand” first");
    m_reason.set_text("");   // a refusal from the last Apply; any edit clears it
}

void DesignTableWindow::on_apply() {
    if (!m_ok.get_sensitive() || !m_apply_fn) return;
    const auto w = want();
    if (two_steps()) {
        if (auto r1 = m_apply_fn(first_step()); !r1) {
            m_reason.add_css_class("error");
            m_reason.set_text("Not changed: " + sentence(r1.error().message));
            return;
        }
        if (auto r2 = m_apply_fn(w); !r2) {
            // The first step stands (Ctrl+Z undoes it); this window's picture
            // is stale now, so it closes on the next look.
            m_reason.add_css_class("error");
            m_reason.set_text("The first step was applied; the new field was not: " + sentence(r2.error().message) +
                              " Cancel, and open Design… again.");
            m_ok.set_sensitive(false);
            return;
        }
        set_visible(false);
        return;
    }
    auto r = m_apply_fn(w);
    if (!r) {
        m_reason.add_css_class("error");
        m_reason.set_text("Not changed: " + sentence(r.error().message));
        return;
    }
    set_visible(false);
}

}  // namespace acorn
