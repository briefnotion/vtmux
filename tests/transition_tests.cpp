// Tests for blend: the picture part of the way from one screen to another (crossfade, dissolve, dip), and the style names.
// sources: ../src/transition.cpp
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "../src/transition.h"

using namespace vtmux;
using vtscreen::PicCell;
using vtscreen::Picture;

namespace {

const TransitionStyle ALL_STYLES[3] = {TransitionStyle::Crossfade, TransitionStyle::Dissolve, TransitionStyle::Dip};

// A picture of body rows (plus the bar row) of text cells. Every cell differs from the same cell of a picture made with
// another letter base and seed.
Picture make(int body, int cols, unsigned seed, char32_t base, bool text = true) {
    std::mt19937 rng(seed);
    Picture p;
    p.init(body, cols);
    for (int r = 0; r < p.rows; r++)
        for (int c = 0; c < cols; c++) {
            PicCell k;
            k.r = 20 + (int)(rng() % 200); k.g = 20 + (int)(rng() % 200); k.b = 20 + (int)(rng() % 200);
            if (text) {
                k.text = true;
                k.ch = base + (char32_t)((r * 7 + c) % 20);
                k.inv = (rng() % 5) == 0;
                if (rng() % 3 == 0) {
                    k.alt_lum = 1;
                    k.alt_r = (double)(rng() % 256); k.alt_g = (double)(rng() % 256); k.alt_b = (double)(rng() % 256);
                }
            } else {
                k.lum = (double)(rng() % 1000) / 1000.0;
                k.shimmer = 1 + (double)(rng() % 3);
                k.alt_lum = (double)(rng() % 100) / 100.0;
                k.alt_r = (double)(rng() % 256); k.alt_g = (double)(rng() % 256); k.alt_b = (double)(rng() % 256);
            }
            p.cell[(size_t)r * cols + c] = k;
        }
    return p;
}

bool rows_equal(const Picture& x, const Picture& y, int row0, int row1) {
    if (x.cols != y.cols) return false;
    for (int r = row0; r < row1; r++)
        for (int c = 0; c < x.cols; c++)
            if (x.cell[(size_t)r * x.cols + c] != y.cell[(size_t)r * y.cols + c]) return false;
    return true;
}

// the sum of every colour value in the picture's body (not the bar)
double ink(const Picture& p) {
    double s = 0;
    for (int r = 0; r < p.rows - 1; r++)
        for (int c = 0; c < p.cols; c++) {
            const PicCell& k = p.cell[(size_t)r * p.cols + c];
            s += k.r + k.g + k.b + k.alt_r + k.alt_g + k.alt_b;
        }
    return s;
}

void test_names() {
    CHECK_EQ(std::string(style_name(TransitionStyle::Crossfade)), std::string("crossfade"));
    CHECK_EQ(std::string(style_name(TransitionStyle::Dissolve)), std::string("dissolve"));
    CHECK_EQ(std::string(style_name(TransitionStyle::Dip)), std::string("dip"));
    for (TransitionStyle s : ALL_STYLES) {
        TransitionStyle got = TransitionStyle::Dip == s ? TransitionStyle::Crossfade : TransitionStyle::Dip;
        CHECK(parse_style(style_name(s), got));
        CHECK(got == s);
    }
    CHECK_EQ(std::string(style_name(TransitionStyle::Slide)), std::string("slide"));
    TransitionStyle s = TransitionStyle::Dissolve;
    CHECK(parse_style("Slide", s));
    CHECK(s == TransitionStyle::Slide);
    CHECK(parse_style("Dip", s));
    CHECK(s == TransitionStyle::Dip);
    CHECK(parse_style("CROSSFADE", s));
    CHECK(s == TransitionStyle::Crossfade);
    s = TransitionStyle::Dissolve;
    CHECK(!parse_style("fade", s));
    CHECK(!parse_style("", s));
    CHECK(!parse_style("dip ", s));
    CHECK(s == TransitionStyle::Dissolve);             // untouched when it fails
}

void test_ends_exact() {
    for (bool text : {true, false}) {
        const Picture a = make(8, 20, 1, U'a', text), b = make(8, 20, 2, U'A', text);
        for (TransitionStyle st : ALL_STYLES) {
            Picture out;
            blend(a, b, 0.0, st, out);
            CHECK_EQ(out.rows, a.rows);
            CHECK_EQ(out.cols, a.cols);
            CHECK(rows_equal(out, a, 0, a.rows - 1));      // a's body
            CHECK(rows_equal(out, b, a.rows - 1, a.rows)); // b's bar
            blend(a, b, 1.0, st, out);
            CHECK(rows_equal(out, b, 0, b.rows));          // b entirely
            // clamped
            blend(a, b, -3.0, st, out);
            CHECK(rows_equal(out, a, 0, a.rows - 1));
            blend(a, b, 7.5, st, out);
            CHECK(rows_equal(out, b, 0, b.rows));
            blend(a, b, std::numeric_limits<double>::quiet_NaN(), st, out);
            CHECK(rows_equal(out, a, 0, a.rows - 1));
        }
    }
}

void test_out_is_assigned_and_not_retained() {
    const Picture a = make(5, 10, 3, U'a'), b = make(5, 10, 4, U'A');
    Picture out = make(2, 3, 9, U'q');                 // a different size, with other content
    out.retain();
    for (TransitionStyle st : ALL_STYLES) {
        blend(a, b, 0.4, st, out);
        CHECK(!out.retained);
        CHECK_EQ(out.rows, 6);
        CHECK_EQ(out.cols, 10);
        CHECK_EQ(out.cell.size(), (size_t)60);
        CHECK(out.all_touched);
    }
    // blending twice into the same out gives the same result as into a new one (nothing is left over)
    for (TransitionStyle st : ALL_STYLES) {
        Picture fresh;
        blend(a, b, 0.4, st, fresh);
        blend(a, b, 0.4, st, out);
        CHECK(rows_equal(out, fresh, 0, 6));
    }
}

void test_bar_from_b() {
    const Picture a = make(6, 12, 5, U'a'), b = make(6, 12, 6, U'A');
    for (TransitionStyle st : ALL_STYLES)
        for (double pct : {0.1, 0.3, 0.5, 0.7, 0.9}) {
            Picture out;
            blend(a, b, pct, st, out);
            CHECK(rows_equal(out, b, 6, 7));
        }
}

void test_crossfade() {
    // same letters, different colours: the colours go from a's to b's, steadily
    Picture a = make(4, 10, 1, U'a'), b = a;
    for (PicCell& k : b.cell) { k.r = 255 - k.r; k.g = 255 - k.g; k.b = 255 - k.b; }
    std::vector<double> prev;
    for (int i = 0; i <= 10; i++) {
        const double pct = i / 10.0;
        Picture out;
        blend(a, b, pct, TransitionStyle::Crossfade, out);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 10; c++) {
                const PicCell& o = out.cell[(size_t)r * 10 + c];
                const PicCell& p = a.cell[(size_t)r * 10 + c];
                const PicCell& q = b.cell[(size_t)r * 10 + c];
                CHECK(std::fabs(o.r - (p.r * (1 - pct) + q.r * pct)) < 1e-9);
                CHECK(o.ch == p.ch);
                if (q.r > p.r) CHECK(o.r >= p.r - 1e-9 && o.r <= q.r + 1e-9);
                else CHECK(o.r <= p.r + 1e-9 && o.r >= q.r - 1e-9);
            }
    }
    // it is vtscreen::fade (the letters change over at the half way point)
    const Picture x = make(4, 10, 1, U'a'), y = make(4, 10, 2, U'A');
    for (double pct : {0.2, 0.49, 0.5, 0.8}) {
        Picture out, ref;
        blend(x, y, pct, TransitionStyle::Crossfade, out);
        vtscreen::fade(x, y, pct, ref);
        CHECK(rows_equal(out, ref, 0, 4));
    }
    Picture lo, hi;
    blend(x, y, 0.49, TransitionStyle::Crossfade, lo);
    blend(x, y, 0.5, TransitionStyle::Crossfade, hi);
    CHECK(lo.cell[0].ch == x.cell[0].ch);
    CHECK(hi.cell[0].ch == y.cell[0].ch);
}

void test_crossfade_mixed_cells() {
    // a text cell and a picture cell cannot be mixed: the one that is ahead is shown
    const Picture a = make(3, 6, 1, U'a', true), b = make(3, 6, 2, U'A', false);
    Picture out;
    blend(a, b, 0.3, TransitionStyle::Crossfade, out);
    CHECK(out.cell[0] == a.cell[0]);
    blend(a, b, 0.7, TransitionStyle::Crossfade, out);
    CHECK(out.cell[0] == b.cell[0]);
    // two picture cells are mixed
    const Picture c = make(3, 6, 3, U'a', false), d = make(3, 6, 4, U'a', false);
    blend(c, d, 0.25, TransitionStyle::Crossfade, out);
    CHECK(!out.cell[0].text);
    CHECK(std::fabs(out.cell[0].lum - (c.cell[0].lum * 0.75 + d.cell[0].lum * 0.25)) < 1e-9);
}

void test_dip() {
    const Picture a = make(6, 16, 1, U'a'), b = make(6, 16, 2, U'A');
    Picture out;
    // before the middle: a's letters, getting darker; after it, b's letters, getting lighter
    double prev = 1e18;
    for (int i = 0; i < 10; i++) {
        const double pct = i / 20.0;                   // 0 to just under 0.5
        blend(a, b, pct, TransitionStyle::Dip, out);
        const double s = ink(out);
        CHECK(s <= prev + 1e-9);
        prev = s;
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 16; c++) {
                const PicCell& o = out.cell[(size_t)r * 16 + c];
                const PicCell& p = a.cell[(size_t)r * 16 + c];
                CHECK(o.ch == p.ch);                   // the letters are kept
                CHECK(o.inv == p.inv);
                CHECK(o.text);
                CHECK((o.alt_lum > 0) == (p.alt_lum > 0));
                CHECK(std::fabs(o.r - p.r * (1 - 2 * pct)) < 1e-9);
                CHECK(std::fabs(o.alt_g - p.alt_g * (1 - 2 * pct)) < 1e-9);
            }
    }
    CHECK(prev > 0.0);
    blend(a, b, 0.5, TransitionStyle::Dip, out);
    CHECK_EQ(ink(out), 0.0);
    prev = -1;
    for (int i = 10; i <= 20; i++) {
        const double pct = i / 20.0;
        blend(a, b, pct, TransitionStyle::Dip, out);
        const double s = ink(out);
        CHECK(s >= prev - 1e-9);
        prev = s;
        const PicCell& o = out.cell[5];
        CHECK(o.ch == b.cell[5].ch);
        CHECK(std::fabs(o.g - b.cell[5].g * (2 * pct - 1)) < 1e-9);
    }
    CHECK(std::fabs(prev - ink(b)) < 1e-6);
    // 0.5 is the minimum over the whole range
    double least = 1e18, at_least = -1;
    for (int i = 0; i <= 100; i++) {
        blend(a, b, i / 100.0, TransitionStyle::Dip, out);
        const double s = ink(out);
        if (s < least) { least = s; at_least = i / 100.0; }
    }
    CHECK_EQ(at_least, 0.5);
    // picture cells: their brightness goes down too
    const Picture pa = make(3, 6, 3, U'a', false), pb = make(3, 6, 4, U'a', false);
    blend(pa, pb, 0.25, TransitionStyle::Dip, out);
    CHECK(std::fabs(out.cell[2].lum - pa.cell[2].lum * 0.5) < 1e-9);
    CHECK(!out.cell[2].text);
    CHECK_EQ(pa.cell[2].shimmer, out.cell[2].shimmer);
}

void test_dissolve() {
    const int R = 100, C = 200;
    const Picture a = make(R, C, 1, U'a'), b = make(R, C, 2, U'A');
    Picture out, prev_out;
    std::vector<unsigned char> was_b((size_t)R * C, 0);
    double last_frac = 0;
    for (int i = 0; i <= 10; i++) {
        const double pct = i / 10.0;
        blend(a, b, pct, TransitionStyle::Dissolve, out);
        long from_b = 0;
        for (int r = 0; r < R; r++)
            for (int c = 0; c < C; c++) {
                const size_t k = (size_t)r * C + c;
                const bool isa = out.cell[k] == a.cell[k], isb = out.cell[k] == b.cell[k];
                CHECK(isa != isb);                     // each cell is wholly one or the other
                if (isb) from_b++;
                if (was_b[k]) CHECK(isb);              // a cell that has changed over stays changed
                was_b[k] = isb ? 1 : 0;
            }
        const double frac = (double)from_b / (R * C);
        CHECK(std::fabs(frac - pct) < 0.02);           // about pct of the cells have changed
        CHECK(frac >= last_frac);
        last_frac = frac;
    }

    // no pattern: at one half, every sort of line and block through the picture has about half
    blend(a, b, 0.5, TransitionStyle::Dissolve, out);
    auto from_b = [&](int r, int c) { return out.cell[(size_t)r * C + c] == b.cell[(size_t)r * C + c] ? 1 : 0; };
    for (int mod : {2, 3, 4, 5, 7, 8, 16}) {
        std::vector<double> diag(mod, 0), anti(mod, 0), rowm(mod, 0), colm(mod, 0), cnt(mod, 0);
        for (int r = 0; r < R; r++)
            for (int c = 0; c < C; c++) {
                const int v = from_b(r, c);
                diag[(size_t)((r + c) % mod)] += v;
                anti[(size_t)(((r - c) % mod + mod) % mod)] += v;
                rowm[(size_t)(r % mod)] += v;
                colm[(size_t)(c % mod)] += v;
            }
        const double per = (double)R * C / mod;
        for (int k = 0; k < mod; k++) {
            CHECK(std::fabs(diag[(size_t)k] / per - 0.5) < 0.05);
            CHECK(std::fabs(anti[(size_t)k] / per - 0.5) < 0.05);
            CHECK(std::fabs(rowm[(size_t)k] / (per) - 0.5) < 0.05 * 4);   // rows and columns have fewer cells: looser
            CHECK(std::fabs(colm[(size_t)k] / (per) - 0.5) < 0.05 * 2);
        }
    }
    // blocks of 4 x 4: none is all one thing, and they are spread
    long full = 0, empty = 0, blocks = 0;
    for (int br = 0; br + 4 <= R; br += 4)
        for (int bc = 0; bc + 4 <= C; bc += 4) {
            int n = 0;
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) n += from_b(br + r, bc + c);
            full += n == 16; empty += n == 0; blocks++;
        }
    CHECK(full < blocks / 100 * 2 + 5);                // expected 1 in 65536 blocks
    CHECK(empty < blocks / 100 * 2 + 5);
    // neighbours are not alike: the chance that two neighbours agree at one half is a half
    long same_h = 0, total_h = 0, same_v = 0, total_v = 0;
    for (int r = 0; r < R - 1; r++)
        for (int c = 0; c < C - 1; c++) {
            same_h += from_b(r, c) == from_b(r, c + 1); total_h++;
            same_v += from_b(r, c) == from_b(r + 1, c); total_v++;
        }
    CHECK(std::fabs((double)same_h / total_h - 0.5) < 0.03);
    CHECK(std::fabs((double)same_v / total_v - 0.5) < 0.03);

    // deterministic: the same blend twice is the same, and does not depend on the pictures
    Picture again;
    blend(a, b, 0.5, TransitionStyle::Dissolve, again);
    CHECK(rows_equal(out, again, 0, R + 1));
    const Picture a2 = make(R, C, 11, U'k'), b2 = make(R, C, 12, U'K');
    Picture other;
    blend(a2, b2, 0.5, TransitionStyle::Dissolve, other);
    bool pattern_same = true;
    for (int r = 0; r < R && pattern_same; r++)
        for (int c = 0; c < C; c++)
            if ((other.cell[(size_t)r * C + c] == b2.cell[(size_t)r * C + c]) != (from_b(r, c) == 1)) { pattern_same = false; break; }
    CHECK(pattern_same);
}

void test_size_mismatch() {
    struct Size { int body, cols; };
    const Size sizes[] = {{8, 20}, {5, 12}, {12, 9}, {1, 1}, {3, 30}, {0, 5}, {4, 0}};
    for (const Size& sa : sizes)
        for (const Size& sb : sizes) {
            const Picture a = make(sa.body, sa.cols, 1, U'a'), b = make(sb.body, sb.cols, 2, U'A');
            for (TransitionStyle st : ALL_STYLES)
                for (double pct : {0.0, 0.3, 0.5, 0.8, 1.0}) {
                    Picture out;
                    blend(a, b, pct, st, out);
                    CHECK_EQ(out.rows, a.rows);
                    CHECK_EQ(out.cols, a.cols);
                    CHECK_EQ(out.cell.size(), (size_t)a.rows * (size_t)a.cols);
                    const int body = std::min(a.rows, b.rows) - 1, cc = std::min(a.cols, b.cols);
                    // beyond the common part: default cells
                    for (int r = 0; r < a.rows; r++)
                        for (int c = 0; c < a.cols; c++) {
                            const bool common = c < cc && (r < body || r == a.rows - 1);
                            if (!common && r != a.rows - 1) CHECK(out.cell[(size_t)r * a.cols + c] == PicCell());
                            if (!common && r == a.rows - 1) CHECK(out.cell[(size_t)r * a.cols + c] == PicCell());
                        }
                    // the bar is b's
                    for (int c = 0; c < cc && a.rows > 0; c++)
                        CHECK(out.cell[(size_t)(a.rows - 1) * a.cols + c] == b.cell[(size_t)(b.rows - 1) * b.cols + c]);
                    if (pct == 0.0)
                        for (int r = 0; r < body; r++)
                            for (int c = 0; c < cc; c++) CHECK(out.cell[(size_t)r * a.cols + c] == a.cell[(size_t)r * a.cols + c]);
                    if (pct == 1.0)
                        for (int r = 0; r < body; r++)
                            for (int c = 0; c < cc; c++) CHECK(out.cell[(size_t)r * a.cols + c] == b.cell[(size_t)r * b.cols + c]);
                }
        }
    // an empty picture
    Picture empty, out;
    const Picture b = make(3, 4, 1, U'a');
    for (TransitionStyle st : ALL_STYLES) {
        blend(empty, b, 0.5, st, out);
        CHECK_EQ(out.rows, 0);
        blend(b, empty, 0.5, st, out);
        CHECK_EQ(out.rows, b.rows);
        blend(empty, empty, 0.5, st, out);
        CHECK_EQ(out.rows, 0);
    }
}

void test_blend_mid_sizes_match_equal_size_result() {
    // where the pictures are the same shape, a bigger b does not change the common part
    const Picture a = make(6, 10, 1, U'a'), b = make(6, 10, 2, U'A');
    Picture bigger = make(9, 14, 2, U'A');
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 10; c++) bigger.cell[(size_t)r * 14 + c] = b.cell[(size_t)r * 10 + c];
    for (TransitionStyle st : ALL_STYLES) {
        Picture x, y;
        blend(a, b, 0.37, st, x);
        blend(a, bigger, 0.37, st, y);
        CHECK(rows_equal(x, y, 0, 6));
    }
}

}  // namespace

// ---------------------------------------------------------------- sliding

void test_compose_slide() {
    const Picture a = make(4, 10, 1, U'a');
    const Picture b = make(4, 10, 2, U'A');
    Picture out;

    compose({{&a, 0, 0}}, a, out);                                 // one layer, in place: the picture itself
    CHECK_EQ(out.rows, 5);
    CHECK_EQ(out.cols, 10);
    CHECK(!out.retained);
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 10; c++) CHECK(out.cell[(size_t)r * 10 + c] == a.cell[(size_t)r * 10 + c]);

    // a slid left by 3 with b following it (b's left edge at column 7): a's columns 3..9 land on 0..6, b's columns 0..2 on 7..9
    compose({{&a, -3, 0}, {&b, 7, 0}}, b, out);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 7; c++) CHECK(out.cell[(size_t)r * 10 + c] == a.cell[(size_t)r * 10 + c + 3]);
        for (int c = 7; c < 10; c++) CHECK(out.cell[(size_t)r * 10 + c] == b.cell[(size_t)r * 10 + c - 7]);
    }
    for (int c = 0; c < 10; c++) CHECK(out.cell[(size_t)4 * 10 + c] == b.cell[(size_t)4 * 10 + c]);          // the bar row is the bar picture's

    // a later layer is over an earlier one; a drop from the top: b's last two rows over a's first two
    compose({{&a, 0, 0}, {&b, 0, -2}}, a, out);
    for (int c = 0; c < 10; c++) {
        CHECK(out.cell[(size_t)0 * 10 + c] == b.cell[(size_t)2 * 10 + c]);
        CHECK(out.cell[(size_t)1 * 10 + c] == b.cell[(size_t)3 * 10 + c]);
        CHECK(out.cell[(size_t)2 * 10 + c] == a.cell[(size_t)2 * 10 + c]);
    }

    // a layer entirely off the body, or a null one, changes nothing; what no layer covers is blank
    compose({{&b, 10, 0}, {&b, 0, 4}, {&b, -10, 0}, {nullptr, 0, 0}}, a, out);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) CHECK(out.cell[(size_t)r * 10 + c] == PicCell());
    compose({{&a, 4, 1}}, a, out);                                 // down and right: the top row and the left columns are blank
    CHECK(out.cell[(size_t)0 * 10 + 5] == PicCell());
    CHECK(out.cell[(size_t)2 * 10 + 3] == PicCell());
    CHECK(out.cell[(size_t)2 * 10 + 4] == a.cell[(size_t)1 * 10 + 0]);

    // what no layer covers is the fill, not the default cell
    PicCell fill;
    fill.text = true; fill.ch = U'.'; fill.alt_lum = 1; fill.alt_r = 5;
    compose({{&a, 3, 0}}, a, out, fill);
    CHECK(out.cell[(size_t)0 * 10 + 0] == fill);
    CHECK(out.cell[(size_t)3 * 10 + 2] == fill);
    CHECK(out.cell[(size_t)0 * 10 + 3] == a.cell[0]);
    CHECK(out.cell[(size_t)4 * 10 + 0] == a.cell[(size_t)4 * 10 + 0]);                    // the bar row is not the fill

    // odd sizes are safe
    Picture empty;
    compose({{&a, 0, 0}}, empty, out);
    CHECK_EQ(out.rows, 0);
    compose({{&empty, 0, 0}}, a, out);
    CHECK_EQ(out.rows, 5);
    const Picture narrow = make(4, 4, 3, U'x');
    compose({{&a, 0, 0}}, narrow, out);                            // a wider layer than the body is cut
    CHECK_EQ(out.cols, 4);
    CHECK(out.cell[(size_t)1 * 4 + 3] == a.cell[(size_t)1 * 10 + 3]);
}

void test_ease_and_slide_time() {
    CHECK_EQ(ease_out(0.0), 0.0);
    CHECK_EQ(ease_out(1.0), 1.0);
    CHECK_EQ(ease_out(-3.0), 0.0);
    CHECK_EQ(ease_out(7.0), 1.0);
    CHECK_EQ(ease_out(std::nan("")), 0.0);
    double last = 0;
    for (int i = 1; i <= 100; i++) {                                // rising, and fast at first (ahead of a straight line)
        const double e = ease_out(i / 100.0);
        CHECK(e >= last);
        CHECK(e >= i / 100.0 - 1e-12);
        last = e;
    }
    CHECK_EQ(ease_in(0.0), 0.0);
    CHECK_EQ(ease_in(1.0), 1.0);
    CHECK_EQ(ease_in(std::nan("")), 0.0);
    CHECK_EQ(ease_in(-1.0), 0.0);
    CHECK_EQ(ease_in(2.0), 1.0);
    for (int i = 1; i < 100; i++) CHECK(ease_in(i / 100.0) <= i / 100.0 + 1e-12);        // slow at first: behind a straight line
    CHECK_EQ(slide_ms(1, 250), 250.0);                              // one step: the base
    CHECK(slide_ms(2, 250) > 250 && slide_ms(2, 250) < 500);
    CHECK_EQ(slide_ms(9, 250), 500.0);                              // capped at twice
    CHECK_EQ(slide_ms(100, 250), 500.0);
    CHECK(slide_ms(0.5, 250) < 250 && slide_ms(0.5, 250) >= 75);    // part of a step: shorter
    CHECK_EQ(slide_ms(0.01, 250), 75.0);                            // but not under 0.3 of it
    CHECK_EQ(slide_ms(0, 250), 0.0);
    CHECK_EQ(slide_ms(3, 0), 0.0);
    double prev = 0;
    for (double st = 0.05; st < 12; st += 0.05) { const double ms = slide_ms(st, 250); CHECK(ms >= prev); prev = ms; }   // never shorter for a longer way
}

void test_dissolve_keeps_wide_pairs() {
    // a has a wide letter over columns 2 and 3; b has ordinary letters there: no pct shows half of the pair
    Picture a = make(3, 8, 11, U'a'), b = make(3, 8, 12, U'A');
    for (int r = 0; r < 3; r++) {
        PicCell h = a.at(r, 2), t = a.at(r, 3);
        h.ch = 0x4E2D; h.attrs = vtscreen::CELL_WIDE; t.ch = U' '; t.attrs = vtscreen::CELL_TAIL;
        a.at(r, 2) = h; a.at(r, 3) = t;
    }
    Picture out;
    for (int k = 1; k < 100; k++) {
        blend(a, b, k / 100.0, TransitionStyle::Dissolve, out);
        for (int r = 0; r < 3; r++) {
            const bool head = (out.at(r, 2).attrs & vtscreen::CELL_WIDE) != 0, tail = (out.at(r, 3).attrs & vtscreen::CELL_TAIL) != 0;
            CHECK(head == tail);
        }
    }
    // and with the pair in a going to a plain b the other way round
    blend(b, a, 0.5, TransitionStyle::Dissolve, out);
    for (int r = 0; r < 3; r++) CHECK(((out.at(r, 2).attrs & vtscreen::CELL_WIDE) != 0) == ((out.at(r, 3).attrs & vtscreen::CELL_TAIL) != 0));
}

// ---------------------------------------------------------------- pictures made of pixels

using vtscreen::ImagePlacement;
using vtscreen::Pixel;
using vtscreen::Raster;

const int CW = 10, CH = 20;                                  // the cell of the pretend terminal

// A picture of `body` rows with a Raster of w x h pixels placed at (row, col): every pixel's colour says where it is and which one it is (tag).
Picture with_image(Picture p, int row, int col, int w, int h, int tag, double letter_rate = -1) {
    auto r = std::make_shared<Raster>();
    r->init(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) r->set(x, y, 40 + (x * 3 + tag * 17) % 200, 40 + (y * 5 + tag * 29) % 200, 60 + tag * 40, 255);
    p.place_image(r, row, col, letter_rate);
    return p;
}

// what a picture's placements make of the pane, pixel by pixel (the later over the earlier); the pane is rows x cols cells
std::vector<Pixel> pane(const Picture& p, int rows, int cols) {
    std::vector<Pixel> out((size_t)rows * CH * cols * CW);
    const int W = cols * CW, H = rows * CH;
    for (const ImagePlacement& ip : p.images) {
        if (!ip.raster) continue;
        for (int y = 0; y < ip.raster->h; y++)
            for (int x = 0; x < ip.raster->w; x++) {
                const int px = ip.col * CW + x, py = ip.row * CH + y;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;      // (what the screen cuts off)
                if (ip.raster->at(x, y).a) out[(size_t)py * W + px] = ip.raster->at(x, y);
            }
    }
    return out;
}

bool same_pane(const std::vector<Pixel>& a, const std::vector<Pixel>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if ((a[i].a == 0) != (b[i].a == 0)) return false;
        if (a[i].a && (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b)) return false;
    }
    return true;
}

int count_ink(const std::vector<Pixel>& v) { int n = 0; for (const Pixel& p : v) n += p.a ? 1 : 0; return n; }

void test_images_at_the_ends() {
    Picture a = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1);
    Picture b = with_image(make(6, 12, 2, U'A'), 0, 0, 50, 60, 2);
    Picture out;
    for (TransitionStyle st : ALL_STYLES) {
        blend(a, b, 0.0, st, out, CW, CH);
        CHECK(out.images.size() == 1 && out.images[0].raster == a.images[0].raster && out.images[0].row == 1 && out.images[0].col == 2);
        blend(a, b, 1.0, st, out, CW, CH);
        CHECK(out.images.size() == 1 && out.images[0].raster == b.images[0].raster && out.images[0].row == 0);
        blend(a, b, -3.0, st, out, CW, CH);
        CHECK(out.images.size() == 1 && out.images[0].raster == a.images[0].raster);
    }
    // a picture that has none gives none, and what out carried before is not left on it
    Picture plain = make(6, 12, 3, U'x');
    blend(a, b, 0.0, TransitionStyle::Dissolve, out, CW, CH);
    CHECK(out.images.size() == 1);
    blend(plain, plain, 0.5, TransitionStyle::Dissolve, out, CW, CH);
    CHECK(out.images.empty());
    blend(a, b, 0.5, TransitionStyle::Crossfade, out, CW, CH);
    CHECK(!out.images.empty());
    blend(plain, plain, 0.5, TransitionStyle::Dip, out, CW, CH);
    CHECK(out.images.empty());
    blend(a, b, 0.5, TransitionStyle::Crossfade, out, CW, CH);
    blend(plain, plain, 0.0, TransitionStyle::Crossfade, out, CW, CH);
    CHECK(out.images.empty() && out.fade_level == 0);
    blend(a, b, 0.5, TransitionStyle::Crossfade, out, CW, CH);
    blend(plain, plain, 1.0, TransitionStyle::Crossfade, out, CW, CH);
    CHECK(out.images.empty());
    blend(a, b, 0.5, TransitionStyle::Crossfade, out, CW, CH);
    CHECK(out.fade_level == 1);
    blend(plain, plain, 0.5, TransitionStyle::Dip, out, CW, CH);
    CHECK(out.fade_level == 0);
    // screens of different sizes: no pixels come along, whatever the style and the place on the way
    Picture other = with_image(make(5, 10, 4, U'z'), 0, 0, 20, 20, 3);
    for (TransitionStyle st : ALL_STYLES)
        for (double pct : {0.0, 0.3, 0.7, 1.0}) {
            blend(a, other, pct, st, out, CW, CH);
            CHECK(out.images.empty());
        }
    // a screen that is not drawn: nothing
    Picture empty;
    blend(empty, empty, 0.5, TransitionStyle::Dissolve, out, CW, CH);
    CHECK(out.images.empty());
}

void test_images_crossfade() {
    Picture a = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1);
    Picture b = with_image(make(6, 12, 2, U'A'), 2, 4, 50, 60, 2);
    Picture out;
    blend(a, b, 0.25, TransitionStyle::Crossfade, out, CW, CH);
    CHECK(out.images.size() == 2);                                          // both dissolve (vtscreen's fade)
    const int bn = 50 * 60;
    int ink_a = 0, ink_b = 0;
    for (const ImagePlacement& ip : out.images) {
        int n = 0;
        for (const Pixel& q : ip.raster->px) n += q.a ? 1 : 0;
        (ip.row == 1 ? ink_a : ink_b) += n;
    }
    CHECK(ink_a > 30 * 40 * 0.6 && ink_a < 30 * 40 * 0.9);                  // about 3/4 of a's pixels
    CHECK(ink_b > bn * 0.1 && ink_b < bn * 0.4);                            // about 1/4 of b's
    // the same picture twice, the same fade: the same result
    Picture again;
    blend(a, b, 0.25, TransitionStyle::Crossfade, again, CW, CH);
    CHECK(same_pane(pane(out, 6, 12), pane(again, 6, 12)));
    // sizes that differ: the cells are mixed over what they share, and no pixels come along
    Picture c = make(5, 10, 4, U'z');
    blend(a, c, 0.5, TransitionStyle::Crossfade, out, CW, CH);
    CHECK(out.images.empty());
}

void test_images_dissolve() {
    Picture a = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1);
    a = with_image(a, 3, 5, 40, 20, 3);
    Picture b = with_image(make(6, 12, 2, U'A'), 0, 0, 50, 60, 2);
    const std::vector<Pixel> pa = pane(a, 6, 12), pb = pane(b, 6, 12);
    Picture out;
    int mixed = 0;
    for (int k = 1; k < 20; k++) {
        const double pct = k / 20.0;
        blend(a, b, pct, TransitionStyle::Dissolve, out, CW, CH);
        const std::vector<Pixel> got = pane(out, 6, 12);
        // the model: each cell's pixels are a's or b's, as its letter is
        std::vector<Pixel> want(got.size());
        const int W = 12 * CW;
        int from_a = 0, from_b = 0;
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 12; c++) {
                const bool pick_b = out.at(r, c).ch >= U'A' && out.at(r, c).ch < U'A' + 20;
                (pick_b ? from_b : from_a)++;
                const std::vector<Pixel>& src = pick_b ? pb : pa;
                for (int y = 0; y < CH; y++)
                    for (int x = 0; x < CW; x++) want[(size_t)(r * CH + y) * W + c * CW + x] = src[(size_t)(r * CH + y) * W + c * CW + x];
            }
        CHECK(same_pane(got, want));
        if (from_a > 0 && from_b > 0) mixed++;
    }
    CHECK(mixed > 10);                                                      // (the model said something: the cells were mixed)
    // the letters of the cells that changed over are b's: all of b's pixels that are left are in cells where b's letter is
    // nothing drawn twice: no pixel of a under a pixel of b in a cell that changed over
    blend(a, b, 0.5, TransitionStyle::Dissolve, out, CW, CH);
    CHECK(out.images.size() == 3);
    // a's and b's rasters are not changed by it
    CHECK(same_pane(pane(a, 6, 12), pa) && same_pane(pane(b, 6, 12), pb));
    // without the size of a cell: the nearer screen's pictures whole
    blend(a, b, 0.3, TransitionStyle::Dissolve, out);
    CHECK(out.images.size() == a.images.size() && out.images[0].raster == a.images[0].raster);
    blend(a, b, 0.7, TransitionStyle::Dissolve, out);
    CHECK(out.images.size() == 1 && out.images[0].raster == b.images[0].raster);
    // a picture with nothing in the cells that are taken from it is not copied
    Picture only_a = with_image(make(6, 12, 1, U'a'), 0, 0, 10, 20, 1);          // one cell, row 0 column 0
    Picture none = make(6, 12, 2, U'A');
    int same_raster = 0;
    for (int k = 1; k < 20; k++) {
        blend(only_a, none, k / 20.0, TransitionStyle::Dissolve, out, CW, CH);
        if (!out.images.empty() && out.images[0].raster == only_a.images[0].raster) same_raster++;
    }
    CHECK(same_raster > 0 && same_raster < 19);                                // (shared while its cell is a's, a copy with nothing in it ... or the copy cleared, after)
}

void test_images_dip() {
    Picture a = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1);
    Picture b = with_image(make(6, 12, 2, U'A'), 0, 0, 20, 20, 2);
    Picture out;
    for (int k = 1; k < 20; k++) {
        const double pct = k / 20.0;
        blend(a, b, pct, TransitionStyle::Dip, out, CW, CH);
        const bool first = pct < 0.5;
        const double kk = first ? 1.0 - 2.0 * pct : 2.0 * pct - 1.0;
        const Picture& s = first ? a : b;
        if (kk <= 0) { CHECK(out.images.empty()); continue; }
        CHECK(out.images.size() == 1);
        const Raster& want = *s.images[0].raster;
        const Raster& got = *out.images[0].raster;
        bool ok = got.w == want.w && got.h == want.h;
        for (int y = 0; ok && y < want.h; y++)
            for (int x = 0; ok && x < want.w; x++) {
                const Pixel& q = want.at(x, y);
                const Pixel& g = got.at(x, y);
                ok = g.a == q.a && g.r == (uint8_t)std::floor(q.r * kk + 0.5) && g.g == (uint8_t)std::floor(q.g * kk + 0.5) &&
                     g.b == (uint8_t)std::floor(q.b * kk + 0.5);
            }
        CHECK(ok);
        CHECK(out.images[0].row == s.images[0].row && out.images[0].col == s.images[0].col);
    }
    blend(a, b, 0.5, TransitionStyle::Dip, out, CW, CH);
    CHECK(out.images.empty());                                              // the middle is dark
    blend(a, b, 0.25, TransitionStyle::Dip, out, CW, CH);
    CHECK(a.images[0].raster->at(0, 0).r == 40 + (0 * 3 + 17) % 200);       // a's own pixels are not touched
}

void test_images_compose() {
    Picture a = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1, 7.5);
    Picture b = with_image(make(6, 12, 2, U'A'), 0, 0, 20, 20, 2);
    Picture bar = make(6, 12, 3, U'z');
    Picture out;
    compose({{&a, -5, 0}, {&b, 7, 0}}, bar, out);
    CHECK(out.images.size() == 2);
    CHECK(out.images[0].raster == a.images[0].raster && out.images[0].row == 1 && out.images[0].col == 2 - 5 && out.images[0].letter_rate == 7.5);
    CHECK(out.images[1].raster == b.images[0].raster && out.images[1].row == 0 && out.images[1].col == 7);
    compose({{&a, 0, 3}}, bar, out);
    CHECK(out.images.size() == 1 && out.images[0].row == 4 && out.images[0].col == 2);
    // the pixels land on the cells they were on, moved with the layer
    Picture moved;
    compose({{&a, 3, 2}}, bar, moved);
    const std::vector<Pixel> was = pane(a, 6, 12), now = pane(moved, 6, 12);
    bool ok = true;
    for (int y = 0; y < 5 * CH && ok; y++)                                  // (rows that fit the 5 rows of the body)
        for (int x = 0; x < 12 * CW && ok; x++) {
            const int sx = x - 3 * CW, sy = y - 2 * CH;
            const Pixel& g = now[(size_t)y * 12 * CW + x];
            const Pixel w = (sx >= 0 && sy >= 0) ? was[(size_t)sy * 12 * CW + sx] : Pixel();
            if (y >= 5 * CH) continue;
            ok = (g.a == 0) == (w.a == 0) && (!g.a || (g.r == w.r && g.g == w.g && g.b == w.b));
        }
    CHECK(ok);
    // no layer, or one with no pictures: none (and what out had before is gone)
    compose({}, bar, out);
    CHECK(out.images.empty());
    Picture plain = make(6, 12, 4, U'p');
    compose({{&a, 0, 0}}, bar, out);
    compose({{&plain, 0, 0}}, bar, out);
    CHECK(out.images.empty());
    // a layer with a null picture
    compose({{nullptr, 0, 0}, {&a, 0, 0}}, bar, out);
    CHECK(out.images.size() == 1);
}

void test_images_without() {
    Picture a = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1);          // cells rows 1-2, columns 2-4
    const std::vector<Pixel> was = pane(a, 6, 12);
    // a hole in the middle
    const std::vector<ImagePlacement> hole = images_without(a, 1, 3, 1, 1, CW, CH);
    CHECK(hole.size() == 1 && hole[0].raster != a.images[0].raster);
    Picture h; h.init(5, 12); h.images = hole;
    const std::vector<Pixel> got = pane(h, 6, 12);
    CHECK(count_ink(got) == count_ink(was) - CW * CH);
    bool ok = true;
    for (int y = 0; y < 6 * CH; y++)
        for (int x = 0; x < 12 * CW; x++) {
            const bool inside = y >= 1 * CH && y < 2 * CH && x >= 3 * CW && x < 4 * CW;
            const Pixel& g = got[(size_t)y * 12 * CW + x];
            const Pixel& w = was[(size_t)y * 12 * CW + x];
            if (inside) ok = ok && g.a == 0;
            else ok = ok && g.a == w.a && (!g.a || (g.r == w.r && g.g == w.g && g.b == w.b));
        }
    CHECK(ok);
    CHECK(same_pane(pane(a, 6, 12), was));                                   // the picture itself is as it was
    // a box that covers all of it, more than it, and some of it at the edge
    CHECK(count_ink(pane([&] { Picture t; t.init(5, 12); t.images = images_without(a, 0, 0, 6, 12, CW, CH); return t; }(), 6, 12)) == 0);
    CHECK(count_ink(pane([&] { Picture t; t.init(5, 12); t.images = images_without(a, 2, 4, 9, 9, CW, CH); return t; }(), 6, 12)) == count_ink(was) - CW * CH);     // (only the cell at row 2, column 4 is in it)
    // no pixels in the box: the same raster, not a copy
    const std::vector<ImagePlacement> miss = images_without(a, 4, 6, 2, 3, CW, CH);
    CHECK(miss.size() == 1 && miss[0].raster == a.images[0].raster);
    // a box over cells of the raster that have no pixels in them: the same raster too
    Picture gap = with_image(make(6, 12, 1, U'a'), 1, 2, 30, 40, 1);
    {
        Raster r2 = *gap.images[0].raster;
        for (int y = 0; y < CH; y++) for (int x = CW; x < 2 * CW; x++) r2.at(x, y) = Pixel();        // the cell at row 0, column 1 of it: empty
        gap.images[0].raster = std::make_shared<Raster>(r2);
        const std::vector<ImagePlacement> in_gap = images_without(gap, 1, 3, 1, 1, CW, CH);            // that cell, on the pane: row 1, column 3
        CHECK(in_gap.size() == 1 && in_gap[0].raster == gap.images[0].raster);
        const std::vector<ImagePlacement> next = images_without(gap, 1, 4, 1, 1, CW, CH);              // the cell beside it has pixels
        CHECK(next.size() == 1 && next[0].raster != gap.images[0].raster);
    }
    // the cell size not known: as it is
    const std::vector<ImagePlacement> unknown = images_without(a, 0, 0, 6, 12, 0, 0);
    CHECK(unknown.size() == 1 && unknown[0].raster == a.images[0].raster);
    // a picture whose last pieces are cut off the cell size (30 x 40 pixels is 3 x 2 cells; try a raster that is not a whole number of cells)
    Picture odd = with_image(make(6, 12, 1, U'a'), 0, 0, 25, 30, 2);        // 3 x 2 cells, the last of them partial
    Picture t; t.init(5, 12); t.images = images_without(odd, 1, 2, 1, 1, CW, CH);
    CHECK(count_ink(pane(t, 6, 12)) == 25 * 30 - 5 * 10);                    // the cell at row 1, column 2 holds 5 x 10 pixels of it
    // none at all
    Picture plain = make(6, 12, 3, U'p');
    CHECK(images_without(plain, 0, 0, 6, 12, CW, CH).empty());
}

int main() {
    test_images_at_the_ends();
    test_images_crossfade();
    test_images_dissolve();
    test_images_dip();
    test_images_compose();
    test_images_without();
    test_dissolve_keeps_wide_pairs();
    test_compose_slide();
    test_ease_and_slide_time();
    test_names();
    test_ends_exact();
    test_out_is_assigned_and_not_retained();
    test_bar_from_b();
    test_crossfade();
    test_crossfade_mixed_cells();
    test_dip();
    test_dissolve();
    test_size_mismatch();
    test_blend_mid_sizes_match_equal_size_result();
    if (failures == 0) std::printf("all transition tests passed\n");
    return failures == 0 ? 0 : 1;
}
