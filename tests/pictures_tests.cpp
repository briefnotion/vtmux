// Tests for the picture layer: a Sixel picture sent to the emulator becomes tiles, one for each cell, that follow the text.
// sources: ../src/vtparser.cpp ../src/emulator.cpp ../src/width.cpp
#include <algorithm>
#include <random>
#include <string>

#include "check.h"
#include "../src/emulator.h"
#include "../src/width.h"
#include "../vtscreen/vtscreen.h"

using namespace vtmux;

namespace {

const int CW = 10, CH = 20;                // the cell of the pretend real terminal, in pixels

// A Sixel picture of this size (all of its pixels are the background colour, but it has a size, and that is what counts here).
std::string sixel(int w, int h, const std::string& params = "") {
    return "\x1bP" + params + "q\"1;1;" + std::to_string(w) + ";" + std::to_string(h) + "~\x1b\\";
}

Emulator make(int rows, int cols, size_t scrollback = 100) {
    Emulator e(rows, cols, scrollback);
    e.set_graphics(CW, CH, true);
    return e;
}

std::string at(int row, int col) { return "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H"; }

Tile tile(const Emulator& e, int row, int col) { return e.picture_tile(row, col); }

// the tile is piece (tc, tr) of some picture; says which picture
bool is(const Emulator& e, int row, int col, int tc, int tr, uint32_t* id = nullptr) {
    const Tile t = tile(e, row, col);
    if (t.empty() || t.col != tc || t.row != tr) return false;
    if (id) *id = t.image;
    return true;
}

int count_tiles(const Emulator& e) {
    int n = 0;
    for (int r = 0; r < e.rows(); r++) for (int c = 0; c < e.cols(); c++) if (!tile(e, r, c).empty()) n++;
    return n;
}

int history_tiles(const Emulator& e) {
    int n = 0;
    for (size_t i = 0; i < e.scrollback_size(); i++) for (const Tile& t : e.scrollback_tiles(i)) if (!t.empty()) n++;
    return n;
}

// ---- the oracle: the same bytes into a second emulator where a picture is written as letters (one private-use letter for each
// tile). What the emulator does to letters (wipe, erase, shift, scroll, scrollback) is what the picture layer must do to tiles.
char32_t marker(int id, int tc, int tr) { return (char32_t)(0x100000 + (id * 16 + tc) * 16 + tr); }

std::string utf8(char32_t cp) {
    std::string s;
    s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    return s;
}

std::string cha(int col) { return "\x1b[" + std::to_string(col + 1) + "G"; }

struct Pair {
    Emulator a, b;                         // a has the pictures, b has the letters
    int pictures = 0;
    bool right = false;                    // mode 8452
    Pair(int rows, int cols, size_t scrollback) : a(rows, cols, scrollback), b(rows, cols, scrollback) {
        a.set_graphics(CW, CH, true);
        a.set_picture_budget((size_t)1 << 30);
    }
    void feed(const std::string& s) { a.feed(s); b.feed(s); }
    void picture(int w, int h) {
        const int tw = (w + CW - 1) / CW, th = (h + CH - 1) / CH;
        const int id = ++pictures;
        a.feed(sixel(w, h));
        const int cols = b.cols(), col0 = b.cursor().col;
        std::string s = cha(col0);
        for (int tr = 0; tr < th; tr++) {
            for (int tc = 0; tc < tw && col0 + tc < cols; tc++) s += utf8(marker(id, tc, tr));
            if (tr + 1 < th || !right) s += "\n" + cha(col0);
        }
        if (right) s += cha(std::min(col0 + tw, cols - 1));
        b.feed(s);
    }
};

// Does the picture layer of a say what the letters of b say?
void compare(const Pair& p, int step) {
    const Emulator &a = p.a, &b = p.b;
    if (a.cursor() != b.cursor() || a.rows() != b.rows() || a.cols() != b.cols() || a.alt_screen() != b.alt_screen()) {
        std::printf("   (step %d: the two emulators went different ways)\n", step);
        CHECK(false);
        return;
    }
    int bad = 0;
    for (int r = 0; r < a.rows(); r++) {
        for (int c = 0; c < a.cols(); c++) {
            const Tile t = a.picture_tile(r, c);
            const char32_t ch = b.cell(r, c).ch;
            if (ch >= 0x100000) {
                const int v = (int)(ch - 0x100000);
                if (t.empty() || t.image != (uint32_t)(v / 256) || t.col != (v / 16) % 16 || t.row != v % 16) bad++;
            } else if (!t.empty()) bad++;
        }
    }
    if (a.scrollback_size() != b.scrollback_size()) bad++;
    else {
        for (size_t i = 0; i < a.scrollback_size(); i++) {
            const std::vector<Tile>& tiles = a.scrollback_tiles(i);
            const std::vector<Cell>& line = b.scrollback_line(i);
            if (!tiles.empty() && tiles.size() != line.size()) { bad++; continue; }
            for (size_t c = 0; c < line.size(); c++) {
                const char32_t ch = line[c].ch;
                const Tile t = tiles.empty() ? Tile() : tiles[c];
                if (ch >= 0x100000) {
                    const int v = (int)(ch - 0x100000);
                    if (t.empty() || t.image != (uint32_t)(v / 256) || t.col != (v / 16) % 16 || t.row != v % 16) bad++;
                } else if (!t.empty()) bad++;
            }
        }
    }
    if (bad) std::printf("   (step %d: %d tiles differ from the letters)\n", step, bad);
    CHECK(bad == 0);
}

std::string random_op(std::mt19937& rng, Pair& p) {
    auto n = [&](int lo, int hi) { return lo + (int)(rng() % (unsigned)(hi - lo + 1)); };
    auto num = [&](int hi) { return std::to_string(n(1, hi)); };
    switch (rng() % 21) {
        case 0: case 1: case 2: { std::string s; const int k = n(1, 25); for (int i = 0; i < k; i++) s += (char)('a' + n(0, 25)); return s; }
        case 3: return "\r";
        case 4: return "\n";
        case 5: return "\x1bM";
        case 6: return "\x1b[" + num(10) + ";" + num(25) + "H";
        case 7: return "\x1b[" + num(4) + "ABCD"[n(0, 3)];
        case 8: return "\x1b[" + std::to_string(n(0, 3)) + "J";
        case 9: return "\x1b[" + std::to_string(n(0, 2)) + "K";
        case 10: return "\x1b[" + num(6) + "X";
        case 11: return "\x1b[" + num(6) + "@";
        case 12: return "\x1b[" + num(6) + "P";
        case 13: return "\x1b[" + num(4) + "L";
        case 14: return "\x1b[" + num(4) + "M";
        case 15: return "\x1b[" + num(4) + (n(0, 1) ? "S" : "T");
        case 16: { const int t = n(1, 6); return "\x1b[" + std::to_string(t) + ";" + std::to_string(t + n(0, 6)) + "r"; }
        case 17: { const char* m[] = {"\x1b[?1049h", "\x1b[?1049l", "\x1b[?47h", "\x1b[?47l", "\x1b[?1047h", "\x1b[?1047l"}; return m[n(0, 5)]; }
        case 18: return "\x1b[" + std::to_string(n(0, 3)) + "J";
        case 19: if (n(0, 6) == 0) return "\x1b#8"; return "\x1b[r";
        case 20: { p.right = n(0, 1) != 0; return p.right ? "\x1b[?8452h" : "\x1b[?8452l"; }
        default: return "";                                 // a picture (below)
    }
}

}  // namespace

int main() {
    // the letters used by the oracle are one cell wide
    CHECK_EQ(char_width(marker(1, 2, 3)), 1);
    CHECK_EQ(char_width(marker(200, 15, 15)), 1);

    // ---- a picture is cut into tiles, one for each cell it covers
    {
        Emulator e = make(8, 20);
        CHECK(!e.has_pictures() && count_tiles(e) == 0);
        e.feed(sixel(25, 50));                                // 3 cells wide, 3 high (a part of a cell is a cell)
        uint32_t id = 0;
        CHECK(is(e, 0, 0, 0, 0, &id) && id != 0);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { uint32_t i2 = 0; CHECK(is(e, r, c, c, r, &i2) && i2 == id); }
        CHECK(tile(e, 0, 3).empty() && tile(e, 3, 0).empty());
        CHECK_EQ(count_tiles(e), 9);
        CHECK(e.has_pictures());
        CHECK_EQ(e.cursor().row, 3);                          // the cursor is below the picture ...
        CHECK_EQ(e.cursor().col, 0);                          // ... where it began
        CHECK(e.picture(id) != nullptr);
        CHECK(e.picture(id)->w == 25 && e.picture(id)->h == 50);
        CHECK_EQ(e.picture_count(), (size_t)1);
        CHECK_EQ(e.picture_bytes(), (size_t)(25 * 50 * 4));
        CHECK(e.any_dirty());
        // the exact size of a cell is one tile
        Emulator f = make(8, 20);
        f.feed(sixel(10, 20));
        CHECK_EQ(count_tiles(f), 1);
        f.feed(at(2, 0) + sixel(11, 21));
        CHECK_EQ(count_tiles(f), 1 + 4);
    }

    // ---- what the picture holds: P2 says whether what is not drawn is the background or transparent
    {
        Emulator e = make(4, 10);
        e.feed("\x1bP0;0q\"1;1;20;20#1;2;100;0;0#1~\x1b\\");
        const uint32_t a = tile(e, 0, 0).image;
        e.feed("\x1bP0;1q\"1;1;20;20#1;2;100;0;0#1~\x1b\\");
        const uint32_t b = tile(e, 1, 0).image;
        CHECK(a != 0 && b != 0 && a != b);
        CHECK(e.picture(a)->at(0, 0).r == 255 && e.picture(a)->at(0, 0).a == 255);           // the red that was drawn
        CHECK(e.picture(a)->at(15, 15).a == 255);                                            // the rest is the background
        CHECK(e.picture(b)->at(0, 0).a == 255 && e.picture(b)->at(15, 15).a == 0);           // or nothing at all
        CHECK(e.picture(a)->w == 20 && e.picture(a)->h == 20);
    }

    // ---- where it starts, and where the cursor ends
    {
        Emulator e = make(8, 20);
        e.feed(at(2, 5) + sixel(25, 40));
        CHECK(is(e, 2, 5, 0, 0) && is(e, 3, 7, 2, 1));
        CHECK(tile(e, 2, 4).empty() && tile(e, 2, 8).empty() && tile(e, 4, 5).empty());
        CHECK(e.cursor().row == 4 && e.cursor().col == 5);
        e.feed("x");                                          // the next text goes under the picture, in its column
        CHECK_EQ(e.cell(4, 5).ch, (char32_t)'x');
        // cut off at the right edge
        Emulator f = make(8, 20);
        f.feed(at(0, 18) + sixel(30, 20));
        CHECK(is(f, 0, 18, 0, 0) && is(f, 0, 19, 1, 0));
        CHECK_EQ(count_tiles(f), 2);
        CHECK(f.cursor().row == 1 && f.cursor().col == 18);
        // a pending wrap at the last column is not a reason to go to the next line
        Emulator g = make(8, 20);
        g.feed(at(0, 19) + "z" + sixel(10, 20));
        CHECK(is(g, 0, 19, 0, 0));
        CHECK(g.cursor().row == 1 && g.cursor().col == 19);
        g.feed("y");                                          // (and the pending wrap is gone: y does not wrap to the next line again)
        CHECK(g.cell(1, 19).ch == (char32_t)'y');
        // mode 8452: the cursor stays by the last row, right of the picture
        Emulator h = make(8, 20);
        h.feed("\x1b[?8452h" + at(1, 3) + sixel(25, 40));
        CHECK(h.sixel_cursor_right());
        CHECK(h.cursor().row == 2 && h.cursor().col == 6);
        h.feed(at(0, 15) + sixel(100, 20));                   // too wide: the cursor stops at the last column
        CHECK(h.cursor().row == 0 && h.cursor().col == 19);
        h.feed("\x1b[?8452l" + at(0, 0) + sixel(10, 20));
        CHECK(h.cursor().row == 1 && h.cursor().col == 0);
        // a pending wrap does not make a picture of this kind start on the next line, nor the next letter
        Emulator k = make(8, 20);
        k.feed("\x1b[?8452h" + at(0, 19) + "z" + sixel(10, 20) + "y");
        CHECK(k.cell(0, 19).ch == (char32_t)'y' && k.cursor().row == 0 && k.cell(1, 0).ch == U' ');     // y is on the same row (and wiped the piece)
        CHECK(tile(k, 0, 19).empty());
    }

    // ---- a letter written over a cell wipes that cell's piece, and only that one
    {
        Emulator e = make(8, 20);
        e.feed(sixel(30, 60));
        e.feed(at(1, 1) + "x");
        CHECK(tile(e, 1, 1).empty());
        CHECK(is(e, 1, 0, 0, 1) && is(e, 1, 2, 2, 1) && is(e, 0, 1, 1, 0) && is(e, 2, 1, 1, 2));
        CHECK_EQ(count_tiles(e), 8);
        e.feed(at(0, 0) + "ab");                              // two letters, two cells
        CHECK(tile(e, 0, 0).empty() && tile(e, 0, 1).empty() && is(e, 0, 2, 2, 0));
        e.feed(at(2, 2) + "\x1b[?25l");                       // other things do not wipe
        CHECK(is(e, 2, 2, 2, 2));
        e.feed(at(2, 2) + " ");                               // a space is a letter
        CHECK(tile(e, 2, 2).empty());
        e.feed("\x1b[31m" + at(2, 1) + "\x1b[0m");            // a colour change alone does not
        CHECK(is(e, 2, 1, 1, 2));
    }
    {
        // wrapping text writes over the cells it passes
        Emulator f = make(8, 10);
        f.feed(at(0, 8) + sixel(20, 40));
        CHECK_EQ(count_tiles(f), 4);                          // 2 wide, 2 high, at the edge: all fit
        f.feed(at(0, 8) + "abcd");                            // a and b on row 0, c and d wrap to the start of row 1
        CHECK(tile(f, 0, 8).empty() && tile(f, 0, 9).empty());
        CHECK_EQ(count_tiles(f), 2);                          // the pieces on row 1 are not touched by the c and d, which are at columns 0 and 1
        CHECK(is(f, 1, 8, 0, 1) && is(f, 1, 9, 1, 1));
    }

    // ---- erasing wipes the pieces of the erased cells
    {
        Emulator e = make(8, 20);
        e.feed(sixel(50, 40));
        e.feed(at(0, 2) + "\x1b[2K");                         // EL 2: row 0
        CHECK(tile(e, 0, 0).empty() && tile(e, 0, 4).empty() && is(e, 1, 0, 0, 1));
        e.feed(at(1, 1) + "\x1b[K");                          // EL 0: from the cursor to the end
        CHECK(is(e, 1, 0, 0, 1) && tile(e, 1, 1).empty() && tile(e, 1, 4).empty());
        e.feed(at(2, 0) + sixel(50, 40));
        e.feed(at(2, 2) + "\x1b[2X");                         // ECH: two cells
        CHECK(is(e, 2, 1, 1, 0) && tile(e, 2, 2).empty() && tile(e, 2, 3).empty() && is(e, 2, 4, 4, 0));
        e.feed(at(3, 2) + "\x1b[1J");                         // ED 1: everything above and up to the cursor
        CHECK(tile(e, 0, 0).empty() && tile(e, 3, 2).empty() && tile(e, 3, 3).empty() == false);
        e.feed("\x1b[J");                                     // ED 0: from the cursor on
        CHECK(tile(e, 3, 3).empty() && tile(e, 4, 0).empty());
        e.feed(sixel(50, 40));
        e.feed("\x1b[2J");                                    // ED 2: the lot
        CHECK_EQ(count_tiles(e), 0);
        CHECK(!e.has_pictures());                             // (and the layer lets go of its memory)
    }

    // ---- insert and delete characters shift the pieces with the cells
    {
        Emulator e = make(4, 12);
        e.feed(sixel(40, 20));                                // row 0, columns 0 to 3, pieces 0 to 3
        e.feed(at(0, 1) + "\x1b[2@");                         // two blanks at column 1: the rest moves right
        CHECK(is(e, 0, 0, 0, 0) && tile(e, 0, 1).empty() && tile(e, 0, 2).empty() && is(e, 0, 3, 1, 0) && is(e, 0, 4, 2, 0) && is(e, 0, 5, 3, 0));
        e.feed(at(0, 1) + "\x1b[2P");                         // and back
        CHECK(is(e, 0, 0, 0, 0) && is(e, 0, 1, 1, 0) && is(e, 0, 2, 2, 0) && is(e, 0, 3, 3, 0) && tile(e, 0, 4).empty());
        e.feed(at(0, 0) + "\x1b[1P");                         // delete the first: they all move left
        CHECK(is(e, 0, 0, 1, 0) && is(e, 0, 2, 3, 0) && tile(e, 0, 3).empty());
        // insert mode: the letter wipes its own cell, and what was there moves right with its piece
        Emulator f = make(4, 12);
        f.feed(sixel(30, 20));
        f.feed("\x1b[4h" + at(0, 1) + "x");
        CHECK(is(f, 0, 0, 0, 0) && tile(f, 0, 1).empty() && is(f, 0, 2, 1, 0) && is(f, 0, 3, 2, 0));
        // pushed off the right edge
        Emulator g = make(4, 6);
        g.feed(at(0, 3) + sixel(30, 20));
        g.feed(at(0, 0) + "\x1b[1@");
        CHECK(tile(g, 0, 3).empty() && is(g, 0, 4, 0, 0) && is(g, 0, 5, 1, 0));    // (the last piece is pushed off the edge)
        CHECK_EQ(count_tiles(g), 2);
    }

    // ---- wide letters: a piece goes with the cell, and the other half of a broken wide letter loses its piece too
    {
        Emulator e = make(4, 10);
        e.feed(sixel(60, 20));
        e.feed(at(0, 2) + "\xE4\xB8\xAD");                    // a wide letter on columns 2 and 3
        CHECK(tile(e, 0, 2).empty() && tile(e, 0, 3).empty() && is(e, 0, 1, 1, 0) && is(e, 0, 4, 4, 0));
        // pieces put on a wide letter, then half of it is written over: both cells lose theirs
        Emulator f = make(4, 10);
        f.feed(at(0, 2) + "\xE4\xB8\xAD");
        f.feed(at(0, 0) + sixel(60, 20));
        CHECK(is(f, 0, 2, 2, 0) && is(f, 0, 3, 3, 0));
        f.feed(at(0, 3) + "x");                               // breaks the wide letter: its first half is blanked
        CHECK(tile(f, 0, 2).empty() && tile(f, 0, 3).empty() && is(f, 0, 1, 1, 0) && is(f, 0, 4, 4, 0));
        // erasing half of one
        Emulator g = make(4, 10);
        g.feed(at(0, 2) + "\xE4\xB8\xAD");
        g.feed(at(0, 0) + sixel(60, 20));
        g.feed(at(0, 3) + "\x1b[X");                          // ECH on the second half
        CHECK(tile(g, 0, 2).empty() && tile(g, 0, 3).empty() && is(g, 0, 4, 4, 0));
        Emulator h = make(4, 10);
        h.feed(at(0, 2) + "\xE4\xB8\xAD");
        h.feed(at(0, 0) + sixel(60, 20));
        h.feed(at(0, 2) + "\x1b[X");                          // ECH on the first half
        CHECK(tile(h, 0, 2).empty() && tile(h, 0, 3).empty() && is(h, 0, 1, 1, 0) && is(h, 0, 4, 4, 0));
        // delete characters through the middle of one
        Emulator k = make(4, 10);
        k.feed(at(0, 2) + "\xE4\xB8\xAD");
        k.feed(at(0, 0) + sixel(60, 20));
        k.feed(at(0, 0) + "\x1b[3P");                         // takes columns 0 to 2: the second half is left alone, then blanked
        CHECK(k.cell(0, 0).ch == U' ');                       // (the cell of column 3 was blanked as half a letter, then moved to column 0, with its piece)
        CHECK(tile(k, 0, 0).empty() && is(k, 0, 1, 4, 0) && is(k, 0, 2, 5, 0));
    }

    {
        // writing over the first half of a wide letter: the other half is blanked, with its piece
        Emulator e = make(4, 10);
        e.feed(at(0, 2) + "\xE4\xB8\xAD" + at(0, 0) + sixel(60, 20));
        e.feed(at(0, 2) + "x");
        CHECK(tile(e, 0, 2).empty() && tile(e, 0, 3).empty() && is(e, 0, 1, 1, 0) && is(e, 0, 4, 4, 0));
        // insert characters at the second half
        Emulator f = make(4, 10);
        f.feed(at(0, 2) + "\xE4\xB8\xAD" + at(0, 0) + sixel(60, 20));
        f.feed(at(0, 3) + "\x1b[1@");
        CHECK(is(f, 0, 1, 1, 0) && tile(f, 0, 2).empty() && tile(f, 0, 3).empty() && tile(f, 0, 4).empty() && is(f, 0, 5, 4, 0));
        // a wide letter pushed so that half of it is off the edge is blanked
        Emulator g = make(4, 6);
        g.feed(at(0, 4) + "\xE4\xB8\xAD" + at(0, 0) + sixel(60, 20));
        g.feed(at(0, 3) + "\x1b[1@");
        CHECK(tile(g, 0, 3).empty() && is(g, 0, 4, 3, 0) && tile(g, 0, 5).empty());
        CHECK(g.cell(0, 5).ch == U' ');
        // delete characters at the second half
        Emulator h = make(4, 10);
        h.feed(at(0, 2) + "\xE4\xB8\xAD" + at(0, 0) + sixel(60, 20));
        h.feed(at(0, 3) + "\x1b[1P");
        CHECK(is(h, 0, 1, 1, 0) && tile(h, 0, 2).empty() && is(h, 0, 3, 4, 0) && is(h, 0, 4, 5, 0) && tile(h, 0, 5).empty());
    }

    // ---- scrolling: the picture scrolls with the text
    {
        Emulator e = make(6, 12);
        e.feed(at(0, 0) + sixel(20, 40));
        e.feed(at(5, 0) + "\n");                              // the screen scrolls one line
        CHECK(is(e, 0, 0, 0, 1) && is(e, 0, 1, 1, 1));        // the second row of the picture is the first row now
        CHECK(tile(e, 1, 0).empty());
        CHECK_EQ(e.scrollback_size(), (size_t)1);
        CHECK_EQ(history_tiles(e), 2);                        // the first row of the picture is in the scrollback ...
        CHECK(e.scrollback_tiles(0).size() == 12);            // ... a tile for each column of the line
        CHECK(e.scrollback_tiles(0)[0].row == 0 && e.scrollback_tiles(0)[1].col == 1);
        CHECK(e.scrollback_tiles(0)[0].image == tile(e, 0, 0).image);
        e.feed("\n");
        CHECK_EQ(history_tiles(e), 4);                        // both rows of it
        CHECK_EQ(count_tiles(e), 0);
        CHECK(e.scrollback_tiles(1)[0].row == 1);
        CHECK(e.picture(e.scrollback_tiles(1)[0].image) != nullptr);   // (and the picture is still held)
        // a line without pieces has none: the empty list
        Emulator f = make(3, 5);
        f.feed("a\nb\nc\nd\ne");
        CHECK_EQ(f.scrollback_size(), (size_t)2);
        CHECK(f.scrollback_tiles(0).empty() && f.scrollback_tiles(1).empty());
        // a line without pieces is empty even when other lines of the screen have some
        Emulator k = make(6, 12);
        k.feed(at(3, 0) + sixel(10, 20) + at(5, 0) + "\n");
        CHECK(k.scrollback_size() == 1 && k.scrollback_tiles(0).empty());
        CHECK(is(k, 2, 0, 0, 0));
        // reverse index scrolls down: pieces move down and fall off the bottom
        Emulator g = make(4, 5);
        g.feed(at(1, 0) + sixel(10, 40));                     // rows 1 and 2
        g.feed(at(0, 0) + "\x1bM");
        CHECK(tile(g, 1, 0).empty() && is(g, 2, 0, 0, 0) && is(g, 3, 0, 0, 1));
        g.feed("\x1bM");
        CHECK(tile(g, 2, 0).empty() && is(g, 3, 0, 0, 0));
        CHECK_EQ(count_tiles(g), 1);
        CHECK_EQ(g.scrollback_size(), (size_t)0);
        // a scroll region: only the lines in it
        Emulator h = make(6, 5);
        h.feed("\x1b[2;4r" + at(0, 0) + sixel(10, 120));      // a tall picture over every row ... then rows 0 to 5
        // (rows 0 and 1 are above the region, 2 and 3 in it, 4 and 5 below it: the linefeeds scroll inside the region)
        CHECK_EQ(h.scrollback_size(), (size_t)0);             // nothing leaves through the top: the region does not start there
        CHECK(is(h, 0, 0, 0, 0) && is(h, 1, 0, 0, 4) && is(h, 2, 0, 0, 5) && tile(h, 3, 0).empty());
        CHECK(h.cursor().row == 3);
    }

    // ---- a picture that scrolls the screen on its way down: the cursor ends below it
    {
        Emulator e = make(6, 12);
        e.feed(at(4, 0) + sixel(10, 60));                     // three rows, but there are two left
        CHECK(is(e, 2, 0, 0, 0) && is(e, 3, 0, 0, 1) && is(e, 4, 0, 0, 2));
        CHECK_EQ(e.scrollback_size(), (size_t)2);
        CHECK(e.cursor().row == 5 && e.cursor().col == 0);    // one row below the picture, and still on the screen
        CHECK_EQ(count_tiles(e), 3);
        // taller than the screen: the top goes to the scrollback
        Emulator f = make(3, 12);
        f.feed(sixel(10, 100));                               // five rows
        CHECK(is(f, 0, 0, 0, 3) && is(f, 1, 0, 0, 4) && tile(f, 2, 0).empty());   // (the cursor needs the last row: one more line scrolled off)
        CHECK_EQ(f.scrollback_size(), (size_t)3);
        CHECK(f.scrollback_tiles(0)[0].row == 0 && f.scrollback_tiles(1)[0].row == 1 && f.scrollback_tiles(2)[0].row == 2);
        CHECK(f.cursor().row == 2);
        // as 8452: the last row is the last row of the picture
        Emulator g = make(3, 12);
        g.feed("\x1b[?8452h" + at(2, 0) + sixel(10, 60));
        CHECK(is(g, 2, 0, 0, 2) && is(g, 1, 0, 0, 1) && is(g, 0, 0, 0, 0));
        CHECK(g.cursor().row == 2 && g.cursor().col == 1);
    }

    // ---- mode 80: from the top left, no scrolling, cut off at the bottom, the cursor stays
    {
        Emulator e = make(4, 10);
        e.feed("\x1b[?80h" + at(2, 3) + sixel(30, 120));      // six rows of tiles, four rows on the screen
        CHECK(e.sixel_display_mode());
        CHECK(is(e, 0, 0, 0, 0) && is(e, 3, 2, 2, 3));
        CHECK_EQ(count_tiles(e), 3 * 4);
        CHECK(e.cursor().row == 2 && e.cursor().col == 3);
        CHECK_EQ(e.scrollback_size(), (size_t)0);
        e.feed("\x1b[?80l" + at(0, 0) + sixel(10, 20));
        CHECK(!e.sixel_display_mode());
        CHECK(e.cursor().row == 1);
        // (the cursor is not touched, a pending wrap included)
        Emulator f = make(4, 10);
        f.feed("\x1b[?80h" + at(1, 9) + "z" + sixel(10, 20) + "y");
        CHECK(f.cell(2, 0).ch == (char32_t)'y' && f.cursor().row == 2);
    }

    // ---- the alternate screen has pictures of its own
    {
        Emulator e = make(4, 10);
        e.feed(sixel(20, 20));
        CHECK_EQ(count_tiles(e), 2);
        e.feed("\x1b[?1049h");
        CHECK(!e.has_pictures() && count_tiles(e) == 0);
        e.feed(sixel(30, 20));
        CHECK_EQ(count_tiles(e), 3);
        e.feed("\x1b[?1049l");
        CHECK_EQ(count_tiles(e), 2);                          // the main screen's own again
        e.feed("\x1b[?1049h");
        CHECK_EQ(count_tiles(e), 0);                          // and the alternate screen starts clear
        // scrolling on the alternate screen leaves nothing in the scrollback
        e.feed(sixel(10, 40) + "\n\n\n\n\n\n");
        CHECK_EQ(e.scrollback_size(), (size_t)0);
        CHECK_EQ(history_tiles(e), 0);
    }

    // ---- the scrollback: wiped by ED 3, and trimmed with the lines
    {
        Emulator e = make(3, 6, 5);
        e.feed(sixel(60, 60));
        e.feed("\n\n\n");
        CHECK(e.scrollback_size() > 0 && history_tiles(e) > 0);
        e.feed("\x1b[3J");
        CHECK_EQ(e.scrollback_size(), (size_t)0);
        // the limit: the oldest lines go, and their pieces with them
        Emulator f = make(3, 6, 4);
        f.feed(sixel(10, 20));                                // one piece
        for (int i = 0; i < 3; i++) f.feed("\n");             // it scrolls to history line 0
        CHECK_EQ(history_tiles(f), 1);
        for (int i = 0; i < 2; i++) f.feed("\n");
        CHECK_EQ(f.scrollback_size(), (size_t)4);             // full
        CHECK_EQ(history_tiles(f), 1);
        f.feed("\n");                                         // the line with the piece is the oldest, and goes
        CHECK_EQ(f.scrollback_size(), (size_t)4);
        CHECK_EQ(history_tiles(f), 0);
        // no scrollback at all
        Emulator g = make(3, 6, 0);
        g.feed(sixel(10, 100));
        CHECK_EQ(g.scrollback_size(), (size_t)0);
        CHECK_EQ(count_tiles(g), 2);                          // (the last two rows; the cursor has the row below)
    }

    // ---- a reset
    {
        Emulator e = make(4, 10);
        e.feed("\x1b[?80h\x1b[?8452h" + sixel(30, 40));
        e.feed("\n\n\n\n\n");
        const size_t kept = e.scrollback_size();
        e.feed("\x1b" "c");                                   // full reset: the screens are cleared, and the modes
        CHECK_EQ(count_tiles(e), 0);
        CHECK(!e.sixel_display_mode() && !e.sixel_cursor_right());
        CHECK_EQ(e.scrollback_size(), kept);                  // (the scrollback is as the cells have it: kept)
        e.feed(sixel(10, 20));
        e.feed("\x1b#8");                                     // the test pattern writes every cell
        CHECK_EQ(count_tiles(e), 0);
        // soft reset leaves pictures and the cell contents alone
        e.feed(sixel(10, 20));
        e.feed("\x1b[!p");
        CHECK_EQ(count_tiles(e), 1);
    }

    // ---- a resize: the pieces keep to their cells, cut at the edges; what scrolls off goes to the scrollback with them
    {
        Emulator e = make(6, 10);
        e.feed(sixel(60, 60));
        e.resize(6, 4);
        CHECK(is(e, 0, 3, 3, 0));
        CHECK_EQ(count_tiles(e), 4 * 3);
        e.resize(8, 12);
        CHECK(is(e, 2, 3, 3, 2) && tile(e, 2, 4).empty() && tile(e, 3, 0).empty());
        CHECK_EQ(count_tiles(e), 4 * 3);
        Emulator f = make(6, 10);
        f.feed(sixel(20, 60) + "xyz");                        // the cursor is on row 3
        f.resize(3, 10);                                      // the cursor would be off the bottom: a line scrolls away
        CHECK_EQ(f.scrollback_size(), (size_t)1);
        CHECK(f.scrollback_tiles(0)[0].row == 0);
        CHECK(is(f, 0, 0, 0, 1) && is(f, 1, 0, 0, 2));
        // a wide letter cut by the edge
        Emulator g = make(3, 10);
        g.feed(at(0, 4) + "\xE4\xB8\xAD" + at(0, 0) + sixel(60, 20));
        g.resize(3, 5);
        CHECK(tile(g, 0, 4).empty());                         // the half that is left is blanked, and loses its piece
        CHECK(is(g, 0, 3, 3, 0));
    }

    // ---- Sixel off, nothing to draw, too big to keep
    {
        Emulator e(6, 20, 10);                                // nothing known about the real terminal
        e.feed(sixel(20, 20));
        CHECK_EQ(count_tiles(e), 0);
        CHECK(e.cursor().row == 0 && e.cursor().col == 0);    // not a trace
        e.set_graphics(10, 20, false);                        // the cell is known, but not Sixel
        e.feed(sixel(20, 20));
        CHECK(count_tiles(e) == 0 && e.cursor().row == 0);
        e.set_graphics(10, 20, true);
        e.feed("\x1bPq\x1b\\");                               // no picture in it
        e.feed("\x1bP1;2;3|data\x1b\\");                      // another kind of string
        e.feed("\x1bP+q4d73\x1b\\");
        CHECK(count_tiles(e) == 0 && e.cursor().row == 0 && e.cursor().col == 0);
        e.feed(sixel(20, 20));
        CHECK_EQ(count_tiles(e), 2);
        // the cell changed size: the pictures were cut for the old cells
        e.set_graphics(10, 20, true);                         // (the same: nothing changes)
        CHECK_EQ(count_tiles(e), 2);
        e.set_graphics(8, 16, true);
        CHECK_EQ(count_tiles(e), 0);
        CHECK_EQ(e.picture_count(), (size_t)0);
        // history lines stay in step
        Emulator f = make(2, 5, 10);
        f.feed(sixel(10, 60));
        CHECK(history_tiles(f) > 0);
        const size_t lines = f.scrollback_size();
        f.set_graphics(7, 7, true);
        CHECK_EQ(history_tiles(f), 0);
        CHECK_EQ(f.scrollback_size(), lines);
        f.feed("\n\n\n\n\n\n");                               // (and the scrolling goes on as before)
        f.feed(sixel(7, 14));                                 // two rows on a screen of two: the top one scrolls off
        CHECK(count_tiles(f) == 1 && is(f, 0, 0, 0, 1));

        // a picture too big to keep is not shown, and takes its place all the same
        Emulator g = make(8, 10);
        g.set_picture_budget(1000);
        g.feed(sixel(25, 50));                                // 5000 bytes
        CHECK(count_tiles(g) == 0 && g.picture_count() == 0);
        CHECK(g.cursor().row == 3 && g.cursor().col == 0);
        g.feed(sixel(10, 20));                                // 800 bytes: fine
        CHECK(count_tiles(g) == 1 && g.picture_count() == 1);
        // (placing over an older picture that was kept: the cells lose their pieces)
        Emulator h = make(8, 10);
        h.feed(sixel(20, 20));
        h.set_picture_budget(100);
        h.feed(at(0, 0) + sixel(20, 20));
        CHECK_EQ(count_tiles(h), 0);
    }

    // ---- the budget: what nothing shows any more goes first, then the oldest
    {
        Emulator e = make(12, 10);
        e.set_picture_budget(2500);                           // three pictures of 800 bytes
        uint32_t id[6] = {0};
        for (int i = 0; i < 3; i++) { e.feed(at(i, 0) + sixel(10, 20)); id[i] = tile(e, i, 0).image; }
        CHECK(e.picture_count() == 3 && e.picture_bytes() == 2400);
        e.feed(at(3, 0) + sixel(10, 20));                     // a fourth: all are shown, so the oldest goes
        id[3] = tile(e, 3, 0).image;
        CHECK(e.picture_count() == 3 && e.picture_bytes() == 2400);
        CHECK(e.picture(id[0]) == nullptr && tile(e, 0, 0).empty());                   // its tile is gone with it
        CHECK(e.picture(id[1]) != nullptr && is(e, 1, 0, 0, 0) && is(e, 2, 0, 0, 0) && is(e, 3, 0, 0, 0));
        // one nothing shows goes before an older one that is shown
        e.feed(at(1, 0) + "x");                               // the picture on row 1 is on screen no more
        e.feed(at(4, 0) + sixel(10, 20));
        CHECK(e.picture(id[1]) == nullptr);
        CHECK(e.picture(id[2]) != nullptr && e.picture(id[3]) != nullptr);
        CHECK_EQ(e.picture_count(), (size_t)3);
        // a picture in the scrollback is shown too
        Emulator f = make(2, 10, 50);
        f.set_picture_budget(2500);
        f.feed(sixel(10, 20));
        const uint32_t first = tile(f, 0, 0).image;
        f.feed("\n\n\n\n");                                   // it is in the scrollback now
        CHECK_EQ(history_tiles(f), 1);
        for (int i = 0; i < 2; i++) f.feed(sixel(10, 20));
        CHECK(f.picture(first) != nullptr);                   // still there: three fit
        f.feed(sixel(10, 20));
        CHECK(f.picture(first) == nullptr);                   // the oldest is dropped, in the scrollback too
        CHECK_EQ(history_tiles(f), 2);                        // (the two that are left, which have scrolled off too)
        CHECK(f.scrollback_tiles(0).empty());                 // and the line that held the first has no tiles now at all
        CHECK(f.picture_count() == 3);
        // the same on the alternate screen
        Emulator a = make(6, 10);
        a.set_picture_budget(2500);
        a.feed("\x1b[?1049h");
        for (int i = 0; i < 4; i++) a.feed(at(i, 0) + sixel(10, 20));
        CHECK(a.picture_count() == 3 && tile(a, 0, 0).empty() && is(a, 1, 0, 0, 0));
        a.feed("\x1b[?1049l");
        CHECK(count_tiles(a) == 0);
        // the picture just added always stays
        Emulator g = make(4, 10);
        g.set_picture_budget(800);
        g.feed(sixel(10, 20));
        g.feed(sixel(10, 20));
        CHECK(g.picture_count() == 1 && count_tiles(g) == 1);
    }

    // ---- a picture animation does not take the memory of every frame it ever drew
    {
        Emulator e = make(4, 10);
        e.set_picture_budget(1 << 20);                        // 1 MiB: 1300 pictures of 800 bytes
        size_t most = 0;
        for (int i = 0; i < 20000; i++) { e.feed(at(0, 0) + sixel(10, 20)); most = std::max(most, e.picture_count()); }
        CHECK(most < 400);                                    // (the old ones that nothing shows are swept long before the budget is full)
        CHECK(e.picture_bytes() <= (size_t)(1 << 20));
        CHECK(is(e, 0, 0, 0, 0));
    }

    // ---- the oracle: a random stream into an emulator with pictures and one with letters
    {
        std::mt19937 rng(2024);
        const int dims[][3] = {{8, 20, 50}, {6, 13, 4}, {10, 30, 200}, {3, 8, 3}, {12, 40, 0}};
        for (const auto& d : dims) {
            for (int run = 0; run < 12; run++) {
                Pair p(d[0], d[1], (size_t)d[2]);
                for (int step = 0; step < 250; step++) {
                    const unsigned k = rng() % 100;
                    if (k < 18 && p.pictures < 200) {
                        const int w = 1 + (int)(rng() % 150), h = 1 + (int)(rng() % 300);
                        p.picture(w, h);
                    } else if (k < 21) {
                        const int r = 3 + (int)(rng() % 10), c = 4 + (int)(rng() % 25);
                        p.a.resize(r, c); p.b.resize(r, c);
                    } else if (k == 21) {
                        p.feed("\x1b" "c");
                        p.right = false;
                    } else {
                        const std::string s = random_op(rng, p);
                        if (!s.empty()) p.feed(s);
                    }
                    compare(p, step);
                    if (failures) { std::printf("   (dims %d x %d, run %d)\n", d[0], d[1], run); goto done; }
                }
                // the screen that is not shown, too
                p.feed("\x1b[?47h"); compare(p, -1);
                p.feed("\x1b[?47l"); compare(p, -2);
            }
        }
    }
done:

    if (failures == 0) std::printf("all picture tests passed\n");
    return failures == 0 ? 0 : 1;
}
