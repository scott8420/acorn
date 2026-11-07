#include "core/TextMap.hpp"

#include <algorithm>
#include <cstddef>

namespace acorn::core {

namespace {
// Byte length of the UTF-8 codepoint led by byte `b`. Malformed -> 1.
int cp_len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b >> 5) == 0x6)  return 2;
    if ((b >> 4) == 0xE)  return 3;
    if ((b >> 3) == 0x1E) return 4;
    return 1;
}

// Byte offset in s of the cp-th codepoint boundary (clamped to s.size()).
int byte_of_cp(const std::string& s, int cp) {
    int i = 0, c = 0;
    const int n = static_cast<int>(s.size());
    while (i < n && c < cp) {
        i = std::min(n, i + cp_len(static_cast<unsigned char>(s[static_cast<std::size_t>(i)])));
        ++c;
    }
    return i;
}

// Pull a byte index back to the nearest UTF-8 boundary at or below it.
int snap(const std::string& s, int i) {
    i = std::max(0, std::min(i, static_cast<int>(s.size())));
    while (i > 0 && i < static_cast<int>(s.size()) &&
           (static_cast<unsigned char>(s[static_cast<std::size_t>(i)]) & 0xC0) == 0x80)
        --i;
    return i;
}
}  // namespace

int utf8_length(const std::string& s) {
    int c = 0;
    for (int i = 0, n = static_cast<int>(s.size()); i < n; ) {
        i += cp_len(static_cast<unsigned char>(s[static_cast<std::size_t>(i)]));
        ++c;
    }
    return c;
}

std::string visible_text(const std::vector<FlatLine>& flat,
                         std::vector<int>& byte_off) {
    byte_off.assign(flat.size(), -1);
    std::string V;
    bool first = true;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        if (!flat[i].prose) continue;
        if (!first) V += (flat[i].para_start ? "\n\n" : " ");
        byte_off[i] = static_cast<int>(V.size());
        V += flat[i].text;
        first = false;
    }
    return V;
}

bool map_range(const std::vector<FlatLine>& flat, int cp_start, int cp_end,
              LineCol& start_out, LineCol& end_out) {
    if (cp_end <= cp_start) return false;

    std::vector<int> byte_off;
    const std::string V = visible_text(flat, byte_off);
    if (V.empty()) return false;   // no prose to land on

    const int vlen = static_cast<int>(V.size());
    const int b0 = std::min(byte_of_cp(V, cp_start), vlen);
    const int b1 = std::min(byte_of_cp(V, cp_end),   vlen);
    if (b1 <= b0) return false;

    const int n = static_cast<int>(flat.size());

    // START: first prose line whose text extends past b0; a b0 in a separator
    // gap rolls forward to the next prose line's start.
    LineCol s;
    bool s_ok = false;
    for (int i = 0; i < n; ++i) {
        if (byte_off[static_cast<std::size_t>(i)] < 0) continue;
        const int off = byte_off[static_cast<std::size_t>(i)];
        const int len = static_cast<int>(flat[static_cast<std::size_t>(i)].text.size());
        if (off + len > b0) {
            s.line = i;
            s.col  = snap(flat[static_cast<std::size_t>(i)].text, std::max(0, b0 - off));
            s_ok = true;
            break;
        }
    }
    if (!s_ok) return false;

    // END: last prose line beginning before b1; close there, clamped to length.
    LineCol e;
    bool e_ok = false;
    for (int i = 0; i < n; ++i) {
        if (byte_off[static_cast<std::size_t>(i)] < 0) continue;
        const int off = byte_off[static_cast<std::size_t>(i)];
        if (off < b1) {
            const int len = static_cast<int>(flat[static_cast<std::size_t>(i)].text.size());
            e.line = i;
            e.col  = snap(flat[static_cast<std::size_t>(i)].text, std::min(b1 - off, len));
            e_ok = true;   // keep going -- we want the LAST such line
        }
    }
    if (!e_ok) return false;

    start_out = s;
    end_out   = e;
    return true;
}

}  // namespace acorn::core
