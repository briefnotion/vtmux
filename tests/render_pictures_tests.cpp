// Tests for PanePainter and Sixel pictures: the pieces of pictures the emulator holds become one Raster on the vtscreen Picture,
// just big enough for the cells that have a piece, and kept up to date as the screen changes and scrolls.
// sources: ../src/render.cpp ../src/emulator.cpp ../src/vtparser.cpp ../src/width.cpp
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "../src/emulator.h"
#include "../src/render.h"

using namespace vtmux;
using vtscreen::Picture;
using vtscreen::Pixel;
using vtscreen::Raster;

namespace {

const int CW = 10, CH = 20;

Picture make_pic(int rows, int cols) {
    Picture p;
    p.init(rows, cols);
    p.retain();
    p.consume();
    return p;
}

Emulator make(int rows, int cols, size_t scrollback = 100) {
    Emulator e(rows, cols, scrollback);
    e.set_graphics(CW, CH, true);
    return e;
}

// A Sixel picture of w x h pixels in which every pixel's colour says where it is (and which picture: seed).
std::string sixel(int w, int h, int seed = 0) {
    std::vector<Pixel> px((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Pixel& p = px[(size_t)y * w + x];
            p.r = (uint8_t)(((x + seed) % 7) * 35 + 10);
            p.g = (uint8_t)(((y * 3 + seed) % 7) * 35 + 10);
            p.b = (uint8_t)((seed * 20) % 200 + 30);
            p.a = 255;
        }
    return vtscreen::detail::sixel_encode(px.data(), w, w, h);
}

std::string at(int row, int col) { return "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H"; }

// the pixels of the pane as the emulator says they should be: every cell's piece of its picture
std::vector<Pixel> expected(const Emulator& emu, int rows, int cols, size_t scroll_back) {
    const int W = cols * CW, H = rows * CH;
    std::vector<Pixel> out((size_t)W * H);
    const size_t history = emu.scrollback_size();
    scroll_back = std::min(scroll_back, history);
    const size_t top = history - scroll_back;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            Tile t;
            const size_t line = top + (size_t)r;
            if (scroll_back == 0) t = emu.picture_tile(r, c);
            else if (line >= history) t = emu.picture_tile((int)(line - history), c);
            else { const std::vector<Tile>& v = emu.scrollback_tiles(line); if ((size_t)c < v.size()) t = v[(size_t)c]; }
            if (t.empty()) continue;
            const auto img = emu.picture(t.image);
            if (!img) continue;
            for (int y = 0; y < CH; y++)
                for (int x = 0; x < CW; x++) {
                    const int sx = t.col * CW + x, sy = t.row * CH + y;
                    if (sx < img->w && sy < img->h) out[(size_t)(r * CH + y) * W + c * CW + x] = img->at(sx, sy);
                }
        }
    return out;
}

// the pixels of the pane as the picture carries them
std::vector<Pixel> carried(const Picture& pic, int rows, int cols) {
    const int W = cols * CW, H = rows * CH;
    std::vector<Pixel> out((size_t)W * H);
    for (const vtscreen::ImagePlacement& p : pic.images) {
        if (!p.raster) continue;
        for (int y = 0; y < p.raster->h; y++)
            for (int x = 0; x < p.raster->w; x++) {
                const int px = p.col * CW + x, py = p.row * CH + y;
                if (px < 0 || py < 0 || px >= W || py >= H) { if (p.raster->at(x, y).a) return std::vector<Pixel>(1); continue; }   // a pixel off the pane: wrong
                if (p.raster->at(x, y).a) out[(size_t)py * W + px] = p.raster->at(x, y);
            }
    }
    return out;
}

bool same_pixels(const std::vector<Pixel>& a, const std::vector<Pixel>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if ((a[i].a == 0) != (b[i].a == 0)) return false;
        if (a[i].a && (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b)) return false;
    }
    return true;
}

int ink(const std::vector<Pixel>& v) { int n = 0; for (const Pixel& p : v) n += p.a ? 1 : 0; return n; }

// paint and say whether the picture carries what the emulator holds
bool paints_right(PanePainter& pp, Emulator& e, Picture& pic, size_t scroll_back = 0, bool cursor = false) {
    pp.paint(e, pic, cursor, Theme(), scroll_back);
    const int rows = pic.rows - 1, cols = pic.cols;
    return same_pixels(carried(pic, rows, cols), expected(e, std::min(rows, e.rows()), std::min(cols, e.cols()), scroll_back));
}

}  // namespace

int main() {
    // ---- no pictures: nothing is placed
    {
        Emulator e = make(6, 20);
        Picture p = make_pic(6, 20);
        PanePainter pp;
        e.feed("hello");
        pp.paint(e, p, false, Theme());
        CHECK(p.images.empty());
    }

    // ---- one picture: a raster just big enough, at the cell where it begins
    {
        Emulator e = make(8, 30);
        Picture p = make_pic(8, 30);
        PanePainter pp;
        e.feed(at(2, 5) + sixel(25, 50));                      // 3 x 3 cells
        CHECK(paints_right(pp, e, p));
        CHECK_EQ(p.images.size(), (size_t)1);
        CHECK(p.images[0].row == 2 && p.images[0].col == 5);
        CHECK(p.images[0].raster->w == 3 * CW && p.images[0].raster->h == 3 * CH);
        CHECK(p.images[0].letter_rate < 0);
        // the exact pixels: the first piece is the top left of the picture, the last the bottom right
        const Raster& r = *p.images[0].raster;
        const auto src = e.picture(e.picture_tile(2, 5).image);
        CHECK(src && src->w == 25 && src->h == 50);
        CHECK(r.at(0, 0) == src->at(0, 0) && r.at(0, 0).a == 255);
        CHECK(r.at(24, 49).a == 255 && r.at(25, 49).a == 0 && r.at(24, 50).a == 0);       // the cut edge of the last pieces: nothing
        CHECK(r.at(CW, CH) == src->at(CW, CH) && r.at(CW + 3, CH + 4) == src->at(CW + 3, CH + 4));     // the piece (1,1) comes from (10,20) on
        CHECK(!(src->at(0, 0) == src->at(CW, CH)));              // (and the pixels differ from place to place, so that this says something)
        CHECK(ink(carried(p, 8, 30)) == 25 * 50);
        CHECK(p.images[0].raster.use_count() >= 1);
        // the same screen painted again: nothing is made again
        const Raster* before = p.images[0].raster.get();
        pp.paint(e, p, false, Theme());
        CHECK(p.images.size() == 1 && p.images[0].raster.get() == before);
    }

    // ---- a letter over a piece takes it out of the raster; when the block shrinks the raster is made again
    {
        Emulator e = make(8, 30);
        Picture p = make_pic(8, 30);
        PanePainter pp;
        e.feed(at(0, 0) + sixel(30, 40));                      // 3 x 2
        CHECK(paints_right(pp, e, p));
        const int full = ink(carried(p, 8, 30));
        CHECK(full == 30 * 40);
        const Raster* r0 = p.images[0].raster.get();
        e.feed(at(0, 1) + "x");                                // a hole in the middle of the top row
        CHECK(paints_right(pp, e, p));
        CHECK(ink(carried(p, 8, 30)) == full - CW * CH);
        CHECK(p.images[0].raster.get() == r0);                 // (the same block: changed where it was)
        e.feed(at(0, 0) + "\x1b[2K");                          // the top row gone: the block is only the second row now
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.size() == 1 && p.images[0].row == 1 && p.images[0].col == 0);
        CHECK(p.images[0].raster->w == 3 * CW && p.images[0].raster->h == CH);
        e.feed(at(1, 0) + "\x1b[2K");                          // all gone
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.empty());
        e.feed(sixel(10, 20));
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.size() == 1);
    }

    // ---- two pictures, and a picture over a picture
    {
        Emulator e = make(8, 30);
        Picture p = make_pic(8, 30);
        PanePainter pp;
        e.feed(at(0, 0) + sixel(20, 20, 1) + at(4, 20) + sixel(20, 40, 2));
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.size() == 1);                           // (one raster for the block that holds both)
        CHECK(p.images[0].row == 0 && p.images[0].col == 0 && p.images[0].raster->w == 22 * CW && p.images[0].raster->h == 6 * CH);
        e.feed(at(0, 1) + sixel(20, 20, 3));                   // over the first
        CHECK(paints_right(pp, e, p));
    }

    // ---- a copy of the picture kept (for a transition) keeps what it had
    {
        Emulator e = make(6, 20);
        Picture p = make_pic(6, 20);
        PanePainter pp;
        e.feed(at(0, 0) + sixel(40, 40));
        CHECK(paints_right(pp, e, p));
        Picture snapshot = p;                                  // as App keeps one for a fade
        const std::vector<Pixel> was = carried(snapshot, 5, 20);
        CHECK(snapshot.images.size() == 1 && snapshot.images[0].raster.get() == p.images[0].raster.get());
        pp.paint(e, p, false, Theme());                        // nothing changed: nothing is copied for the holder's sake
        CHECK(snapshot.images[0].raster.get() == p.images[0].raster.get());
        pp.paint(e, p, true, Theme());
        CHECK(snapshot.images[0].raster.get() == p.images[0].raster.get());
        e.feed(at(0, 0) + "x" + at(1, 1) + "y");
        CHECK(paints_right(pp, e, p));
        CHECK(same_pixels(carried(snapshot, 5, 20), was));     // the snapshot did not change under its holder ...
        CHECK(snapshot.images[0].raster.get() != p.images[0].raster.get());       // ... because the painter made a raster of its own
        CHECK(ink(carried(p, 5, 20)) == ink(was) - 2 * CW * CH);
        // with nobody else holding it, it is changed where it is (nothing is made again for every little change)
        snapshot = Picture();
        const Raster* r = p.images[0].raster.get();
        e.feed(at(2, 2) + "z");
        CHECK(paints_right(pp, e, p));
        CHECK(p.images[0].raster.get() == r);
    }

    // ---- a Picture that was made again gets its pictures back
    {
        Emulator e = make(6, 20);
        Picture p = make_pic(6, 20);
        PanePainter pp;
        e.feed(sixel(20, 20));
        CHECK(paints_right(pp, e, p));
        p.init(5, 20);
        p.retain();
        pp.invalidate();
        CHECK(p.images.empty());
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.size() == 1);
        // another painter, another picture: the same
        PanePainter other;
        Picture q = make_pic(5, 20);
        CHECK(paints_right(other, e, q));
    }

    // ---- scrolled back: the history lines show their pictures, and coming back to the live screen puts the screen's back
    {
        Emulator e = make(5, 20);
        Picture p = make_pic(5, 20);
        PanePainter pp;
        e.feed(at(0, 0) + sixel(20, 40, 4));                   // rows 0 and 1
        e.feed(at(4, 0) + "\n\n\n");                           // three lines scroll off: the picture is in the history
        CHECK(e.scrollback_size() == 3);
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.empty());                               // none on the live screen
        CHECK(paints_right(pp, e, p, 3));                      // all the way back: the picture is at the top
        CHECK(p.images.size() == 1 && p.images[0].row == 0 && p.images[0].raster->h == 2 * CH);
        CHECK(paints_right(pp, e, p, 2));
        CHECK(p.images.size() == 1 && p.images[0].row == 0);   // one line on: the picture's first row has left the view, its second is at the top
        CHECK(p.images[0].raster->h == CH);
        CHECK(paints_right(pp, e, p, 1));
        CHECK(p.images.empty());
        CHECK(paints_right(pp, e, p, 3));
        CHECK(paints_right(pp, e, p));                         // back to live
        CHECK(p.images.empty());
        // a view that holds history lines and screen lines together, with a picture on each
        e.feed(at(0, 0) + sixel(10, 20, 5));
        CHECK(paints_right(pp, e, p, 2));
        CHECK(paints_right(pp, e, p, 3));
        CHECK(paints_right(pp, e, p, 100));                    // (held to the history)
    }

    // ---- the other screen, wide letters, a screen narrower than the pane
    {
        Emulator e = make(6, 20);
        Picture p = make_pic(6, 20);
        PanePainter pp;
        e.feed(at(0, 0) + sixel(20, 20));
        CHECK(paints_right(pp, e, p));
        e.feed("\x1b[?1049h");
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.empty());
        e.feed(at(1, 1) + sixel(20, 20, 2));
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.size() == 1 && p.images[0].row == 1);
        e.feed("\x1b[?1049l");
        CHECK(paints_right(pp, e, p));
        CHECK(p.images.size() == 1 && p.images[0].row == 0);
        // a picture on a wide letter's cells
        e.feed(at(3, 2) + "\xE4\xB8\xAD" + at(3, 0) + sixel(60, 20));
        CHECK(paints_right(pp, e, p));
        // a picture and a cursor
        CHECK(paints_right(pp, e, p, 0, true));
        // a wrong sized picture is safe
        Picture small = make_pic(2, 8);
        PanePainter pq;
        pq.paint(e, small, false, Theme());
        CHECK(carried(small, 2, 8).size() == (size_t)(8 * CW * 2 * CH));
        for (const vtscreen::ImagePlacement& ip : small.images)
            CHECK(ip.row + (ip.raster->h + CH - 1) / CH <= 2 && ip.col + (ip.raster->w + CW - 1) / CW <= 8);
    }

    // ---- the cell changed size: the emulator drops its pictures, and so does the picture
    {
        Emulator e = make(6, 20);
        Picture p = make_pic(6, 20);
        PanePainter pp;
        e.feed(sixel(20, 20));
        pp.paint(e, p, false, Theme());
        CHECK(p.images.size() == 1);
        e.set_graphics(8, 16, true);
        pp.paint(e, p, false, Theme());
        CHECK(p.images.empty());
        e.feed(sixel(16, 16));
        pp.paint(e, p, false, Theme());
        CHECK(p.images.size() == 1 && p.images[0].raster->w == 2 * 8 && p.images[0].raster->h == 16);
        // the cell changed and a picture of the same shape in cells came before the next paint: the raster is made again for the new cell
        Emulator e2 = make(6, 20);
        Picture p2 = make_pic(6, 20);
        PanePainter pp2;
        e2.feed(sixel(20, 20));
        pp2.paint(e2, p2, false, Theme());
        e2.set_graphics(8, 16, true);
        e2.feed(at(0, 0) + sixel(16, 16));
        pp2.paint(e2, p2, false, Theme());
        CHECK(p2.images.size() == 1 && p2.images[0].raster->w == 2 * 8 && p2.images[0].raster->h == 16);
        // no cell size known: no pictures, and nothing placed
        Emulator n(6, 20);
        Picture q = make_pic(6, 20);
        PanePainter pn;
        n.feed(sixel(20, 20));
        pn.paint(n, q, false, Theme());
        CHECK(q.images.empty());
    }

    // ---- a picture forgotten to make room disappears from the picture
    {
        Emulator e = make(8, 20);
        e.set_picture_budget(2500);
        Picture p = make_pic(8, 20);
        PanePainter pp;
        e.feed(at(0, 0) + sixel(10, 20, 1) + at(1, 0) + sixel(10, 20, 2) + at(2, 0) + sixel(10, 20, 3));
        CHECK(paints_right(pp, e, p));
        CHECK(ink(carried(p, 7, 20)) == 3 * CW * CH);
        e.feed(at(3, 0) + sixel(10, 20, 4));                   // the oldest is forgotten
        CHECK(paints_right(pp, e, p));
        CHECK(ink(carried(p, 7, 20)) == 3 * CW * CH);
        CHECK(p.images.size() == 1 && p.images[0].row == 1);
    }

    // ---- the whole thing: random screens against what the emulator says, painted again and again
    {
        std::mt19937 rng(99);
        auto n = [&](int lo, int hi) { return lo + (int)(rng() % (unsigned)(hi - lo + 1)); };
        for (int run = 0; run < 30; run++) {
            int rows = n(4, 10), cols = n(6, 30);
            Emulator e = make(rows, cols, (size_t)n(0, 30));
            if (n(0, 3) == 0) e.set_picture_budget((size_t)n(2000, 20000));
            Picture p = make_pic(rows, cols);
            PanePainter pp;
            int seed = 0;
            size_t back = 0;
            for (int step = 0; step < 120; step++) {
                switch (n(0, 14)) {
                    case 0: case 1: case 2: case 3: e.feed(at(n(0, rows - 1), n(0, cols - 1)) + sixel(n(1, 70), n(1, 80), ++seed)); break;
                    case 4: case 5: { std::string s; for (int i = n(1, 15); i > 0; i--) s += (char)('a' + n(0, 25)); e.feed(s); break; }
                    case 6: e.feed("\n\n"); break;
                    case 7: e.feed(at(n(0, rows - 1), 0) + "\x1b[" + std::to_string(n(0, 2)) + "K"); break;
                    case 8: e.feed("\x1b[" + std::to_string(n(0, 3)) + "J"); break;
                    case 9: e.feed(at(n(0, rows - 1), n(0, cols - 1)) + "\x1b[" + std::to_string(n(1, 5)) + (n(0, 1) ? "@" : "P")); break;
                    case 10: e.feed("\x1b[" + std::to_string(n(1, 3)) + (n(0, 1) ? "L" : "M")); break;
                    case 11: e.feed(n(0, 1) ? "\x1b[?1049h" : "\x1b[?1049l"); break;
                    case 12: { rows = n(4, 10); cols = n(6, 30); e.resize(rows, cols); p = make_pic(rows, cols); break; }
                    case 13: back = (size_t)n(0, 12); break;
                    default: back = 0; break;
                }
                const size_t use = e.alt_screen() ? 0 : back;
                if (!paints_right(pp, e, p, use, n(0, 1) != 0)) {
                    std::printf("   (run %d, step %d, scrolled back %zu)\n", run, step, use);
                    CHECK(false);
                    goto done;
                }
                if (step % 20 == 19) {                          // a painter that never saw the earlier screens makes the same pictures
                    PanePainter fresh;
                    Picture q = make_pic(rows, cols);
                    CHECK(paints_right(fresh, e, q, use));
                }
            }
        }
    }
done:

    if (failures == 0) std::printf("all painter picture tests passed\n");
    return failures == 0 ? 0 : 1;
}
