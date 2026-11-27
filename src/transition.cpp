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
        case TransitionStyle::Slide: return "slide";
    }
    return "crossfade";
}

bool parse_style(const std::string& name, TransitionStyle& style) {
    std::string n;
    for (char c : name) n += (char)std::tolower((unsigned char)c);
    if (n == "crossfade") { style = TransitionStyle::Crossfade; return true; }
    if (n == "dissolve") { style = TransitionStyle::Dissolve; return true; }
    if (n == "dip") { style = TransitionStyle::Dip; return true; }
    if (n == "slide") { style = TransitionStyle::Slide; return true; }
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

// A copy of a placement with the pixels of every cell for which drop(row, col) is true made transparent; the cells are those of the picture
// the placement is on, a cell being cw x ch pixels. A placement with no pixels in those cells is returned as it is (the raster is shared, not copied).
template <class Drop>
vtscreen::ImagePlacement masked(const vtscreen::ImagePlacement& p, int cw, int ch, Drop drop) {
    if (!p.raster || cw <= 0 || ch <= 0) return p;
    const vtscreen::Raster& src = *p.raster;
    const int rows = (src.h + ch - 1) / ch, cols = (src.w + cw - 1) / cw;
    std::shared_ptr<vtscreen::Raster> copy;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            if (!drop(p.row + r, p.col + c)) continue;
            const int x0 = c * cw, x1 = std::min(src.w, x0 + cw), y0 = r * ch, y1 = std::min(src.h, y0 + ch);
            bool ink = false;
            for (int y = y0; y < y1 && !ink; y++)
                for (int x = x0; x < x1 && !ink; x++) ink = src.at(x, y).a != 0;
            if (!ink) continue;
            if (!copy) copy = std::make_shared<vtscreen::Raster>(src);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) copy->at(x, y) = vtscreen::Pixel();
        }
    if (!copy) return p;
    vtscreen::ImagePlacement o = p;
    o.raster = copy;
    return o;
}

// A copy of a placement with its colours scaled by k (0 to 1); one with a k of 0 or less is left out (an empty placement), and 1 is the placement itself.
vtscreen::ImagePlacement darkened(const vtscreen::ImagePlacement& p, double k) {
    if (!p.raster || k <= 0.0) return vtscreen::ImagePlacement();
    if (k >= 1.0) return p;
    auto copy = std::make_shared<vtscreen::Raster>(*p.raster);
    for (vtscreen::Pixel& q : copy->px) {
        if (q.a == 0) continue;
        q.r = (uint8_t)std::floor(q.r * k + 0.5);
        q.g = (uint8_t)std::floor(q.g * k + 0.5);
        q.b = (uint8_t)std::floor(q.b * k + 0.5);
    }
    vtscreen::ImagePlacement o = p;
    o.raster = copy;
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

void blend(const Picture& a, const Picture& b, double pct, TransitionStyle style, Picture& out, int cell_w, int cell_h) {
    if (!(pct > 0)) pct = 0;                       // also catches NaN
    if (pct > 1) pct = 1;

    // out is a's size, not retained, every cell new
    const int orows = std::max(0, a.rows), ocols = std::max(0, a.cols);
    out.retained = false;
    out.rows = orows; out.cols = ocols;
    out.cell.assign((size_t)orows * ocols, PicCell());
    out.clear_images();                            // (out is used again and again: what it carried before is not what it carries now)
    out.fade_level = 0;
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
            if (same_size) { out.images = s.images; out.fade_level = s.fade_level; }
        } else if (style == TransitionStyle::Crossfade || style == TransitionStyle::Slide) {
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
            std::vector<unsigned char> from_b((size_t)body * cc, 0);               // for each cell: it has changed over to b's
            for (int r = 0; r < body; r++)
                for (int c = 0; c < cc; c++) {
                    // the second half of a wide letter changes over with its first half (the same threshold), so a pair is not torn apart
                    const bool tail = c > 0 && ((a.cell[(size_t)r * a.cols + c].attrs & vtscreen::CELL_TAIL) ||
                                                (b.cell[(size_t)r * b.cols + c].attrs & vtscreen::CELL_TAIL));
                    const bool pick_b = threshold(r, tail ? c - 1 : c) < pct;
                    from_b[(size_t)r * cc + c] = pick_b ? 1 : 0;
                    const Picture& s = pick_b ? b : a;
                    out.cell[(size_t)r * ocols + c] = s.cell[(size_t)r * s.cols + c];
                }
            if (same_size) {                                                       // the pixels of a cell go with its letter
                auto is_b = [&](int row, int col) { return row >= 0 && row < body && col >= 0 && col < cc && from_b[(size_t)row * cc + col] != 0; };
                if (cell_w > 0 && cell_h > 0) {
                    for (const vtscreen::ImagePlacement& p : a.images) out.images.push_back(masked(p, cell_w, cell_h, [&](int r, int c) { return is_b(r, c); }));
                    for (const vtscreen::ImagePlacement& p : b.images) out.images.push_back(masked(p, cell_w, cell_h, [&](int r, int c) { return !is_b(r, c); }));
                } else {
                    out.images = pct < 0.5 ? a.images : b.images;                  // the size of a cell is not known: the nearer screen's pictures
                }
            }
        } else {                                   // Dip: a fades to dark by the half way point, then b fades up from dark
            const bool first = pct < 0.5;
            const Picture& s = first ? a : b;
            const double k = first ? 1.0 - 2.0 * pct : 2.0 * pct - 1.0;
            for (int r = 0; r < body; r++)
                for (int c = 0; c < cc; c++)
                    out.cell[(size_t)r * ocols + c] = dimmed(s.cell[(size_t)r * s.cols + c], k);
            if (same_size)
                for (const vtscreen::ImagePlacement& p : s.images) {
                    vtscreen::ImagePlacement d = darkened(p, k);
                    if (d.raster) out.images.push_back(std::move(d));
                }
        }
    }

    // the bar's row is b's, whatever the style
    for (int c = 0; c < cc; c++)
        out.cell[(size_t)(orows - 1) * ocols + c] = b.cell[(size_t)(b.rows - 1) * b.cols + c];
}

void compose(const std::vector<Layer>& layers, const Picture& bar, Picture& out, const PicCell& fill) {
    const int orows = std::max(0, bar.rows), ocols = std::max(0, bar.cols);
    out.retained = false;
    out.rows = orows; out.cols = ocols;
    out.cell.assign((size_t)orows * ocols, fill);
    out.clear_images();
    out.fade_level = 0;
    out.touch_all();
    if (orows == 0 || ocols == 0) return;
    const int body = orows - 1;
    for (const Layer& l : layers) {
        if (l.pic == nullptr || !usable(*l.pic)) continue;
        for (const vtscreen::ImagePlacement& p : l.pic->images)     // the pixels move with the layer (the screen cuts what is outside the body)
            if (p.raster) out.place_image(p.raster, p.row + l.dy, p.col + l.dx, p.letter_rate);
        const int lrows = l.pic->rows - 1;                          // its body
        for (int r = 0; r < lrows; r++) {
            const int row = r + l.dy;
            if (row < 0 || row >= body) continue;
            // the columns of this row that land on the body
            const int from = std::max(0, -l.dx), to = std::min(l.pic->cols, ocols - l.dx);
            for (int c = from; c < to; c++)
                out.cell[(size_t)row * ocols + (size_t)(c + l.dx)] = l.pic->cell[(size_t)r * l.pic->cols + c];
        }
    }
    if (usable(bar))
        for (int c = 0; c < ocols; c++) out.cell[(size_t)body * ocols + c] = bar.cell[(size_t)body * bar.cols + c];
}

std::vector<vtscreen::ImagePlacement> images_without(const Picture& pic, int row0, int col0, int rows, int cols, int cell_w, int cell_h) {
    std::vector<vtscreen::ImagePlacement> out;
    for (const vtscreen::ImagePlacement& p : pic.images)
        out.push_back(masked(p, cell_w, cell_h, [&](int r, int c) { return r >= row0 && r < row0 + rows && c >= col0 && c < col0 + cols; }));
    return out;
}

double ease_out(double t) {
    if (!(t > 0)) return 0.0;                                       // also catches NaN
    if (t >= 1) return 1.0;
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

double ease_in(double t) {
    if (!(t > 0)) return 0.0;
    if (t >= 1) return 1.0;
    return t * t * t;
}

double slide_ms(double steps, double base_ms) {
    if (!(steps > 0) || !(base_ms > 0)) return 0.0;
    const double factor = steps <= 1.0 ? std::max(0.3, steps) : std::min(2.0, 1.0 + 0.25 * (steps - 1.0));
    return base_ms * factor;
}

}  // namespace vtmux
