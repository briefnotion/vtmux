#include "width.h"

#include <cstddef>

namespace vtmux {

#include "width_table.inc"

namespace {

template <size_t N>
bool in_table(const char32_t (&table)[N][2], char32_t cp) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < table[mid][0]) hi = mid;
        else if (cp > table[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

}  // namespace

int char_width(char32_t cp) {
    if (cp < 0x300) return cp < 0x20 || (cp >= 0x7F && cp < 0xA0) ? 0 : 1;      // the common case: controls take no cell
    if (in_table(ZERO_WIDTH, cp)) return 0;
    if (in_table(WIDE, cp)) return 2;
    return 1;
}

}  // namespace vtmux
