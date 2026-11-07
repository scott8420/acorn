#pragma once
#include <string>
#include <string_view>

namespace Gtk { class Widget; }

// ─────────────────────────────────────────────────────────────────────────────
// Registry -- the live address book (CANON: "the substrate registry is a live
// address book"). name -> widget*, populated by Named<W> at construction and
// cleared at destruction. It mirrors what exists RIGHT NOW, not what the source
// could build.
//
// The single payoff is debuggability. Three moves make the widget layer legible
// from a debugger:
//   * a backtrace frame names itself, because the widget has a name;
//   * find(name) resolves a name forward to a pointer (call it from gdb);
//   * dump() walks the live tree to stdout / the log (call it from gdb or a
//     crash handler).
//
// add() WARNS on a duplicate name rather than throwing: two live widgets can't
// share an address, and hitting it means a self-rebuild forgot to pre-clear
// (CANON s191). Cairn is a teaching seed, so it warns loudly and keeps running;
// a scripting project (where the name is an addressable key) would throw. The
// warn-vs-throw choice is itself the Pole-A-vs-Pole-B tell -- docs/widget-naming.md.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::registry {

void add(std::string_view name, Gtk::Widget* w);
void remove(Gtk::Widget* w);            // by pointer (dtor doesn't re-pass the name)
Gtk::Widget* find(std::string_view name);
void dump();                            // print the live name->widget tree
std::size_t size();

}  // namespace acorn::registry
