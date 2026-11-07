// acorn_selftest -- the headless core harness.
//
// A seed is only trustworthy if its paradigms are EXERCISED, not just described
// (CANON: "a claim in the doc isn't a fact until a consumer exercises it"). This
// runs the GTK-free core with no display, so a future you can trust the pump and
// the mapping before wiring any widget on top:
//   * Recents  -- move-to-front + trim + the JSON round-trip with stale-prune;
//   * TextMap  -- forward(selection) -> inverse(map_range) round-trips byte-exact
//                 across single-line / multi-line / cross-paragraph / UTF-8, and
//                 chrome lines are never landed on.
//
//   cmake --build build --target acorn_selftest && ./build/acorn_selftest

#include "core/Recents.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/TextMap.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace core = acorn::core;

namespace acorn::selftest {
using CheckFn = void (*)(const std::string&, bool, const std::string&);
void db_tests(CheckFn check, int& skip_count);   // selftest_db.cpp (s002)
}

namespace {
int g_pass = 0, g_fail = 0, g_skip = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    if (ok) { ++g_pass; std::cout << "  PASS  " << name; }
    else    { ++g_fail; std::cout << "  FAIL  " << name; }
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}

// The FORWARD mapping, mirroring what a viewer's selection() would do: a
// (al,ac)-(cl,cc) mark -> V byte endpoints -> codepoint range + quote. Returns
// the byte endpoints too, so the round-trip through map_range can be exact.
struct Fwd { int cp_start, cp_end, bstart, bend; std::string quote, V; };
Fwd forward(const std::vector<core::FlatLine>& flat, int al, int ac, int cl, int cc) {
    std::vector<int> off;
    std::string V = core::visible_text(flat, off);
    int bstart = off[static_cast<std::size_t>(al)] +
                 std::min(ac, static_cast<int>(flat[static_cast<std::size_t>(al)].text.size()));
    int bend   = off[static_cast<std::size_t>(cl)] +
                 std::min(cc, static_cast<int>(flat[static_cast<std::size_t>(cl)].text.size()));
    std::string quote = V.substr(static_cast<std::size_t>(bstart),
                                 static_cast<std::size_t>(bend - bstart));
    int cps = core::utf8_length(V.substr(0, static_cast<std::size_t>(bstart)));
    int cpe = cps + core::utf8_length(quote);
    return {cps, cpe, bstart, bend, quote, V};
}

// Assert map_range inverts forward(): endpoints mapped back through byte_off
// reproduce the forward byte endpoints and reconstruct the same quote.
void roundtrip(const std::vector<core::FlatLine>& flat,
               int al, int ac, int cl, int cc, const std::string& what) {
    Fwd f = forward(flat, al, ac, cl, cc);
    std::vector<int> off;
    (void)core::visible_text(flat, off);
    core::LineCol s, e;
    const bool ok = core::map_range(flat, f.cp_start, f.cp_end, s, e);
    check("textmap: " + what + " maps", ok);
    if (!ok) return;
    const int rb0 = off[static_cast<std::size_t>(s.line)] + s.col;
    const int rb1 = off[static_cast<std::size_t>(e.line)] + e.col;
    check("textmap: " + what + " round-trips byte-exact",
          rb0 == f.bstart && rb1 == f.bend,
          std::to_string(rb0) + "," + std::to_string(rb1) +
          " vs " + std::to_string(f.bstart) + "," + std::to_string(f.bend));
    check("textmap: " + what + " reconstructs quote",
          f.V.substr(static_cast<std::size_t>(rb0),
                     static_cast<std::size_t>(rb1 - rb0)) == f.quote);
}
}  // namespace

int main() {
    std::cout << "acorn_selftest -- core paradigms + the database layer\n";

    // ── Undo -- the one door, two pictures, gestures, merging (s046) ──────────
    {
        std::cout << "\n-- undo --\n";
        using core::Stones;
        using core::Stone;
        using Clock = core::StoneJournal::Clock;
        using SnapMap = std::vector<std::pair<std::string, Stone>>;
        const auto snap = [](const Stones& s) {
            SnapMap out;
            for (const auto& id : s.ids()) out.emplace_back(id, *s.get(id));
            return out;
        };
        const auto names = [](const Stones& s) {
            std::string out;
            for (const auto& id : s.ids()) out += s.get(id)->name + ",";
            return out;
        };
        // A fixture: three stones, LOADED (not a user choice -- not undoable).
        struct Fix {
            Stones s;
            core::StoneJournal j{s};
            std::vector<std::string> sel;
            Fix() {
                s.set_door([this] { return j.open(); });
                j.set_selection_source([this] { return sel; });
                Stones::Loading load(s);
                for (const char* n : {"Ruby", "Jade", "Onyx"}) {
                    const auto id = s.fresh_id();
                    s.put(id, Stone{n, 3, s.pos_after({})});
                }
            }
        };

        // THE GATE. Every verb the page offers, from a fresh fixture: nothing
        // written with the door shut; at least one step; undoing them all puts
        // the model back exactly; redoing them all reproduces the result.
        for (const auto& v : core::stones::verbs()) {
            Fix f;
            const auto first = f.s.ids().front();
            const auto before = snap(f.s);
            v.run(f.j, f.s, first);
            const auto after = snap(f.s);
            const auto steps = f.j.size();
            check("gate/" + v.name + ": through the one door", f.s.strays() == 0,
                  std::to_string(f.s.strays()) + " stray write(s)");
            check("gate/" + v.name + ": is a step", steps > 0 && !(after == before));
            for (std::size_t i = 0; i < steps; ++i) f.j.undo();
            check("gate/" + v.name + ": undo puts it back exactly", snap(f.s) == before);
            for (std::size_t i = 0; i < steps; ++i) f.j.redo();
            check("gate/" + v.name + ": redo reproduces it", snap(f.s) == after);
        }
        {   // ...and the gate bites: a write around the door is counted.
            Fix f;
            const auto id = f.s.ids().front();
            f.s.put(id, Stone{"sneaky", 3, 0.5});
            check("gate: a write around the door is a stray", f.s.strays() == 1);
        }

        {   // Delete comes back whole, in its place, under its id, selected.
            Fix f;
            const auto ids = f.s.ids();
            f.sel = {ids[1]};
            core::stones::remove(f.j, f.s, ids[1]);
            f.sel = {ids[2]};      // the UI selects the neighbour...
            f.j.reselect(f.sel);   // ...and tells the step
            check("delete: gone", names(f.s) == "Ruby,Onyx,", names(f.s));
            check("delete: the label names it", f.j.undo_label() == "Delete “Jade”", f.j.undo_label());
            const auto r = f.j.undo();
            check("delete: undo -- back in its place", names(f.s) == "Ruby,Jade,Onyx,", names(f.s));
            check("delete: undo -- the same id", f.s.ids()[1] == ids[1]);
            check("delete: undo hands back its selection", r.selection == std::vector<std::string>{ids[1]});
            const auto r2 = f.j.redo();
            check("delete: redo -- gone again, the neighbour selected",
                  names(f.s) == "Ruby,Onyx," && r2.selection == std::vector<std::string>{ids[2]});
        }

        {   // Created records: undo removes, redo brings back the same id.
            Fix f;
            const auto first = f.s.ids()[0];
            const auto made = core::stones::add(f.j, f.s, "Opal", first);
            check("add: just after the one given", names(f.s) == "Ruby,Opal,Jade,Onyx,", names(f.s));
            f.j.undo();
            check("add: undone", !f.s.get(made));
            f.j.redo();
            check("add: redone, same id, same place", f.s.get(made) && names(f.s) == "Ruby,Opal,Jade,Onyx,");
        }

        {   // A no-op is not a step.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::rename(f.j, f.s, id, "Ruby");
            core::stones::remove(f.j, f.s, "nope");
            core::stones::move_up(f.j, f.s, id);   // already first
            check("no-op: nothing kept", f.j.size() == 0 && !f.j.can_undo());
        }

        {   // Merging: arrows on the slider fold into one step inside the window.
            Fix f;
            const auto id = f.s.ids()[0];
            const auto t0 = Clock::now();
            core::stones::resize(f.j, f.s, id, 4);
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 5; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(400));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 6; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(800));
            check("merge: three presses, one step", f.j.size() == 1, std::to_string(f.j.size()));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 7; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::seconds(10));
            check("merge: after the window, a new step", f.j.size() == 2);
            f.j.undo();
            f.j.undo();
            check("merge: two undos, back to the start", f.s.get(id)->size == 3);
            // Another stone's key never merges into this one's step.
            Fix g;
            const auto a = g.s.ids()[0], b = g.s.ids()[1];
            core::stones::resize(g.j, g.s, a, 5);
            core::stones::resize(g.j, g.s, b, 5);
            check("merge: a different key is a different step", g.j.size() == 2);
            // Nor into a step that was undone.
            core::stones::resize(g.j, g.s, b, 6);
            g.j.undo();
            core::stones::resize(g.j, g.s, b, 7);
            check("merge: never into an undone step", g.j.size() == 2 && g.j.can_undo() && !g.j.can_redo());
            // Back to where it started inside the window: the step goes away.
            Fix h;
            const auto c = h.s.ids()[0];
            core::stones::resize(h.j, h.s, c, 4);
            core::stones::resize(h.j, h.s, c, 3);
            check("merge: there and back again is no step", h.j.size() == 0);
        }

        {   // A gesture is one step, however many writes the drag made.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::drag_begin(f.j, f.s, id);
            for (int n = 4; n <= 9; ++n) core::stones::drag_to(f.j, f.s, id, n);
            check("gesture: open while dragging", f.j.in_gesture() && f.j.size() == 0);
            core::stones::drag_end(f.j);
            check("gesture: a six-write drag is one step", f.j.size() == 1 && f.s.get(id)->size == 9);
            f.j.undo();
            check("gesture: one undo, back to the start", f.s.get(id)->size == 3);
            // Escape mid-drag: back, and nothing kept.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 7);
            f.j.cancel();
            check("gesture: cancel puts it back and keeps nothing",
                  f.s.get(id)->size == 3 && !f.j.in_gesture() && f.j.can_redo());
            // Ctrl+Z mid-drag: the drag becomes a step, then is undone.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 2);
            f.j.undo();
            check("gesture: undo mid-drag closes the drag, then undoes it",
                  !f.j.in_gesture() && f.s.get(id)->size == 3 && f.j.can_redo());
            check("gesture: no stray writes", f.s.strays() == 0);
        }

        {   // A new step forgets the redo branch; Move keeps both stones in one step.
            Fix f;
            const auto ids = f.s.ids();
            core::stones::move_down(f.j, f.s, ids[0]);
            check("move: one step for both stones", f.j.size() == 1 && names(f.s) == "Jade,Ruby,Onyx,", names(f.s));
            f.j.undo();
            check("move: undone", names(f.s) == "Ruby,Jade,Onyx,");
            core::stones::rename(f.j, f.s, ids[2], "Coal");
            check("redo branch: a new step forgets it", !f.j.can_redo() && f.j.size() == 1);
        }

        {   // The cap: the oldest step falls off; the newest still undoes.
            Fix f;
            const auto id = f.s.ids()[0];
            for (std::size_t i = 0; i < core::StoneJournal::kMax + 20; ++i)
                core::stones::rename(f.j, f.s, id, "n" + std::to_string(i));
            check("cap: kMax steps kept", f.j.size() == core::StoneJournal::kMax);
            f.j.undo();
            check("cap: the newest still undoes", f.s.get(id)->name == "n" + std::to_string(core::StoneJournal::kMax + 18));
        }

        {   // on_change: the UI hears every journal change (labels, greying).
            Fix f;
            int heard = 0;
            f.j.set_on_change([&] { ++heard; });
            core::stones::rename(f.j, f.s, f.s.ids()[0], "X");
            f.j.undo();
            f.j.redo();
            f.j.clear();
            check("on_change: step, undo, redo, clear -- four", heard == 4, std::to_string(heard));
        }
    }

    std::cout << "-----------------------------------------------\n";

    // -- Recents: list ops + JSON round-trip with prune -----------------------
    {
        std::vector<std::string> l;
        core::recents_add(l, "/a");
        core::recents_add(l, "/b");
        core::recents_add(l, "/c");
        check("recents: newest first", l.size() == 3 && l.front() == "/c");
        core::recents_add(l, "/a");
        check("recents: re-add moves to front (no dup)",
              l.size() == 3 && l.front() == "/a");
        core::recents_add(l, "/x/");
        check("recents: trailing slash normalised",
              std::find(l.begin(), l.end(), "/x") != l.end() &&
              std::find(l.begin(), l.end(), "/x/") == l.end());
        std::vector<std::string> cap;
        for (int i = 0; i < 20; ++i)
            core::recents_add(cap, "/p" + std::to_string(i), 8);
        check("recents: trim to max (8)", cap.size() == 8 && cap.front() == "/p19");

        const std::string base =
            (std::filesystem::temp_directory_path() / "acorn_selftest_recents").string();
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        const std::string live = base + "/live";
        std::filesystem::create_directories(live, ec);
        const std::string file = base + "/recent.json";

        std::vector<std::string> to_save = {live, base + "/GONE"};
        check("recents: save ok", core::save_recents(file, to_save));
        auto loaded = core::load_recents(file);
        check("recents: load prunes the missing path",
              loaded.size() == 1 && loaded.front() == live);
        check("recents: missing file -> empty",
              core::load_recents(base + "/nope.json").empty());
    }

    // -- TextMap: forward/inverse round-trip ----------------------------------
    {
        // A scene as a viewer flattens it:
        //   0 "The Cellar"     title  (non-prose)
        //   1 ""               gap    (non-prose)
        //   2 "She opened the" prose, para_start   <- para 1, wrap 1
        //   3 "old iron door." prose               <- para 1, wrap 2
        //   4 "Caf\u00e9 dust." prose, para_start  <- para 2 (UTF-8: é)
        std::vector<core::FlatLine> flat = {
            {"The Cellar",     false, false},
            {"",               false, false},
            {"She opened the", true,  true },
            {"old iron door.", true,  false},
            {"Caf\xC3\xA9 dust.", true, true },
        };
        roundtrip(flat, 2, 0, 2, 14, "single line");
        roundtrip(flat, 2, 4, 3, 8,  "multi-line (soft wrap)");
        roundtrip(flat, 2, 0, 4, 10, "cross-paragraph");
        roundtrip(flat, 4, 0, 4, 6,  "UTF-8 span (accented)");

        // Chrome is never landed on: a range covering all prose starts on line 2.
        std::vector<int> off;
        (void)core::visible_text(flat, off);
        core::LineCol s, e;
        core::map_range(flat, 0, 3, s, e);
        check("textmap: start snaps to prose, never chrome", s.line == 2);

        // Degenerate / empty inputs fail cleanly.
        check("textmap: empty range rejected", !core::map_range(flat, 5, 5, s, e));
        std::vector<core::FlatLine> no_prose = {{"Title", false, false}};
        check("textmap: no prose -> false", !core::map_range(no_prose, 0, 3, s, e));
    }
    // -- Shortcuts: the registry paradigm, exercised ---------------------------
    // A seed's claim isn't a fact until a consumer exercises it. The registry's
    // whole promise is that a key and its advertised row can't drift, because
    // both read one list -- so the guard that matters is pure and lives here:
    // no two chords may collide. (App wires from this same list; ShortcutsDialog
    // renders from it.)
    {
        namespace sc = acorn::core;

        check("shortcuts: format_accel renders modifiers + key",
              sc::format_accel("<Ctrl><Shift>n") == "Ctrl+Shift+N",
              sc::format_accel("<Ctrl><Shift>n"));
        check("shortcuts: format_accel maps a named key to its glyph",
              sc::format_accel("<Ctrl>question") == "Ctrl+?",
              sc::format_accel("<Ctrl>question"));

        {
            sc::ShortcutSpec derived; derived.accels = {"<Ctrl>question", "<Ctrl>slash"};
            check("shortcuts: display_keys derives + joins accels",
                  derived.display_keys() == "Ctrl+?  /  Ctrl+/",
                  derived.display_keys());
            sc::ShortcutSpec doc; doc.keys = "Click a recent";
            check("shortcuts: display_keys prefers an explicit key string",
                  doc.display_keys() == "Click a recent");
        }

        check("shortcuts: registry is populated",
              !sc::shortcut_registry().empty(),
              std::to_string(sc::shortcut_registry().size()) + " specs");
        check("shortcuts: registry is accel-collision-free",
              sc::find_accel_collisions().empty(),
              sc::find_accel_collisions().empty()
                  ? "clean" : sc::find_accel_collisions().front());

        // Sections authored A-Z + contiguous: the dialog walks linearly and
        // starts a heading on change, so a stray out-of-order row would split a
        // section into two headings.
        {
            std::vector<std::string> secs;
            for (const auto& r : sc::shortcut_registry())
                if (secs.empty() || secs.back() != r.section) secs.push_back(r.section);
            std::vector<std::string> sorted = secs;
            std::sort(sorted.begin(), sorted.end());
            check("shortcuts: sections are contiguous and A-Z ordered", secs == sorted);
        }

        // The doc-only half of the paradigm: a gesture row documents without
        // binding (no accels), but must still show keys.
        {
            bool ok = true, saw_doc_only = false;
            for (const auto& r : sc::shortcut_registry())
                if (r.action.empty()) {
                    saw_doc_only = true;
                    if (!r.accels.empty() || r.keys.empty()) ok = false;
                }
            check("shortcuts: a doc-only row binds nothing but still displays keys",
                  ok && saw_doc_only);
        }
    }



    // ── The database layer (s002) ─────────────────────────────────────────────
    acorn::selftest::db_tests(&check, g_skip);

    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail / " << g_skip << " skipped\n";
    return g_fail == 0 ? 0 : 1;
}
