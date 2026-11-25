#include "transition.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace vtmux {

const char* style_name(TransitionStyle style) {
    switch (style) {
        case TransitionStyle::Crossfade: return "crossfade";
        case TransitionStyle::Dissolve: return "dissolve";
        case TransitionStyle::Dip: return "dip";
    }
    return "crossfade";
}

bool parse_style(const std::string& name, TransitionStyle& style) {
    std::string n;
    for (char c : name) n += (char)std::tolower((unsigned char)c);
    if (n == "crossfade") { style = TransitionStyle::Crossfade; return true; }
    if (n == "dissolve") { style = TransitionStyle::Dissolve; return true; }
    if (n == "dip") { style = TransitionStyle::Dip; return true; }
    return false;
}

namespace {

using vtscreen::Picture;
using vtscreen::PicCell;

// A fixed threshold for a cell, 0 up to (not including) 1: a hash of its row and column that scatters well (the
// finalizer of a 32 bit murmur hash over two multiplied coordinates), so no diagonal or tiled pattern shows.
double threshold(int row, int col) {
    uint32_t h = (uint32_t)row * 0x9E3779B1u ^ ((uint32_t)col + 0x7F4A7C15u) * 0x85EBCA6Bu;
    h ^= h >> 16; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (double)h / 4294967296.0;
}

// the colours of a cell scaled by k (0 to 1); the letter, the inverse flag and whether there is a background stay.
PicCell dimmed(const PicCell& p, double k) {
    PicCell o = p;
    o.r *= k; o.g *= k; o.b *= k;
    o.alt_r *= k; o.alt_g *= k; o.alt_b *= k;
    if (!p.text) o.lum *= k;                       // a picture cell's letter comes from its brightness
    return o;
}

// A picture that is the top left rows x cols of p.
Picture crop(const Picture& p, int rows, int cols) {
    Picture o;
    o.rows = rows; o.cols = cols;
    o.cell.resize((size_t)rows * cols);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) o.cell[(size_t)r * cols + c] = p.cell[(size_t)r * p.cols + c];
    return o;
}

bool usable(const Picture& p) {
    return p.rows > 0 && p.cols > 0 && p.cell.size() >= (size_t)p.rows * (size_t)p.cols;
}

}  // namespace

void blend(const Picture& a, const Picture& b, double pct, TransitionStyle style, Picture& out) {
    if (!(pct > 0)) pct = 0;                       // also catches NaN
    if (pct > 1) pct = 1;

    // out is a's size, not retained, every cell new
    const int orows = std::max(0, a.rows), ocols = std::max(0, a.cols);
    out.retained = false;
    out.rows = orows; out.cols = ocols;
    out.cell.assign((size_t)orows * ocols, PicCell());
    out.touch_all();
    if (!usable(a) || !usable(b)) return;

    // what both pictures have: the body is every row but the bar's, over the columns they share
    const int cc = std::min(a.cols, b.cols);
    const int body = std::min(a.rows, b.rows) - 1;
    const bool same_size = a.rows == b.rows && a.cols == b.cols;

    if (body > 0 && cc > 0) {
        if (pct <= 0 || pct >= 1) {                // the ends are the pictures themselves, whatever the style
            const Picture& s = pct <= 0 ? a : b;
            for (int r = 0; r < body; r++)
                for (int c = 0; c < cc; c++) out.cell[(size_t)r * ocols + c] = s.cell[(size_t)r * s.cols + c];
        } else if (style == TransitionStyle::Crossfade) {
            if (same_size) {
                vtscreen::fade(a, b, pct, out);
                out.retained = false;
            } else {
                const Picture ca = crop(a, body, cc), cb = crop(b, body, cc);
                Picture mixed;
                vtscreen::fade(ca, cb, pct, mixed);
                for (int r = 0; r < body; r++)
                    for (int c = 0; c < cc; c++) out.cell[(size_t)r * ocols + c] = mixed.cell[(size_t)r * cc + c];
            }
        } else if (style == TransitionStyle::Dissolve) {
            for (int r = 0; r < body; r++)
                for (int c = 0; c < cc; c++) {
                    const Picture& s = threshold(r, c) < pct ? b : a;
                    out.cell[(size_t)r * ocols + c] = s.cell[(size_t)r * s.cols + c];
                }
        } else {                                   // Dip: a fades to dark by the half way point, then b fades up from dark
            const bool first = pct < 0.5;
            const Picture& s = first ? a : b;
            const double k = first ? 1.0 - 2.0 * pct : 2.0 * pct - 1.0;
            for (int r = 0; r < body; r++)
                for (int c = 0; c < cc; c++)
                    out.cell[(size_t)r * ocols + c] = dimmed(s.cell[(size_t)r * s.cols + c], k);
        }
    }

    // the bar's row is b's, whatever the style
    for (int c = 0; c < cc; c++)
        out.cell[(size_t)(orows - 1) * ocols + c] = b.cell[(size_t)(b.rows - 1) * b.cols + c];
}

}  // namespace vtmux
