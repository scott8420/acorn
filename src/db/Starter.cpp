#include "db/Starter.hpp"

// db/Starter.cpp -- the starter databases (s009b). Every table, link and
// record goes through the verbs, so a starter is exactly what a person could
// have built by hand -- on any engine.

namespace acorn::db {

namespace {
ColumnDef field(Dialect d, const char* name, FieldType t) { return {name, decl_for(d, t), true, std::nullopt, 0, false}; }
ColumnDef counting_key(Dialect d) { return {"id", decl_for(d, FieldType::Integer), false, std::nullopt, 1, true}; }
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

    // Three made-up people.
    struct Person {
        const char* first; const char* last; const char* company; const char* email; const char* birthday; const char* notes;
        const char* kind; const char* street; const char* city; const char* region; const char* postal; const char* country;
        std::vector<std::pair<const char*, const char*>> phones;
    };
    const Person people[] = {
        {"Mabel", "Thorne", "Thorne & Daughters Bindery", "mabel@thorne-bindery.example", "1961-04-12",
         "Rebinds old ledgers. Prefers a phone call to email.",
         "work", "14 Quill Lane", "Harrowgate", "TN", "38401", "USA", {{"work", "555-0142"}, {"mobile", "555-0177"}}},
        {"Rufus", "Kettle", "", "rufus.kettle@example.com", "1978-09-30", "Neighbour. Borrowed the ladder.",
         "home", "7 Orchard Row", "Millbrook", "TN", "38463", "USA", {{"home", "555-0109"}}},
        {"Imogen", "Vale", "Vale Survey Co.", "imogen@valesurvey.example", "", "Met at the county fair.",
         "work", "220 Ridge Road, Suite 4", "Port Ellis", "OR", "97031", "USA", {{"mobile", "555-0188"}}},
    };
    auto text = [](const char* s) { return *s ? Value(std::string(s)) : Value(); };
    for (const auto& p : people) {
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

}  // namespace acorn::db
