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
// s011a: + Portal -- the records of another table that point at this record
// (a contact's phones on the contact's Form): `from` is that table, `field`
// its link field (phones.contact_id -> contacts), `columns` the fields of
// `from` it lists. A portal whose link is gone is drawn dashed, never dropped.
enum class ObjectKind { Field, Text, Line, Box, Portal };

struct LayoutObject {
    ObjectKind  kind = ObjectKind::Field;
    std::string field;   // Field: the field it shows. Portal: the link field in `from`
    std::string text;    // Text: what it says
    int x = 0, y = 0, w = 0, h = 0;
    std::string from;                  // Portal: the table whose records it lists
    std::vector<std::string> columns;  // Portal: the fields of `from` it shows, in order
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

// "first_name" / "the words “Who”" / "a line" / "a box" / "the portal to
// “phones”" -- what the editor's
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

// ── s010e: several picked ───────────────────────────────────────────────────
// A pick is a list of object indexes, kept sorted, no repeats.
// Shift-click: `i` joins the pick, or leaves it if it was in.
void toggle_pick(std::vector<int>& picks, int i);
bool is_picked(const std::vector<int>& picks, int i);
// Ctrl+A: every object in the body (not the header's, not the footer's).
std::vector<int> body_objects(const Layout& l);
// The picked objects of `from` moved together by (dx, dy): the grabbed one
// (`anchor`) lands where it would alone (snapped to `grid`), the rest by the
// same amount, so the group keeps its shape; the group stops whole at the top
// / left edge. Indexes not in `from` are ignored.
Layout moved_objects(const Layout& from, const std::vector<int>& picks, int anchor, int dx, int dy, int grid = kLayoutGrid);
// The picked objects off the layout. Returns how many went.
int remove_objects(Layout& l, const std::vector<int>& picks);
// One: object_label ("“first_name”"); several: "3 objects".
std::string picks_label(const Layout& l, const std::vector<int>& picks);

// ── s011a: portals ──────────────────────────────────────────────────────────
// A link INTO the layout's table that a portal can follow: `from`.`field`
// points at the table's key (one-field links -- the ones the Graph makes).
struct PortalLink {
    std::string from, field;
    bool operator==(const PortalLink&) const = default;
};
inline constexpr int kPortalRowH = 24;   // the column names' row and each record's
inline constexpr int kPortalRows = 4;    // a new portal shows four records before it scrolls
// A portal object whose link isn't among `links` (the table or the link went).
bool missing_link(const LayoutObject& o, const std::vector<PortalLink>& links);
// The columns a new portal lists: `from`'s fields, without the link field
// (it holds this record's key -- the same on every row) and without a key
// that counts up (`counting`: Acorn's number, not the person's).
std::vector<std::string> portal_columns(const std::vector<std::string>& from_fields, const std::string& link_field,
                                        const std::string& counting = {});
// `l` with a portal added under everything, last, as wide as a default row,
// kPortalRows records tall. The body is fitted.
void add_portal(Layout& l, const PortalLink& link, const std::vector<std::string>& columns);
// s011c: choosing a portal's columns. `from`'s fields the portal doesn't list
// yet, in the table's order -- never the link field.
std::vector<std::string> columns_not_on(const LayoutObject& o, const std::vector<std::string>& from_fields);
// Column `i` moved by `delta` places (-1 left / up, +1 right / down); false
// when it can't go (the end, or no such column).
bool move_column(LayoutObject& o, int i, int delta);
// "phones · contact_id" -- how the editor names a link.
std::string link_label(const PortalLink& k);

}  // namespace acorn::core
