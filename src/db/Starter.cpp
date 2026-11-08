#include "db/Starter.hpp"

#include <iterator>

// db/Starter.cpp -- the starter databases (s009b). Every table, link and
// record goes through the verbs, so a starter is exactly what a person could
// have built by hand -- on any engine.

namespace acorn::db {

namespace {
ColumnDef field(Dialect d, const char* name, FieldType t) { return {name, decl_for(d, t), true, std::nullopt, 0, false}; }
ColumnDef counting_key(Dialect d) { return {"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true}; }

// The made-up people [from, to): the starter takes the first three, the
// sample (s010f) all twelve.
std::expected<void, Error> add_people(Journal& j, std::size_t from, std::size_t to) {
    struct Person {
        const char* first; const char* last; const char* company; const char* email; const char* birthday; const char* notes;
        const char* kind; const char* street; const char* city; const char* region; const char* postal; const char* country;
        std::vector<std::pair<const char*, const char*>> phones;
    };
    static const Person people[] = {
        {"Mabel", "Thorne", "Thorne & Daughters Bindery", "mabel@thorne-bindery.example", "1961-04-12",
         "Rebinds old ledgers. Prefers a phone call to email.",
         "work", "14 Quill Lane", "Harrowgate", "TN", "38401", "USA", {{"work", "555-0142"}, {"mobile", "555-0177"}}},
        {"Rufus", "Kettle", "", "rufus.kettle@example.com", "1978-09-30", "Neighbour. Borrowed the ladder.",
         "home", "7 Orchard Row", "Millbrook", "TN", "38463", "USA", {{"home", "555-0109"}}},
        {"Imogen", "Vale", "Vale Survey Co.", "imogen@valesurvey.example", "", "Met at the county fair.",
         "work", "220 Ridge Road, Suite 4", "Port Ellis", "OR", "97031", "USA", {{"mobile", "555-0188"}}},
        // s010f: the sample's other nine.
        {"Bartholomew", "Quist", "Quist Clockworks", "bart@quistclocks.example", "1949-01-22",
         "Fixed the hall clock twice. Talks about escapements.",
         "work", "3 Gearwheel Court", "Harrowgate", "TN", "38401", "USA", {{"work", "555-0121"}}},
        {"Odette", "Marchbanks", "", "odette.m@example.com", "1985-06-03", "Book club, second Tuesdays.",
         "home", "88 Larkspur Way", "Millbrook", "TN", "38463", "USA", {{"mobile", "555-0156"}, {"home", "555-0133"}}},
        {"Cyrus", "Feld", "Feld Brothers Feed & Seed", "cyrus@feldfeed.example", "1970-11-17",
         "Orders the winter wheat in August.",
         "work", "Route 9, Box 41", "Linden Hollow", "TN", "37096", "USA", {{"work", "555-0165"}}},
        {"Henrietta", "Pike", "County Library", "hpike@countylib.example", "1958-02-28", "Holds the interlibrary loans.",
         "work", "1 Courthouse Square", "Harrowgate", "TN", "38401", "USA", {{"work", "555-0100"}, {"mobile", "555-0190"}}},
        {"Ansel", "Graves", "", "ansel.graves@example.com", "", "Fishing partner. Owes a lure.",
         "home", "12 Mill Pond Road", "Linden Hollow", "TN", "37096", "USA", {{"mobile", "555-0114"}}},
        {"Theodora", "Wren", "Wren Glassworks", "theo@wrenglass.example", "1990-07-19", "Made the lamp for the porch.",
         "work", "560 Furnace Street", "Port Ellis", "OR", "97031", "USA", {{"work", "555-0147"}}},
        {"Lucius", "Mott", "Mott & Lyle, Attorneys", "lmott@mottlyle.example", "1966-12-01", "",
         "work", "40 Bank Street, Floor 2", "Kingsferry", "KY", "42001", "USA", {{"work", "555-0172"}, {"fax", "555-0173"}}},
        {"Pearl", "Ashdown", "", "pearl.ashdown@example.com", "1952-03-14", "Grows the good tomatoes.",
         "home", "5 Sorrel Lane", "Millbrook", "TN", "38463", "USA", {{"home", "555-0128"}}},
        {"Jasper", "Nyland", "Nyland Marine", "jasper@nylandmarine.example", "1981-10-08",
         "Sails out of Port Ellis in the summer.",
         "work", "Pier 3", "Port Ellis", "OR", "97031", "USA", {{"mobile", "555-0183"}}},
    };
    auto text = [](const char* s) { return *s ? Value(std::string(s)) : Value(); };
    for (std::size_t i = from; i < to && i < std::size(people); ++i) {
        const Person& p = people[i];
        auto c = verbs::new_record(j, "contacts", {"first_name", "last_name", "company", "email", "birthday", "notes"},
                                   {text(p.first), text(p.last), text(p.company), text(p.email), text(p.birthday), text(p.notes)});
        if (!c) return std::unexpected(c.error());
        std::string t;
        std::vector<Value> key;
        if (!Store::parse_row_id(*c, t, key) || key.size() != 1)
            return std::unexpected(Error{Error::Code::Internal, "the new contact's key could not be read"});
        if (auto a = verbs::new_record(j, "addresses", {"contact_id", "kind", "street", "city", "region", "postal_code", "country"},
                                       {key[0], text(p.kind), text(p.street), text(p.city), text(p.region), text(p.postal),
                                        text(p.country)});
            !a)
            return std::unexpected(a.error());
        for (const auto& [kind, number] : p.phones)
            if (auto ph = verbs::new_record(j, "phones", {"contact_id", "kind", "number"}, {key[0], text(kind), text(number)}); !ph)
                return std::unexpected(ph.error());
    }
    return {};
}
}  // namespace

std::expected<void, Error> make_address_book(Journal& j) {
    const Dialect d = j.store().dialect();
    j.store().forget_schema();
    for (const char* t : {"contacts", "addresses", "phones"})
        if (j.store().table_info(t))
            return std::unexpected(Error{Error::Code::Exists, std::string("there is already a table named “") + t + "”"});
    j.store().clear_failed();

    TableDef contacts;
    contacts.name = "contacts";
    contacts.columns = {counting_key(d), field(d, "first_name", FieldType::Text), field(d, "last_name", FieldType::Text),
                        field(d, "company", FieldType::Text), field(d, "email", FieldType::Text),
                        field(d, "birthday", FieldType::Date), field(d, "notes", FieldType::Text)};
    contacts.indexes = {{"contacts_last_name", {"last_name"}, false, false}};
    TableDef addresses;
    addresses.name = "addresses";
    addresses.columns = {counting_key(d), field(d, "contact_id", FieldType::Integer), field(d, "kind", FieldType::Text),
                         field(d, "street", FieldType::Text), field(d, "city", FieldType::Text),
                         field(d, "region", FieldType::Text), field(d, "postal_code", FieldType::Text),
                         field(d, "country", FieldType::Text)};
    TableDef phones;
    phones.name = "phones";
    phones.columns = {counting_key(d), field(d, "contact_id", FieldType::Integer), field(d, "kind", FieldType::Text),
                      field(d, "number", FieldType::Text)};
    for (const auto* t : {&contacts, &addresses, &phones})
        if (auto r = verbs::new_table(j, *t); !r) return r;
    for (const char* t : {"addresses", "phones"})
        if (auto r = verbs::new_link(j, t, {"contact_id"}, "contacts", OnDelete::Cascade); !r) return r;

    return add_people(j, 0, 3);
}

std::expected<void, Error> make_sample(Journal& j) {  // s010f
    if (auto r = make_address_book(j); !r) return r;
    if (auto r = add_people(j, 3, 12); !r) return r;
    if (auto r = verbs::save_layout(j, sample_layout()); !r) return r;
    if (auto r = verbs::save_layout(j, sample_list_layout()); !r) return r;   // s012b
    // s015b: a report -- addresses by state, each state's count.
    if (auto r = verbs::save_summary(j, core::Summary{"addresses", kSampleCountName, core::SummaryOp::Count, "id"}); !r) return r;
    if (auto r = verbs::save_layout(j, sample_report_layout()); !r) return r;
    return verbs::save_layout(j, sample_labels_layout());   // s016
}

core::Layout sample_layout() {  // s010f: "Contact Card" -- on the 8 px grid
    using K = core::ObjectKind;
    core::Layout l;
    l.table = "contacts";
    l.name = kSampleLayoutName;
    l.width = 504;
    l.header = 48;
    auto words = [&](const char* s, int y, int w = 144) { l.objects.push_back({K::Text, "", s, 16, y, w, 24}); };
    auto box = [&](const char* f, int x, int y, int w) { l.objects.push_back({K::Field, f, "", x, y, w, 24}); };
    words("Contact Card", 12, 200);                          // the header
    l.objects.push_back({K::Box, "", "", 8, 56, 488, 80});   // a frame behind the name rows
    words("Name", 64);
    box("first_name", 168, 64, 152);
    box("last_name", 328, 64, 160);
    words("Company", 96);
    box("company", 168, 96, 320);
    l.objects.push_back({K::Line, "", "", 16, 144, 472, 8});   // a rule
    words("Email", 160);
    box("email", 168, 160, 320);
    words("Birthday", 192);
    box("birthday", 168, 192, 152);
    words("Notes", 224);
    box("notes", 168, 224, 320);
    words("Phones", 256);                                    // s011a: a portal -- this contact's phones
    core::LayoutObject portal{K::Portal, "contact_id", "", 168, 256, 320, core::kPortalRowH * (1 + core::kPortalRows)};
    portal.from = "phones";
    portal.columns = {"kind", "number"};
    l.objects.push_back(std::move(portal));
    l.footer = 40;                                           // the footer, with words of its own
    l.height = 384 + l.footer;
    words("Acorn's sample", 392, 200);
    return l;
}

core::Layout sample_list_layout() {  // s012b: "Contact List" -- a row per person
    using K = core::ObjectKind;
    core::Layout l;
    l.table = "contacts";
    l.name = kSampleListName;
    l.width = 664;
    l.header = 40;
    auto title = [&](const char* s, int x, int w) {   // column titles: left, over their boxes' text
        core::LayoutObject o{K::Text, "", s, x, 8, w, 24};
        o.align = core::Align::Left;
        l.objects.push_back(std::move(o));
    };
    auto box = [&](const char* f, int x, int w) { l.objects.push_back({K::Field, f, "", x, 48, w, 24}); };
    title("Name", 24, 216);
    title("Company", 256, 168);
    title("Email", 440, 208);
    l.objects.push_back({K::Line, "", "", 16, 32, 632, 8});   // a rule under the titles
    box("first_name", 16, 96);
    box("last_name", 120, 120);
    box("company", 248, 176);
    box("email", 432, 216);
    l.footer = 40;
    l.height = 80 + l.footer;
    core::LayoutObject foot{K::Text, "", "Acorn's sample", 24, 88, 200, 24};
    foot.align = core::Align::Left;
    l.objects.push_back(std::move(foot));
    return l;
}

core::Layout sample_report_layout() {  // s015b: "Addresses by State" -- a leading and a trailing sub-summary
    using K = core::ObjectKind;
    core::Layout l;
    l.table = "addresses";
    l.name = kSampleReportName;
    l.width = 520;   // fits Letter and A4 unscaled
    auto left = [&](const char* s, int x, int y, int w) {
        core::LayoutObject o{K::Text, "", s, x, y, w, 24};
        o.align = core::Align::Left;
        l.objects.push_back(std::move(o));
    };
    auto box = [&](const char* f, int x, int y, int w) { l.objects.push_back({K::Field, f, "", x, y, w, 24}); };
    l.header = 40;                                            // [0, 40) the title
    left("Addresses by State", 16, 8, 280);
    l.subs.push_back({"region", true, 40});                   // [40, 80) the state, above its addresses
    left("State", 16, 48, 64);
    box("region", 88, 48, 64);
    core::LayoutObject who{K::Related, "contact_id", "", 32, 80, 136, 24};   // [80, 112) one address
    who.from = "contacts";
    who.shows = "last_name";
    l.objects.push_back(std::move(who));
    box("city", 176, 80, 144);
    box("street", 328, 80, 176);
    l.subs.push_back({"region", false, 40});                  // [112, 152) the state's count, under them
    left("Addresses in the state", 32, 120, 200);
    box(kSampleCountName, 240, 120, 64);
    l.footer = 40;                                            // [152, 192)
    left("Acorn's sample", 16, 160, 200);
    l.height = 192;
    return l;
}

core::Layout sample_labels_layout() {  // s016: "Mailing Labels" -- two across, 24 px apart
    using K = core::ObjectKind;
    core::Layout l;
    l.table = "addresses";
    l.name = kSampleLabelsName;
    l.width = 248;   // two across with the gap: 520 -- Letter and A4 unscaled
    l.across = 2;
    l.gap = 24;
    core::LayoutObject who{K::Related, "contact_id", "", 8, 8, 232, 24};   // the contact's last name, through the link
    who.from = "contacts";
    who.shows = "last_name";
    l.objects.push_back(std::move(who));
    auto box = [&](const char* f, int x, int y, int w) { l.objects.push_back({K::Field, f, "", x, y, w, 24}); };
    box("street", 8, 32, 232);
    box("city", 8, 56, 120);
    box("region", 136, 56, 40);
    box("postal_code", 184, 56, 56);
    l.height = 88;
    return l;
}

}  // namespace acorn::db
