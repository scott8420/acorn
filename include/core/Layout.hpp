#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Layout -- a layout: how a table's records are shown (s010, REQUIREMENTS
// §2). Part of the SOLUTION (the .acorn), by table, each named; a database's
// own schema never holds one.
//
// Objects sit at pixel places on an 8 px grid (the editor, s010b, snaps to
// it). A FIELD object shows one field of the record -- its kind (a check box
// for Yes-No, a date box for dates ...) comes from the live table when it is
// drawn, so a field that changes kind is drawn as its new kind. A field the
// table no longer has is drawn as a dashed "missing field" box, never dropped.
// A TEXT object is words (the default layout's labels).
//
// Every table has a DEFAULT layout, "<table> Form": its fields in order, a
// label on the left, a box on the right, one per row. Until a layout is kept
// in the solution the default is computed from the table as it is now (so a
// new field shows up on it); once kept, it is what was kept.
// GTK-free, headless-testable.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::core {

inline constexpr int kLayoutGrid = 8;

// s010c: + Line (a rule: horizontal when wider than tall, else vertical,
// through the middle of its place) and Box (a rectangle) -- drawing, drawn
// behind the fields in the Form.
enum class ObjectKind { Field, Text, Line, Box };

struct LayoutObject {
    ObjectKind  kind = ObjectKind::Field;
    std::string field;   // Field: the field it shows
    std::string text;    // Text: what it says
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const LayoutObject&) const = default;
};

struct Layout {
    std::string table;                  // the table it shows
    std::string name;                   // unique within the table
    int width = 0, height = 0;          // the whole layout's size
    // s010d: parts -- the header's and the footer's heights (0 = none). The
    // body is what is between: [header, height - footer). An object belongs
    // to the part its top sits in.
    int header = 0, footer = 0;
    std::vector<LayoutObject> objects;  // drawn in order (later on top)
    bool operator==(const Layout&) const = default;
};

// The default layout's geometry (the numbers the selftest checks).
inline constexpr int kFormMargin  = 16;
inline constexpr int kFormLabelW  = 144;
inline constexpr int kFormGap     = 8;
inline constexpr int kFormBoxW    = 320;
inline constexpr int kFormRowH    = 24;
inline constexpr int kFormRowStep = 32;

std::string default_layout_name(const std::string& table);   // "contacts Form"
Layout default_layout(const std::string& table, const std::vector<std::string>& fields);
// A field object whose field `fields` doesn't have.
bool missing_field(const LayoutObject& o, const std::vector<std::string>& fields);
// "" = fine, else why in words: a name, a place off the grid's sense
// (negative, or zero / negative size), a field object with no field.
std::string check_layout(const Layout& l);

// ── s010b: the editor's geometry (GTK-free, so the selftest walks it) ───────
// What a press on an object grabbed: its middle (Move) or one of its eight
// handles -- the corners and the sides.
enum class Grab { None, Move, N, S, E, W, NE, NW, SE, SW };

// `v` to the nearest multiple of `grid` (grid <= 1: unchanged); never below 0.
int snap(int v, int grid = kLayoutGrid);

// `from` dragged by (dx, dy) through `g`, its moving edges snapped to `grid`
// (Shift: grid 1). It never leaves the top / left (x, y >= 0) and never gets
// smaller than one grid step (8 px; 1 with Shift) -- a side pushed past the
// other stops there.
LayoutObject dragged(const LayoutObject& from, Grab g, int dx, int dy = 0, int grid = kLayoutGrid);

// The body's size follows its objects: the right-most and lowest edges plus
// the margin, up to the grid (an empty layout: two margins square).
void fit(Layout& l);

// `base`, or "base 2", "base 3" ... -- the first not in `taken`.
std::string unique_layout_name(const std::string& base, const std::vector<std::string>& taken);

// "first_name" / "the words “Who”" / "a line" / "a box" -- what the editor's
// notes call an object.
std::string object_label(const LayoutObject& o);

// ── s010d: parts ────────────────────────────────────────────────────────────
enum class Part { Header, Body, Footer };
inline int footer_top(const Layout& l) { return l.height - l.footer; }
Part part_of(const Layout& l, const LayoutObject& o);
// Drag the line between two parts to `y` (snapped to the grid, clamped so no
// object is cut off and the body keeps one grid row):
//   Header -- the header/body line: everything below it moves with it (a
//             line dragged down from the top edge MAKES a header; dragged
//             back to 0, an empty header is gone);
//   Footer -- the body/footer line: the footer grows or shrinks, nothing
//             moves (dragged up from the bottom edge MAKES a footer).
// Returns false when nothing changed.
bool move_part_line(Layout& l, Part line, int y);
// The bottom edge, when there is a footer: the footer grows or shrinks
// (never past its objects). With no footer it is the footer line.
bool move_layout_bottom(Layout& l, int y);

// ── s010c: Add ──────────────────────────────────────────────────────────────
// Where the next object goes: the first free row under everything (the
// margin on an empty layout), on the grid.
int next_row_y(const Layout& l);
// `l` with one thing added under everything, last (so on top, and picked):
//   Field -- the field's words on the left and its box on the right, as the
//            default lays a row (two objects; `what` = the field);
//   Text  -- words (`what`), label-sized;  Line -- a rule across the body;
//   Box   -- a rectangle.
// The body is fitted. Returns how many objects were added.
int add_object(Layout& l, ObjectKind kind, const std::string& what = {});
// The table's fields this layout doesn't show yet, in the table's order.
std::vector<std::string> fields_not_on(const Layout& l, const std::vector<std::string>& fields);

}  // namespace acorn::core
