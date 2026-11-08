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
// s011d: + Related -- the other direction: a field of the record this one
// points at (an address showing its contact's last_name): `field` is this
// table's link field (addresses.contact_id), `from` the table it points at
// (contacts), `shows` that table's field (last_name). Shown, not edited.
// s017b: + Button -- words (`text`) that run one of the table's scripts
// (`script`, its name; "" runs nothing) when clicked in the Form or a List
// row (on that row's record). Dim in Find; never printed. A button whose
// script is gone is drawn dashed in Layouts and greyed in Browse, never
// dropped.
enum class ObjectKind { Field, Text, Line, Box, Portal, Related, Button };

// s012b: how words sit in their place. Right is the default -- toward the
// box beside them, as the default layout's labels; in the .acorn only when
// not Right ("align": "left" / "center"), so older files read the same.
enum class Align { Right, Left, Center };

struct LayoutObject {
    ObjectKind  kind = ObjectKind::Field;
    std::string field;   // Field: the field it shows. Portal: the link field in `from`
    std::string text;    // Text: what it says
    int x = 0, y = 0, w = 0, h = 0;
    std::string from;                  // Portal: the table whose records it lists
    std::vector<std::string> columns;  // Portal: the fields of `from` it shows, in order
    std::string shows;                 // Related: the field of `from` it shows
    Align align = Align::Right;        // s012b: Text -- how the words sit
    std::string list;                  // s014c: Field -- the table's value list its box offers ("" none)
    std::string script;                // s017b: Button -- the table's script it runs ("" none)
    bool operator==(const LayoutObject&) const = default;
};

// s015b: a SUB-SUMMARY part -- FileMaker's "sub-summary when sorted by
// <field>". Preview sorts the records by `by` and prints the part once per
// group: LEADING above the group's records (with the group's first record's
// values), TRAILING below them (its last record's). A summary field in it is
// worked out over that group. Browse (Form / List / Find) never shows it.
struct SubSummary {
    std::string by;        // the field the records are grouped by
    bool leading = true;   // above its group (else below)
    int height = 0;
    bool operator==(const SubSummary&) const = default;
};

struct Layout {
    std::string table;                  // the table it shows
    std::string name;                   // unique within the table
    int width = 0, height = 0;          // the whole layout's size
    // s010d: parts -- the header's and the footer's heights (0 = none). The
    // body is what is between: [header, height - footer). An object belongs
    // to the part its top sits in.
    int header = 0, footer = 0;
    // s015b: the sub-summaries. Top-down the layout is: the header, the
    // LEADING subs (in this order), the body, the TRAILING subs (in this
    // order), the footer -- so the body is what the rest leave.
    std::vector<SubSummary> subs;
    // s016: LABELS -- Preview / Print lay the body `across` to a row (1: a
    // list, one per row), `gap` px between labels both ways. The label's
    // size is the layout's width by the body's height.
    int across = 1, gap = 0;
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
// s015b: + Sub -- an object in a sub-summary (sub_of says which).
enum class Part { Header, Body, Footer, Sub };
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

// ── s011d: related fields ───────────────────────────────────────────────────
// A link OUT of the layout's table: its `field` points at `to`'s key
// (addresses.contact_id -> contacts). One-field links, as the Graph makes.
struct RelatedLink {
    std::string field, to;
    bool operator==(const RelatedLink&) const = default;
};
// A related object whose link isn't among `links` (the field, the table or
// the link went).
bool missing_related_link(const LayoutObject& o, const std::vector<RelatedLink>& links);
// "contact_id → last_name" -- how the editor offers one.
std::string related_label(const std::string& link_field, const std::string& shows);
// `to`'s fields a related object on `l` doesn't show yet through `link`, in
// `to`'s order -- never `to`'s key (it is the link's own value).
std::vector<std::string> related_not_on(const Layout& l, const RelatedLink& link,
                                        const std::vector<std::string>& to_fields, const std::string& to_key = {});
// `l` with a row added under everything: the shown field's name as words on
// the left, the related value on the right (as Add Field lays a row). The
// body is fitted. Returns 2 (the words and the value).
int add_related(Layout& l, const RelatedLink& link, const std::string& shows);

// ── s012b: words alignment ──────────────────────────────────────────────────
// "left" / "center" / "right" -- the .acorn's words; parse: anything else is Right.
const char* align_name(Align a);
Align parse_align(const std::string& s);
// How the inspector and the notes say it: "left" / "centred" / "right".
const char* align_label(Align a);

// ── s012a: the list view ────────────────────────────────────────────────────
// Browse -> List draws a layout as rows: the header once at the top, the body
// once per record, the footer once at the bottom. Where a part starts on the
// layout (Header 0, Body the header's height, Footer footer_top) and how tall
// it is (0 for a part the layout hasn't).
int part_top(const Layout& l, Part p);
int part_height(const Layout& l, Part p);
// The objects of one part, in drawing order: lines and boxes first (behind),
// then the rest -- each group in the layout's own order.
std::vector<int> part_objects(const Layout& l, Part p);

// ── s015b: sub-summary parts ────────────────────────────────────────────────
inline constexpr int kSubH = 40;   // a new sub-summary's height
// One band of the layout, top-down: the header (when it has one), the
// leading subs, the body (always), the trailing subs, the footer (when it
// has one). `sub` is the index into Layout::subs (-1 for the rest).
struct Band {
    Part part = Part::Body;
    int sub = -1;
    int top = 0, height = 0;
    bool operator==(const Band&) const = default;
};
std::vector<Band> bands(const Layout& l);
int body_top(const Layout& l);      // under the header and the leading subs
int body_bottom(const Layout& l);   // over the trailing subs and the footer
int sub_top(const Layout& l, int i);
// The sub-summary an object sits in (its top), or -1.
int sub_of(const Layout& l, const LayoutObject& o);
// A band's objects in drawing order (lines and boxes first), as part_objects.
std::vector<int> band_objects(const Layout& l, const Band& b);
// The fields a report groups by, outermost first: the leading subs' fields
// top-down, then the trailing subs' bottom-up (the last trailing is the
// outermost), each once.
std::vector<std::string> group_fields(const Layout& l);
// Sub `i`'s place in group_fields (its group is the records alike in that
// field and every one before it); -1 if there's no such sub.
int group_level(const Layout& l, int i);
// "leading sub-summary by “city”"
std::string sub_label(const SubSummary& s);
// A sub-summary by `by` added kSubH tall: a leading one just above the body
// (under the leading ones there are), a trailing one just under it (over the
// trailing ones there are) -- everything under it moves down. It gets `by`'s
// box, so the part shows which group it is. Returns its index.
int add_sub(Layout& l, const std::string& by, bool leading);
// Sub `i` gone, with its objects; everything under it moves up. False when
// there's no such sub.
bool remove_sub(Layout& l, int i);
// Drag the bottom line of band `b` (an index into bands(l): the body or a
// sub) to `y`: that band grows or shrinks and everything under it moves with
// the line (FileMaker's part lines). Snapped; never cuts the band's objects;
// a band keeps one grid row. False when nothing changed.
bool move_band_line(Layout& l, int b, int y);

// What a report prints, in order: per record its body, with each sub-summary
// where its group starts (leading) or ends (trailing). `keys[r]` are record
// r's values of group_fields(l), in that order, as text; a group is a run of
// records alike in its field and every field before it. A sub prints the
// values of `record` -- the group's first (leading) or last (trailing).
struct ReportBlock {
    Part part = Part::Body;   // Body or Sub
    int sub = -1;
    int record = 0;
    bool operator==(const ReportBlock&) const = default;
};
std::vector<ReportBlock> report_blocks(const Layout& l, const std::vector<std::vector<std::string>>& keys);

// ── s017b: buttons ──────────────────────────────────────────────────────────
inline constexpr int kButtonW = 128;   // a new button's size
inline constexpr int kButtonH = 32;
// A button whose script isn't among `scripts` (the table's script names).
// A button that runs nothing ("") is not missing.
bool missing_script(const LayoutObject& o, const std::vector<std::string>& scripts);
// `l` with a button added under everything, last: `words`, running `script`.
// The body is fitted.
void add_button(Layout& l, const std::string& words, const std::string& script);
// Every button of `l` that runs `from` made to run `to` (a script renamed).
// Returns how many changed.
int rename_script_on(Layout& l, const std::string& from, const std::string& to);
// "runs “Tidy”" / "runs no script" -- how the inspector and tooltips say it.
std::string button_says(const LayoutObject& o);

// ── s016: labels ────────────────────────────────────────────────────────────
inline constexpr int kLabelsMax = 8;   // the most across
inline bool is_labels(const Layout& l) { return l.across > 1; }
// How wide a row of labels prints: across labels and the gaps between.
int labels_width(const Layout& l);
// What prints for `records` records as labels: one Body block per ROW, its
// `record` the row's first (the row holds up to `across` from there).
std::vector<ReportBlock> label_blocks(const Layout& l, int records);
// "3 across, 16 px apart" / "one per row"
std::string labels_label(const Layout& l);

}  // namespace acorn::core
