#pragma once
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/TextMap -- forward/inverse text mapping with a round-trip acceptance test.
// (Cairn: distilled from Folio's folioedit::TextMap. The paradigm a future you
// wants the instant it touches ANY screen<->text or image<->text coordinate
// mapping: an editor overlay, a click-to-word hit map, a re-anchored highlight.)
//
// The problem: a viewer flattens content into WRAPPED visual lines (title +
// chrome + prose). Two directions need the SAME coordinate space:
//   * forward -- a (line,col) selection becomes a codepoint range in the joined
//                VISIBLE TEXT (prose only, wraps rejoined);
//   * inverse -- a codepoint range in that same visible text becomes (line,col)
//                endpoints, so a re-anchored range can be highlighted.
//
// The lesson worth not rediscovering: factor the PURE half of both directions
// out of the widget so it's headless-testable, and make forward->inverse a
// round-trip acceptance test (see src/selftest.cpp). No GTK, no deps -- plain
// strings + primitives. That test is what lets a future you TRUST the mapping
// before building the fragile custom widget on top of it (DESIGN Option-2: prove
// the data, then build the widget).
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::core {

// A flattened visual line as a viewer holds it: wrapped text + the two flags
// that decide how it joins into the visible text.
struct FlatLine {
    std::string text;
    bool        prose      = false;  // markable content (vs title/chrome)
    bool        para_start = false;  // first wrapped line of its paragraph
};

// One endpoint in the viewer's own coordinates.
struct LineCol {
    int line = 0;   // index into `flat`
    int col  = 0;   // BYTE offset within that line's text, on a UTF-8 boundary
};

// Number of UTF-8 codepoints in s (malformed bytes count as one each).
int utf8_length(const std::string& s);

// FORWARD side: build the visible text exactly as a selection would -- join the
// PROSE lines only, "\n\n" at a paragraph break, " " at a soft-wrap join. Fills
// byte_off[i] = byte offset in V where prose line i's text begins (after any
// separator), or -1 for a non-prose line. So a prose (line,col) -> V byte offset
// is byte_off[line]+col, and map_range inverts that.
std::string visible_text(const std::vector<FlatLine>& flat,
                         std::vector<int>& byte_off);

// INVERSE side: map a codepoint range [cp_start, cp_end) in V onto (line,col)
// endpoints. START lands at the first prose line reaching cp_start (a codepoint
// in a join rolls forward to the next prose line); END closes on the last prose
// line before cp_end. Cols snap to UTF-8 boundaries. Returns false on no prose /
// degenerate / out-of-range. The exact inverse of the forward mapping.
bool map_range(const std::vector<FlatLine>& flat, int cp_start, int cp_end,
              LineCol& start_out, LineCol& end_out);

}  // namespace acorn::core
