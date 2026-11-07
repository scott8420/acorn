#include "NewTableWindow.hpp"
#include "Registry.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>

// NewTableWindow.cpp -- see the header. Nothing here writes: Create hands the
// picture to Shell, which runs it through db::Journal.

namespace acorn {

namespace {

// The DropDown's rows, in db::FieldType order (Text .. Container).
const char* const kTypeNames[] = {"Text", "Integer", "Number", "Date",
                                  "Time", "Timestamp", "Yes / No", "Container"};
constexpr unsigned kTypeCount = sizeof(kTypeNames) / sizeof(kTypeNames[0]);

Gtk::Label* label(const char* text) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    return l;
}

}  // namespace

NewTableWindow::NewTableWindow(Gtk::Window& parent)
    : m_name("newtable.name"),
      m_key("newtable.key"),
      m_columns("newtable.columns", Gtk::Orientation::VERTICAL, 6),
      m_add("newtable.add_column"),
      m_reason("newtable.reason"),
      m_cancel("newtable.cancel"),
      m_ok("newtable.create") {
    set_name("newtable");
    registry::add("newtable", this);
    set_title("New Table");
    set_modal(true);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(560, 560);

    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    set_titlebar(*hb);

    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    page->set_margin(18);

    auto* name_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto* nl = label("Name");
    nl->set_size_request(70, -1);
    m_name.set_hexpand(true);
    m_name.set_placeholder_text("customers");
    m_name.signal_changed().connect([this] { refresh(); });
    m_name.signal_activate().connect([this] { on_create(); });
    name_row->append(*nl);
    name_row->append(m_name);
    page->append(*name_row);

    m_key.set_label("Start with a key: “id”, a whole number that counts up");
    m_key.set_active(true);
    m_key.signal_toggled().connect([this] { refresh(); });
    page->append(m_key);

    page->append(*label("Columns"));
    auto* cscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    cscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    cscroll->set_vexpand(true);
    cscroll->set_min_content_height(140);
    cscroll->set_child(m_columns);
    page->append(*cscroll);
    m_add.set_label("Add Column");
    m_add.set_halign(Gtk::Align::START);
    m_add.signal_clicked().connect([this] {
        add_column();
        if (!m_rows.empty()) m_rows.back().name->grab_focus();
    });
    page->append(m_add);

    page->append(*label("The SQL Acorn will run"));
    m_preview.set_editable(false);
    m_preview.set_cursor_visible(false);
    m_preview.set_monospace(true);
    m_preview.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    m_preview.set_top_margin(6);
    m_preview.set_left_margin(6);
    m_preview.set_name("newtable.preview");
    auto* pscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    pscroll->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    pscroll->set_min_content_height(120);
    pscroll->set_child(m_preview);
    page->append(*pscroll);

    auto* note = label("You can undo it: Ctrl+Z, or Undo in the menu.");
    note->add_css_class("dim-label");
    page->append(*note);

    m_reason.set_xalign(0.0f);
    m_reason.set_wrap(true);
    m_reason.add_css_class("error");
    page->append(m_reason);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    buttons->set_halign(Gtk::Align::END);
    m_cancel.set_label("Cancel");
    m_cancel.signal_clicked().connect([this] { set_visible(false); });
    m_ok.set_label("Create");
    m_ok.add_css_class("suggested-action");
    m_ok.signal_clicked().connect([this] { on_create(); });
    buttons->append(m_cancel);
    buttons->append(m_ok);
    page->append(*buttons);

    set_child(*page);
    set_default_widget(m_ok);
}

NewTableWindow::~NewTableWindow() { registry::remove(this); }

void NewTableWindow::start(db::Dialect d, char quote, const db::Schema& existing, Create create) {
    m_dialect = d;
    m_quote = quote;
    m_existing = existing;
    m_create = std::move(create);
    m_resetting = true;
    m_name.set_text("");
    m_key.set_active(true);
    while (!m_rows.empty()) remove_column(m_rows.back().box);
    add_column();
    m_resetting = false;
    m_reason.set_text("");
    refresh();
    present();
    m_name.grab_focus();
}

void NewTableWindow::add_column(const std::string& name, db::FieldType t) {
    ColumnRow r;
    r.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    r.name = Gtk::make_managed<Gtk::Entry>();
    r.name->set_hexpand(true);
    r.name->set_placeholder_text("column name");
    r.name->set_text(name);
    r.name->signal_changed().connect([this] { refresh(); });
    r.name->signal_activate().connect([this] { on_create(); });
    std::vector<Glib::ustring> types(std::begin(kTypeNames), std::end(kTypeNames));
    r.type = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(types));
    r.type->set_selected(static_cast<unsigned>(t) < kTypeCount ? static_cast<unsigned>(t) : 0u);
    r.type->property_selected().signal_changed().connect([this] { refresh(); });
    auto* x = Gtk::make_managed<Gtk::Button>();
    x->set_icon_name("list-remove-symbolic");
    x->set_has_frame(false);
    x->set_tooltip_text("Remove this column");
    Gtk::Box* box = r.box;
    x->signal_clicked().connect([this, box] { remove_column(box); refresh(); });
    r.box->append(*r.name);
    r.box->append(*r.type);
    r.box->append(*x);
    m_columns.append(*r.box);
    m_rows.push_back(r);
    if (!m_resetting) refresh();
}

void NewTableWindow::remove_column(Gtk::Box* box) {
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it)
        if (it->box == box) {
            m_columns.remove(*box);
            m_rows.erase(it);
            return;
        }
}

db::TableDef NewTableWindow::def() const {
    db::TableDef t;
    t.name = m_name.get_text();
    if (m_key.get_active())
        t.columns.push_back({"id", db::decl_for(m_dialect, db::FieldType::Integer), false, std::nullopt, 1, true});
    for (const auto& r : m_rows) {
        const std::string n = r.name->get_text();
        const unsigned sel = r.type->get_selected();
        // A row left completely empty is not a column -- just an unused line.
        if (n.empty() && m_rows.size() > 1) continue;
        const auto ft = static_cast<db::FieldType>(sel < kTypeCount ? sel : 0u);
        t.columns.push_back({n, db::decl_for(m_dialect, ft), true, std::nullopt, 0, false});
    }
    return t;
}

void NewTableWindow::refresh() {
    if (m_resetting) return;
    const auto t = def();
    m_preview.get_buffer()->set_text(db::create_table_sql(m_dialect, m_quote, t));
    const std::string why = db::check_new_table(t, m_existing);
    m_ok.set_sensitive(why.empty());
    m_ok.set_tooltip_text(why);
    m_reason.set_text("");   // a refusal from the last Create; typing clears it
}

void NewTableWindow::on_create() {
    const auto t = def();
    if (const std::string why = db::check_new_table(t, m_existing); !why.empty()) {
        m_reason.set_text(why + ".");
        return;
    }
    if (!m_create) return;
    auto r = m_create(t);
    if (!r) {
        m_reason.set_text("Not created: " + r.error().message + ".");
        return;
    }
    set_visible(false);
}

}  // namespace acorn
