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

int main() {
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
