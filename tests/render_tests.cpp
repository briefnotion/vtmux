// Tests for PanePainter: an emulator's screen drawn into a retained vtscreen Picture (colours, attributes, the cursor,
// only the dirty rows, the bar row left alone).
// sources: ../src/render.cpp ../src/emulator.cpp ../src/vtparser.cpp ../src/width.cpp
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "../src/emulator.h"
#include "../src/render.h"

using namespace vtmux;
using vtscreen::PicCell;
using vtscreen::Picture;

namespace {

// a retained picture for an emulator of rows x cols, with the flags used up (as after a render)
Picture make_pic(int rows, int cols) {
    Picture p;
    p.init(rows, cols);
    p.retain();
    p.consume();
    return p;
}

bool row_touched(const Picture& p, int r) { return p.row_touched.size() > (size_t)r && p.row_touched[(size_t)r] != 0; }
bool cell_touched(const Picture& p, int r, int c) { return p.touched.size() > (size_t)(r * p.cols + c) && p.touched[(size_t)(r * p.cols + c)] != 0; }
int rows_touched(const Picture& p) {
    int n = 0;
    for (int r = 0; r < p.rows; r++) n += row_touched(p, r) ? 1 : 0;
    return n;
}
bool anything_touched(const Picture& p) { return p.all_touched || rows_touched(p) > 0; }

const PicCell& at(const Picture& p, int r, int c) { return p.at(r, c); }

bool rgb_is(const PicCell& c, double r, double g, double b) { return c.r == r && c.g == g && c.b == b; }
bool alt_is(const PicCell& c, double r, double g, double b) { return c.alt_lum == 1 && c.alt_r == r && c.alt_g == g && c.alt_b == b; }

// ---------------------------------------------------------------- the basics

void test_plain_text() {
    Emulator e(5, 20);
    Picture p = make_pic(5, 20);
    PanePainter pp;
    e.feed("hi");
    const int n = pp.paint(e, p, false, Theme());
    CHECK_EQ(n, 5);                                   // the first paint is a full one
    CHECK(!e.any_dirty());                            // the dirty flags are cleared
    const PicCell& h = at(p, 0, 0);
    CHECK(h.text);
    CHECK_EQ((int)h.ch, (int)U'h');
    CHECK(rgb_is(h, 204, 204, 204));                  // the default foreground is the theme's
    CHECK(!h.inv);
    CHECK_EQ(h.alt_lum, 0.0);                         // the default background: none, so the terminal's shows
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'i');
    CHECK_EQ((int)at(p, 0, 2).ch, (int)U' ');
    CHECK(at(p, 3, 7).text);                          // every cell of the screen is a text cell
    CHECK_EQ(p.rows, 6);
}

void test_theme_default_fg() {
    Emulator e(2, 10);
    Picture p = make_pic(2, 10);
    PanePainter pp;
    Theme th;
    th.default_fg[0] = 10; th.default_fg[1] = 20; th.default_fg[2] = 30;
    e.feed("x");
    pp.paint(e, p, false, th);
    CHECK(rgb_is(at(p, 0, 0), 10, 20, 30));
}

void test_colours() {
    Emulator e(4, 40);
    Picture p = make_pic(4, 40);
    PanePainter pp;
    // 16 colours
    e.feed("\x1b[31mA\x1b[91mB\x1b[0m");
    // 256: a cube colour, the first and the last grey, the cube corners
    e.feed("\x1b[38;5;196mC\x1b[38;5;232mD\x1b[38;5;255mE\x1b[38;5;21mF\x1b[38;5;16mG\x1b[38;5;231mH\x1b[38;5;67mI\x1b[0m");
    // truecolor
    e.feed("\x1b[38;2;10;20;30mJ\x1b[0m");
    pp.paint(e, p, false, Theme());
    CHECK(rgb_is(at(p, 0, 0), 205, 0, 0));
    CHECK(rgb_is(at(p, 0, 1), 255, 0, 0));
    CHECK(rgb_is(at(p, 0, 2), 255, 0, 0));            // 196: the cube's red
    CHECK(rgb_is(at(p, 0, 3), 8, 8, 8));              // 232: the darkest grey
    CHECK(rgb_is(at(p, 0, 4), 238, 238, 238));        // 255: the lightest
    CHECK(rgb_is(at(p, 0, 5), 0, 0, 255));            // 21
    CHECK(rgb_is(at(p, 0, 6), 0, 0, 0));              // 16
    CHECK(rgb_is(at(p, 0, 7), 255, 255, 255));        // 231
    int r, g, b;
    palette_rgb(67, r, g, b);
    CHECK(rgb_is(at(p, 0, 8), r, g, b));              // 67 = 95,135,175
    CHECK(rgb_is(at(p, 0, 8), 95, 135, 175));
    CHECK(rgb_is(at(p, 0, 9), 10, 20, 30));
    CHECK_EQ(at(p, 0, 0).alt_lum, 0.0);               // no background was set
}

void test_backgrounds() {
    Emulator e(3, 20);
    Picture p = make_pic(3, 20);
    PanePainter pp;
    e.feed("\x1b[44mA\x1b[48;5;196mB\x1b[48;2;1;2;3mC\x1b[49mD\x1b[41;32mE\x1b[0m");
    pp.paint(e, p, false, Theme());
    CHECK(alt_is(at(p, 0, 0), 0, 0, 238));            // 44 = palette 4
    CHECK(alt_is(at(p, 0, 1), 255, 0, 0));
    CHECK(alt_is(at(p, 0, 2), 1, 2, 3));
    CHECK_EQ(at(p, 0, 3).alt_lum, 0.0);               // default again: no alt colour
    CHECK(alt_is(at(p, 0, 4), 205, 0, 0));
    CHECK(rgb_is(at(p, 0, 4), 0, 205, 0));
    CHECK(rgb_is(at(p, 0, 0), 204, 204, 204));        // the foreground is separate
    // erasing keeps the background (the emulator does that): the erased cells carry it
    Emulator e2(2, 10);
    Picture p2 = make_pic(2, 10);
    PanePainter pp2;
    e2.feed("\x1b[42m\x1b[2K");
    pp2.paint(e2, p2, false, Theme());
    CHECK(alt_is(at(p2, 0, 9), 0, 205, 0));
    CHECK(at(p2, 0, 9).text);
}

void test_bold() {
    Emulator e(3, 40);
    Picture p = make_pic(3, 40);
    PanePainter pp;
    // bold red; bold bright red; bold default; bold truecolor; bold 256 colour 1 (indexed 1 too); bold cube colour
    e.feed("\x1b[1;31mA\x1b[1;91mB\x1b[0;1mC\x1b[1;38;2;1;2;3mD\x1b[0;1;38;5;1mE\x1b[0;1;38;5;100mF");
    pp.paint(e, p, false, Theme());
    CHECK(rgb_is(at(p, 0, 0), 255, 0, 0));            // 1 -> 9
    CHECK(rgb_is(at(p, 0, 1), 255, 0, 0));            // 9 stays
    CHECK(rgb_is(at(p, 0, 2), 255, 255, 255));        // the default foreground: bright white
    CHECK(rgb_is(at(p, 0, 3), 1, 2, 3));              // truecolor is left alone
    CHECK(rgb_is(at(p, 0, 4), 255, 0, 0));
    int r, g, b;
    palette_rgb(100, r, g, b);
    CHECK(rgb_is(at(p, 0, 5), r, g, b));              // 16 and up are left alone
    // with bold_is_bright off nothing changes
    Theme th;
    th.bold_is_bright = false;
    PanePainter pp2;
    Picture p2 = make_pic(3, 40);
    pp2.paint(e, p2, false, th);
    CHECK(rgb_is(at(p2, 0, 0), 205, 0, 0));
    CHECK(rgb_is(at(p2, 0, 2), 204, 204, 204));
    // bold black (index 0) -> 8, the dark grey
    Emulator e2(2, 10);
    Picture p3 = make_pic(2, 10);
    PanePainter pp3;
    e2.feed("\x1b[1;30mx");
    pp3.paint(e2, p3, false, Theme());
    CHECK(rgb_is(at(p3, 0, 0), 127, 127, 127));
}

void test_dim() {
    Emulator e(2, 20);
    Picture p = make_pic(2, 20);
    PanePainter pp;
    e.feed("\x1b[2mA\x1b[0;2;1mB\x1b[0;2;38;2;100;50;10mC\x1b[0;2;44mD");
    Theme th;
    pp.paint(e, p, false, th);
    CHECK(rgb_is(at(p, 0, 0), 122, 122, 122));        // 204 * 0.6
    CHECK(rgb_is(at(p, 0, 1), 153, 153, 153));        // bold first (255), then dim
    CHECK(rgb_is(at(p, 0, 2), 60, 30, 6));
    CHECK(alt_is(at(p, 0, 3), 0, 0, 238));            // the background is not dimmed
    th.dim = 0.5;
    pp.invalidate();
    pp.paint(e, p, false, th);
    CHECK(rgb_is(at(p, 0, 0), 102, 102, 102));
}

void test_hidden() {
    Emulator e(2, 20);
    Picture p = make_pic(2, 20);
    PanePainter pp;
    e.feed("\x1b[8;31;44mSECRET");
    pp.paint(e, p, false, Theme());
    for (int c = 0; c < 6; c++) {
        CHECK_EQ((int)at(p, 0, c).ch, (int)U' ');
        CHECK(alt_is(at(p, 0, c), 0, 0, 238));        // the colours are kept
        CHECK(rgb_is(at(p, 0, c), 205, 0, 0));
    }
}

void test_reverse_and_ignored() {
    Emulator e(2, 20);
    Picture p = make_pic(2, 20);
    PanePainter pp;
    e.feed("\x1b[7mR\x1b[0mN\x1b[4;3;9;5mU\x1b[0m");
    pp.paint(e, p, false, Theme());
    CHECK(at(p, 0, 0).inv);
    CHECK(rgb_is(at(p, 0, 0), 204, 204, 204));        // reverse is the inv flag; the colours are not swapped here
    CHECK(!at(p, 0, 1).inv);
    // underline, italic, strike and blink are passed to vtscreen as attributes of the cell, with the same letter and colours as a plain cell
    CHECK_EQ((int)at(p, 0, 2).ch, (int)U'U');
    CHECK(!at(p, 0, 2).inv);
    CHECK_EQ((int)at(p, 0, 2).attrs, (int)(vtscreen::CELL_UNDERLINE | vtscreen::CELL_ITALIC | vtscreen::CELL_STRIKE | vtscreen::CELL_BLINK));
    CHECK_EQ((int)at(p, 0, 1).attrs, 0);
    CHECK(at(p, 0, 2) == [&] { PicCell c = at(p, 0, 1); c.ch = U'U'; c.attrs = at(p, 0, 2).attrs; return c; }());
}

void test_attributes() {
    Emulator e(2, 30);
    Picture p = make_pic(2, 30);
    PanePainter pp;
    e.feed("\x1b[1mB\x1b[0m\x1b[3mI\x1b[0m\x1b[4mU\x1b[0m\x1b[9mS\x1b[0m\x1b[5mK\x1b[0m\x1b[1;4mX\x1b[22mY\x1b[24mZ");
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 0, 0).attrs, (int)vtscreen::CELL_BOLD);              // bold is also brighter (see test_bold): both
    CHECK_EQ((int)at(p, 0, 1).attrs, (int)vtscreen::CELL_ITALIC);
    CHECK_EQ((int)at(p, 0, 2).attrs, (int)vtscreen::CELL_UNDERLINE);
    CHECK_EQ((int)at(p, 0, 3).attrs, (int)vtscreen::CELL_STRIKE);
    CHECK_EQ((int)at(p, 0, 4).attrs, (int)vtscreen::CELL_BLINK);
    CHECK_EQ((int)at(p, 0, 5).attrs, (int)(vtscreen::CELL_BOLD | vtscreen::CELL_UNDERLINE));
    CHECK_EQ((int)at(p, 0, 6).attrs, (int)vtscreen::CELL_UNDERLINE);         // 22 took bold off only
    CHECK_EQ((int)at(p, 0, 7).attrs, 0);                                     // 24 took the underline off
    // an underlined blank is a blank cell with the attribute (the Display draws it)
    e.feed("\x1b[2;1H\x1b[4m  \x1b[0m");
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 1, 0).ch, (int)U' ');
    CHECK_EQ((int)at(p, 1, 0).attrs, (int)vtscreen::CELL_UNDERLINE);
    // concealed text shows nothing, not even a line
    e.feed("\x1b[2;5H\x1b[8;4mhid\x1b[0m");
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 1, 4).ch, (int)U' ');
    CHECK_EQ((int)at(p, 1, 4).attrs, 0);
    // without colour the attributes are still there
    Theme nc; nc.color = false;
    pp.invalidate();
    pp.paint(e, p, false, nc);
    CHECK_EQ((int)at(p, 0, 2).attrs, (int)vtscreen::CELL_UNDERLINE);
}

void test_wide() {
    Emulator e(2, 20);
    Picture p = make_pic(2, 20);
    PanePainter pp;
    e.feed("\x1b[31;44m\xE6\x97\xA5\xE6\x9C\xAC\x1b[0mx");     // two wide characters, then x
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 0, 0).ch, (int)0x65E5);                                        // the letters themselves now
    CHECK_EQ((int)at(p, 0, 0).attrs, (int)vtscreen::CELL_WIDE);
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U' ');
    CHECK_EQ((int)at(p, 0, 1).attrs, (int)vtscreen::CELL_TAIL);
    CHECK_EQ((int)at(p, 0, 2).ch, (int)0x672C);
    CHECK_EQ((int)at(p, 0, 2).attrs, (int)vtscreen::CELL_WIDE);
    CHECK_EQ((int)at(p, 0, 3).attrs, (int)vtscreen::CELL_TAIL);
    CHECK_EQ((int)at(p, 0, 4).ch, (int)U'x');
    CHECK_EQ((int)at(p, 0, 4).attrs, 0);
    CHECK(rgb_is(at(p, 0, 0), 205, 0, 0));            // the head keeps its colours
    CHECK(alt_is(at(p, 0, 0), 0, 0, 238));
    CHECK(alt_is(at(p, 0, 1), 0, 0, 238));            // and so does the tail's background
    CHECK(at(p, 0, 1).text);
}

// ---------------------------------------------------------------- the cursor

void test_cursor_drawn_and_removed() {
    Emulator e(5, 20);
    Picture p = make_pic(5, 20);
    PanePainter pp;
    e.feed("ab");                                      // the cursor is at row 0, col 2
    pp.paint(e, p, true, Theme());
    CHECK(at(p, 0, 2).inv);                            // the cursor cell: inverse
    CHECK(!at(p, 0, 1).inv);
    CHECK_EQ((int)at(p, 0, 2).ch, (int)U' ');

    // the same emulator, show_cursor off: the cell goes back, and only that row is written
    p.consume();
    int n = pp.paint(e, p, false, Theme());
    CHECK(!at(p, 0, 2).inv);
    CHECK_EQ(n, 1);
    CHECK_EQ(rows_touched(p), 1);
    CHECK(row_touched(p, 0));
    CHECK(cell_touched(p, 0, 2));
    CHECK(!cell_touched(p, 0, 1));

    // and on again
    p.consume();
    n = pp.paint(e, p, true, Theme());
    CHECK(at(p, 0, 2).inv);
    CHECK_EQ(n, 1);

    // the program hides the cursor
    p.consume();
    e.feed("\x1b[?25l");
    n = pp.paint(e, p, true, Theme());
    CHECK(!at(p, 0, 2).inv);
    CHECK_EQ(n, 1);
    // and shows it again
    p.consume();
    e.feed("\x1b[?25h");
    pp.paint(e, p, true, Theme());
    CHECK(at(p, 0, 2).inv);
}

void test_cursor_moves_repaint_old_row() {
    Emulator e(8, 20);
    Picture p = make_pic(8, 20);
    PanePainter pp;
    e.feed("\x1b[6;4H");                               // row 5, col 3 (nothing written, no row is dirty by the move)
    pp.paint(e, p, true, Theme());
    CHECK(at(p, 5, 3).inv);
    p.consume();
    e.feed("\x1b[2;1H");                               // only a move
    const int n = pp.paint(e, p, true, Theme());
    CHECK(!at(p, 5, 3).inv);                           // the old cursor is gone
    CHECK(at(p, 1, 0).inv);                            // the new one is there
    CHECK_EQ(n, 2);
    CHECK_EQ(rows_touched(p), 2);
    CHECK(row_touched(p, 5));
    CHECK(row_touched(p, 1));
    // a move within the row: that one row
    p.consume();
    e.feed("\x1b[2;9H");
    CHECK_EQ(pp.paint(e, p, true, Theme()), 1);
    CHECK(!at(p, 1, 0).inv);
    CHECK(at(p, 1, 8).inv);
    // when the cursor does not move and nothing changes: nothing is written
    p.consume();
    CHECK_EQ(pp.paint(e, p, true, Theme()), 0);
    CHECK(!anything_touched(p));
}

void test_cursor_over_a_cell_with_content() {
    Emulator e(3, 20);
    Picture p = make_pic(3, 20);
    PanePainter pp;
    e.feed("\x1b[7mA\x1b[0mB\x1b[1;1H");               // the cursor on a reversed cell
    pp.paint(e, p, true, Theme());
    CHECK(!at(p, 0, 0).inv);                           // toggled off
    e.feed("\x1b[1;2H");
    pp.paint(e, p, true, Theme());
    CHECK(at(p, 0, 0).inv);                            // back to its own reverse
    CHECK(at(p, 0, 1).inv);                            // 'B' under the cursor
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'B');
    CHECK(rgb_is(at(p, 0, 1), 204, 204, 204));
}

void test_cursor_row_written_when_dirty() {
    // when the cursor's own row is written the cursor is part of it
    Emulator e(3, 20);
    Picture p = make_pic(3, 20);
    PanePainter pp;
    pp.paint(e, p, true, Theme());
    CHECK(at(p, 0, 0).inv);
    e.feed("xy");
    pp.paint(e, p, true, Theme());
    CHECK(!at(p, 0, 0).inv);
    CHECK_EQ((int)at(p, 0, 0).ch, (int)U'x');
    CHECK(at(p, 0, 2).inv);
    int cursors = 0;
    for (int c = 0; c < 20; c++) cursors += at(p, 0, c).inv ? 1 : 0;
    CHECK_EQ(cursors, 1);
}

void test_cursor_off_the_screen_is_ignored() {
    Emulator e(3, 10);
    Picture p = make_pic(3, 10);
    PanePainter pp;
    e.feed("0123456789");                              // the cursor waits at the right edge
    pp.paint(e, p, true, Theme());
    int inv = 0;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 10; c++) inv += at(p, r, c).inv ? 1 : 0;
    CHECK(inv <= 1);
}

// ---------------------------------------------------------------- what is written

void test_only_dirty_rows() {
    Emulator e(10, 30);
    Picture p = make_pic(10, 30);
    PanePainter pp;
    CHECK_EQ(pp.paint(e, p, false, Theme()), 10);
    p.consume();

    e.feed("\x1b[4;1Hhello");                          // row 3
    CHECK(e.row_dirty(3));
    const int n = pp.paint(e, p, false, Theme());
    CHECK_EQ(n, 1);
    CHECK_EQ(rows_touched(p), 1);
    CHECK(row_touched(p, 3));
    CHECK(!e.any_dirty());
    for (int c = 0; c < 30; c++) CHECK_EQ(cell_touched(p, 3, c), c < 5);   // only the cells that really changed
    CHECK_EQ((int)at(p, 3, 0).ch, (int)U'h');

    // two rows
    p.consume();
    e.feed("\x1b[1;1Ha\x1b[10;1Hz");
    CHECK_EQ(pp.paint(e, p, false, Theme()), 2);
    CHECK(row_touched(p, 0));
    CHECK(row_touched(p, 9));
    CHECK_EQ(rows_touched(p), 2);
}

void test_idempotent() {
    Emulator e(6, 20);
    Picture p = make_pic(6, 20);
    PanePainter pp;
    e.feed("\x1b[31mred\x1b[0m and text\r\nsecond");
    pp.paint(e, p, true, Theme());
    p.consume();
    CHECK_EQ(pp.paint(e, p, true, Theme()), 0);
    CHECK(!anything_touched(p));
    CHECK_EQ(pp.paint(e, p, true, Theme()), 0);
    CHECK(!anything_touched(p));
    // a row marked dirty with no change: written, but nothing is noted as changed
    e.mark_all_dirty();
    CHECK_EQ(pp.paint(e, p, true, Theme()), 6);
    CHECK(!anything_touched(p));
}

void test_bar_row_untouched() {
    Emulator e(4, 20);
    Picture p = make_pic(4, 20);
    PanePainter pp;
    p.text(4, 0, "BAR", 1, 2, 3, true);                 // the control bar wrote its text
    p.consume();
    const std::vector<PicCell> before(p.cell.begin() + 4 * 20, p.cell.end());
    e.feed("\x1b[1;1Hfull screen of text\r\n\r\n\r\n\r\n\r\n");
    pp.paint(e, p, true, Theme());
    pp.invalidate();
    pp.paint(e, p, true, Theme());
    e.feed("\x1b[?5h");
    pp.paint(e, p, true, Theme());
    CHECK(!row_touched(p, 4));
    const std::vector<PicCell> after(p.cell.begin() + 4 * 20, p.cell.end());
    CHECK(before == after);
    CHECK_EQ((int)at(p, 4, 0).ch, (int)U'B');
}

void test_resize_and_invalidate() {
    Emulator e(4, 10);
    Picture p = make_pic(4, 10);
    PanePainter pp;
    e.feed("abc");
    pp.paint(e, p, false, Theme());
    p.consume();

    // invalidate: every row is written again (the picture is put right even where it was altered behind its back)
    p.cell[1 * 10 + 5].ch = U'#';                      // a stale cell nobody noted
    CHECK_EQ(pp.paint(e, p, false, Theme()), 0);       // without invalidate it stays
    CHECK_EQ((int)at(p, 1, 5).ch, (int)U'#');
    pp.invalidate();
    CHECK_EQ(pp.paint(e, p, false, Theme()), 4);
    CHECK_EQ((int)at(p, 1, 5).ch, (int)U' ');
    CHECK(cell_touched(p, 1, 5));

    // the emulator is resized and the picture made again: a full paint
    e.resize(6, 14);
    Picture q = make_pic(6, 14);
    CHECK_EQ(pp.paint(e, q, false, Theme()), 6);
    CHECK_EQ((int)at(q, 0, 0).ch, (int)U'a');
    CHECK_EQ(q.rows, 7);
    q.consume();
    CHECK_EQ(pp.paint(e, q, false, Theme()), 0);

    // a different picture of the same size is a full paint too
    Picture r = make_pic(6, 14);
    CHECK_EQ(pp.paint(e, r, false, Theme()), 6);
    CHECK_EQ((int)at(r, 0, 1).ch, (int)U'b');
}

void test_wrong_sized_picture_is_safe() {
    Emulator e(5, 20);
    PanePainter pp;
    e.feed("text\x1b[3;3H");
    Picture small = make_pic(3, 10);                   // smaller than the emulator
    pp.paint(e, small, true, Theme());
    CHECK_EQ((int)at(small, 0, 0).ch, (int)U't');
    Picture empty;
    pp.paint(e, empty, true, Theme());
    Picture big = make_pic(9, 40);
    PanePainter pp2;
    pp2.paint(e, big, true, Theme());
    CHECK_EQ((int)at(big, 0, 0).ch, (int)U't');
    CHECK(!row_touched(big, 6));                       // rows past the emulator's are not written
}

void test_reverse_video() {
    Emulator e(4, 10);
    Picture p = make_pic(4, 10);
    PanePainter pp;
    e.feed("\x1b[1;1Hnorm\x1b[7mR\x1b[0m");
    pp.paint(e, p, false, Theme());
    CHECK(!at(p, 0, 0).inv);
    CHECK(at(p, 0, 4).inv);
    p.consume();

    e.feed("\x1b[?5h");                                // DECSCNM on: every row is written
    CHECK_EQ(pp.paint(e, p, false, Theme()), 4);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++)
            CHECK_EQ(at(p, r, c).inv, !(r == 0 && c == 4));     // the meaning of reverse is swapped
    CHECK_EQ(rows_touched(p), 4);

    // the cursor in a reversed screen: toggled against the screen's own inversion
    p.consume();
    pp.paint(e, p, true, Theme());
    CHECK(!at(p, 0, 5).inv);
    CHECK(at(p, 0, 6).inv);

    p.consume();
    e.feed("\x1b[?5l");
    CHECK_EQ(pp.paint(e, p, false, Theme()), 4);
    CHECK(!at(p, 0, 0).inv);
    CHECK(at(p, 0, 4).inv);
    CHECK(!at(p, 3, 9).inv);
}

void test_no_color() {
    Emulator e(3, 20);
    Picture p = make_pic(3, 20);
    PanePainter pp;
    Theme th;
    th.color = false;
    e.feed("\x1b[1;31;44mA\x1b[7;38;2;1;2;3mB\x1b[0m\x1b[2mC");
    pp.paint(e, p, false, th);
    for (int c = 0; c < 3; c++) {
        const PicCell& k = at(p, 0, c);
        CHECK(rgb_is(k, 0, 0, 0));
        CHECK_EQ(k.alt_lum, 0.0);
        CHECK_EQ(k.alt_r + k.alt_g + k.alt_b, 0.0);
        CHECK(k.text);
    }
    CHECK(!at(p, 0, 0).inv);
    CHECK(at(p, 0, 1).inv);                            // reverse is kept
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'B');
}

// ---------------------------------------------------------------- a real screen, and a random one

void test_scroll_and_alt_screen() {
    Emulator e(4, 12);
    Picture p = make_pic(4, 12);
    PanePainter pp;
    e.feed("one\r\ntwo\r\nthree\r\nfour\r\nfive");      // scrolls
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 0, 0).ch, (int)U't');          // "two"
    CHECK_EQ((int)at(p, 3, 0).ch, (int)U'f');
    e.feed("\x1b[?1049h\x1b[Hvim");
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 0, 0).ch, (int)U'v');
    CHECK_EQ((int)at(p, 3, 0).ch, (int)U' ');
    e.feed("\x1b[?1049l");
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 0, 0).ch, (int)U't');
}

// Painting step by step must give the same picture as painting the finished screen from scratch.
void test_incremental_equals_fresh() {
    std::mt19937 rng(12345);
    const std::vector<std::string> pieces = {
        "hello", " world ", "\r\n", "\n", "\r", "\t", "\b", "\x1b[31m", "\x1b[1m", "\x1b[2m", "\x1b[7m", "\x1b[8m", "\x1b[0m",
        "\x1b[44m", "\x1b[38;5;200m", "\x1b[48;2;9;8;7m", "\x1b[H", "\x1b[2J", "\x1b[K", "\x1b[3;5H", "\x1b[10;2H", "\x1b[2L",
        "\x1b[3M", "\x1b[2@", "\x1b[2P", "\x1b[?25l", "\x1b[?25h", "\x1b[?5h", "\x1b[?5l", "\x1b[?1049h", "\x1b[?1049l",
        "\xE6\x97\xA5", "\xE2\x94\x80", "\x1b(0qqx\x1b(B", "\x1b[3S", "\x1b[2T", "\x1b[4;12r", "\x1b[r", "z"};
    for (int round = 0; round < 40; round++) {
        const int rows = 6 + (int)(rng() % 8), cols = 10 + (int)(rng() % 20);
        Emulator e(rows, cols);
        Picture inc = make_pic(rows, cols);
        PanePainter pp;
        const bool cursor = (rng() & 1) != 0;
        Theme th;
        th.bold_is_bright = (rng() & 1) != 0;
        for (int step = 0; step < 30; step++) {
            const int k = 1 + (int)(rng() % 6);
            for (int i = 0; i < k; i++) e.feed(pieces[rng() % pieces.size()]);
            pp.paint(e, inc, cursor, th);
            Picture fresh = make_pic(rows, cols);
            PanePainter pf;
            Emulator copy = e;                         // painting clears the dirty flags: use a copy
            pf.paint(copy, fresh, cursor, th);
            bool same = inc.cell.size() == fresh.cell.size();
            for (size_t i = 0; same && i < inc.cell.size(); i++) same = inc.cell[i] == fresh.cell[i];
            CHECK(same);
            if (!same) return;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- the history

void test_scrolled_back_view() {
    Emulator e(3, 10, 50);
    Picture p = make_pic(3, 10);
    PanePainter pp;
    for (int i = 1; i <= 8; i++) e.feed("L" + std::to_string(i) + "\r\n");          // 8 lines, the screen has 3: L7 and L8 and a blank row
    e.feed("live");
    CHECK_EQ((int)e.scrollback_size(), 6);                                            // L1 to L6 went off the top; the screen is L7, L8, live
    pp.paint(e, p, true, Theme());
    CHECK_EQ((int)at(p, 2, 0).ch, (int)U'l');                                         // the live screen: "live" on the last row
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'7');

    pp.paint(e, p, true, Theme(), 2);                                                 // two lines back: L5 L6 L7
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'5');
    CHECK_EQ((int)at(p, 1, 1).ch, (int)U'6');
    CHECK_EQ((int)at(p, 2, 1).ch, (int)U'7');
    CHECK(!at(p, 2, 4).inv);                                                          // no cursor drawn when scrolled back

    pp.paint(e, p, true, Theme(), 1000);                                              // more than there is: held to the history (L1 L2 L3)
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'1');
    CHECK_EQ((int)at(p, 2, 1).ch, (int)U'3');

    pp.paint(e, p, true, Theme(), 0);                                                 // back to live: a full repaint
    CHECK_EQ((int)at(p, 2, 0).ch, (int)U'l');
    CHECK_EQ((int)at(p, 0, 1).ch, (int)U'7');
    CHECK_EQ((int)at(p, 1, 1).ch, (int)U'8');
}

void test_narrow_screen_in_a_wide_pane() {
    // the screen is narrower than the pane (DECCOLM): what is in the rest of the pane is blanked, and again when the view is scrolled back
    Emulator e(3, 20, 50);
    Picture p = make_pic(3, 20);
    PanePainter pp;
    e.feed("0123456789ABCDEFGHIJ");
    pp.paint(e, p, false, Theme());
    CHECK_EQ((int)at(p, 0, 19).ch, (int)U'J');
    e.feed("\x1b[?40h\x1b[?3l");                          // 80 does not fit 20: nothing changes
    e.resize(3, 20);
    Emulator w(3, 100, 50);
    Picture q = make_pic(3, 100);
    PanePainter qp;
    w.feed("\x1b[1;90Hxyz");
    qp.paint(w, q, false, Theme());
    CHECK_EQ((int)at(q, 0, 89).ch, (int)U'x');
    w.feed("\x1b[?40h\x1b[?3l\x1b[1;1Hhello");           // 80 columns in a pane of 100
    qp.paint(w, q, false, Theme());
    CHECK_EQ(w.cols(), 80);
    CHECK_EQ((int)at(q, 0, 0).ch, (int)U'h');
    for (int c = 80; c < 100; c++) CHECK(at(q, 0, c) == PicCell());           // the old content out in the margin is gone
    CHECK_EQ((int)at(q, 0, 89).ch, (int)U' ');
    for (int i = 0; i < 70; i++) w.feed("line\r\n");
    qp.paint(w, q, false, Theme(), 5);                                         // scrolled back: the margin is still blank
    for (int c = 80; c < 100; c++) CHECK(at(q, 0, c) == PicCell());
    qp.paint(w, q, true, Theme());
    for (int c = 80; c < 100; c++) CHECK(at(q, 1, c) == PicCell());
}

int main() {
    test_narrow_screen_in_a_wide_pane();
    test_scrolled_back_view();
    test_plain_text();
    test_theme_default_fg();
    test_colours();
    test_backgrounds();
    test_bold();
    test_dim();
    test_hidden();
    test_reverse_and_ignored();
    test_attributes();
    test_wide();
    test_cursor_drawn_and_removed();
    test_cursor_moves_repaint_old_row();
    test_cursor_over_a_cell_with_content();
    test_cursor_row_written_when_dirty();
    test_cursor_off_the_screen_is_ignored();
    test_only_dirty_rows();
    test_idempotent();
    test_bar_row_untouched();
    test_resize_and_invalidate();
    test_wrong_sized_picture_is_safe();
    test_reverse_video();
    test_no_color();
    test_scroll_and_alt_screen();
    test_incremental_equals_fresh();
    if (failures == 0) std::printf("all render tests passed\n");
    return failures == 0 ? 0 : 1;
}
