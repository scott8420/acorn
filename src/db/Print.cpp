#include "db/Print.hpp"

namespace acorn::db {

namespace {
int col_at(const Table& t, const std::string& name) {
    for (std::size_t i = 0; i < t.columns.size(); ++i) if (t.columns[i].name == name) return static_cast<int>(i);
    return -1;
}
}  // namespace

PrintedRecord print_record(Connection& c, const Schema& s, const Table& t, const core::Layout& l, const Row& row,
                           const std::map<std::string, core::Calc>& calcs,
                           const std::map<std::string, core::CalcValue>& sums, const std::string& today) {
    PrintedRecord out(l.objects.size());
    std::map<std::string, core::CalcOutcome> worked;
    bool worked_out = false;
    for (std::size_t i = 0; i < l.objects.size(); ++i) {
        const auto& o = l.objects[i];
        auto& p = out[i];
        switch (o.kind) {
        case core::ObjectKind::Text: p.text = o.text; break;
        case core::ObjectKind::Line:
        case core::ObjectKind::Box: break;
        case core::ObjectKind::Field: {
            if (const int k = col_at(t, o.field); k >= 0 && static_cast<std::size_t>(k) < row.size()) {
                p.text = t.columns[static_cast<std::size_t>(k)].type == FieldType::Container
                             ? (row[static_cast<std::size_t>(k)].is_null() ? "" : "(binary)")
                             : cell_text(row[static_cast<std::size_t>(k)], t.columns[static_cast<std::size_t>(k)].type);
            } else if (auto it = sums.find(o.field); it != sums.end()) {
                p.text = core::calc_text(it->second);
            } else if (calcs.contains(o.field)) {
                if (!worked_out) { worked = row_calcs(t, row, calcs, today, &sums); worked_out = true; }
                p.text = core::calc_text(worked[o.field]);
            } else {
                p.missing = true;
                p.text = "missing field “" + o.field + "”";
            }
            break;
        }
        case core::ObjectKind::Related: {
            const Table* pt = s.table(o.from);
            const ForeignKey* fk = nullptr;
            for (const auto& k : t.foreign_keys)
                if (k.ref_table == o.from && k.columns.size() == 1 && k.columns.front() == o.field) fk = &k;
            const auto ref = fk && pt ? s.ref_columns(*fk) : std::vector<std::string>{};
            const int lk = col_at(t, o.field);
            const Column* sc = pt ? pt->column(o.shows) : nullptr;
            if (!fk || !pt || ref.size() != 1 || lk < 0 || !sc) { p.missing = true; p.text = "missing link"; break; }
            const Value& link = row[static_cast<std::size_t>(lk)];
            if (link.is_null()) break;
            auto page = read_related(c, *pt, {ref.front()}, {link}, {0, 1});
            if (!page || page->rows.empty()) break;
            if (const int sk = col_at(*pt, o.shows); sk >= 0) p.text = cell_text(page->rows.front()[static_cast<std::size_t>(sk)], sc->type);
            break;
        }
        case core::ObjectKind::Portal: {
            const Table* ft = s.table(o.from);
            const ForeignKey* fk = nullptr;
            if (ft)
                for (const auto& k : ft->foreign_keys)
                    if (k.ref_table == t.name && k.columns.size() == 1 && k.columns.front() == o.field) fk = &k;
            const auto ref = fk ? s.ref_columns(*fk) : std::vector<std::string>{};
            const int rk = ref.size() == 1 ? col_at(t, ref.front()) : -1;
            if (!ft || !fk || rk < 0) { p.missing = true; p.text = "missing link"; break; }
            const int fits = std::max(0, (o.h - core::kPortalRowH) / core::kPortalRowH);
            if (fits == 0 || row[static_cast<std::size_t>(rk)].is_null()) break;
            auto page = read_related(c, *ft, {o.field}, {row[static_cast<std::size_t>(rk)]}, {0, fits});
            if (!page) break;
            for (const auto& r : page->rows) {
                std::vector<std::string> cells;
                for (const auto& cn : o.columns) {
                    const int k = col_at(*ft, cn);
                    cells.push_back(k < 0 ? std::string{} : cell_text(r[static_cast<std::size_t>(k)], ft->columns[static_cast<std::size_t>(k)].type));
                }
                p.rows.push_back(std::move(cells));
            }
            break;
        }
        }
    }
    return out;
}

}  // namespace acorn::db
