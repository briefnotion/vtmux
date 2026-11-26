// VT100 conformance scenarios, in the order of the vttest menus (1 cursor movements, 2 screen features, 3 character sets,
// 6 reports), written from the DEC VT100 manual and from what xterm does. The point is to hold the emulator to the real
// terminals, so where the standard is ambiguous or terminals differ the test says so and leaves that out instead of
// asserting one reading (see tests/conformance/FINDINGS.md).
//
// Tests for what is not implemented yet fail until it is: DECID (ESC Z), DECREQTPARM (CSI x), the UK character set,
// DECCOLM clearing the screen (a multiplexer cannot resize the real terminal: it clears and keeps its size).
// sources: ../src/vtparser.cpp ../src/emulator.cpp ../src/width.cpp
#include <string>

#include "check.h"
#include "../src/emulator.h"

using namespace vtmux;

namespace {

std::string utf8(char32_t cp) {
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    return s;
}

// a row as text, trailing spaces cut
std::string line(const Emulator& e, int row) {
    std::string s;
    for (int c = 0; c < e.cols(); c++) {
        const Cell& k = e.cell(row, c);
        if (k.attrs & ATTR_WIDE_TAIL) continue;
        s += utf8(k.ch);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string screen(const Emulator& e) {
    std::string s;
    for (int r = 0; r < e.rows(); r++) s += line(e, r) + "|";
    return s;
}

bool at(const Emulator& e, int row, int col) { return e.cursor().row == row && e.cursor().col == col; }

std::string reply(Emulator& e) { return e.take_reply(); }

}  // namespace

int main() {
    // ================================================================================================ menu 1: cursor movements

    {   // the cursor stops at every edge (CUU CUD CUF CUB)
        Emulator e(6, 10);
        e.feed("\x1b[3;4H");
        CHECK(at(e, 2, 3));
        e.feed("\x1b[99A");   CHECK(at(e, 0, 3));
        e.feed("\x1b[99D");   CHECK(at(e, 0, 0));
        e.feed("\x1b[99B");   CHECK(at(e, 5, 0));
        e.feed("\x1b[99C");   CHECK(at(e, 5, 9));
        e.feed("\x1b[A");     CHECK(at(e, 4, 9));          // no parameter is 1
        e.feed("\x1b[0B");    CHECK(at(e, 5, 9));          // 0 is 1 as well
        e.feed("\x1b[;3H");   CHECK(at(e, 0, 2));          // an empty row parameter is 1
        e.feed("\x1b[4;H");   CHECK(at(e, 3, 0));
        e.feed("\x1b[0;0H");  CHECK(at(e, 0, 0));          // 0 means 1 in CUP
        e.feed("\x1b[7;77H"); CHECK(at(e, 5, 9));          // past the corner: the corner
        e.feed("\x1b[2;3f");  CHECK(at(e, 1, 2));          // HVP is CUP
        e.feed("\x1b[H");     CHECK(at(e, 0, 0));
    }
    {   // the vttest frame: DECALN fills the screen with E, then the inside is cleared with cursor movement, leaving a border
        Emulator e(8, 20);
        e.feed("\x1b#8");
        for (int r = 0; r < 8; r++) CHECK_EQ(line(e, r), std::string(20, 'E'));
        CHECK(at(e, 0, 0));
        for (int r = 2; r <= 7; r++) e.feed("\x1b[" + std::to_string(r) + ";2H\x1b[18X");     // ECH over the inside of rows 2..7 (1-based)
        CHECK_EQ(line(e, 0), std::string(20, 'E'));
        CHECK_EQ(line(e, 1), std::string("E") + std::string(18, ' ') + "E");
        CHECK_EQ(line(e, 6), std::string("E") + std::string(18, ' ') + "E");
        CHECK_EQ(line(e, 7), std::string(20, 'E'));
        e.feed("\x1b[8;1H\x1b[K\x1b[8;1H" + std::string(20, 'x'));
        CHECK_EQ(line(e, 7), std::string(20, 'x'));
    }
    {   // the wrap: the 80th column holds the last character, the wrap waits (the "last column flag")
        Emulator e(3, 10);
        e.feed("0123456789");
        CHECK(at(e, 0, 9));                                 // still on the last column: CPR would say 10
        CHECK_EQ(reply(e), std::string());
        e.feed("\x1b[6n");
        CHECK_EQ(reply(e), std::string("\x1b[1;10R"));
        e.feed("A");
        CHECK(at(e, 1, 1));
        CHECK_EQ(line(e, 1), std::string("A"));
    }
    {   // the deferred wrap is cancelled by the cursor moving: BS and CUB step back from the last column (not from the edge),
        // CUF and CR clear it, and what is printed next overwrites instead of wrapping
        Emulator e(3, 10);
        e.feed("0123456789\b" "X");
        CHECK_EQ(line(e, 0), std::string("01234567X9"));    // BS from the last column is column 9 -> 8
        CHECK(at(e, 0, 9));
        Emulator f(3, 10);
        f.feed("0123456789\x1b[2D" "X");
        CHECK_EQ(line(f, 0), std::string("0123456X89"));
        Emulator g(3, 10);
        g.feed("0123456789\x1b[C" "X");                       // CUF at the margin: nothing moves, but the wrap is gone
        CHECK_EQ(line(g, 0), std::string("012345678X"));
        CHECK_EQ(line(g, 1), std::string());
        Emulator h(3, 10);
        h.feed("0123456789\r" "X");
        CHECK_EQ(line(h, 0), std::string("X123456789"));
        Emulator i(3, 10);
        i.feed("0123456789\x1b[2;3H" "X");                    // CUP clears it too
        CHECK_EQ(line(i, 1), std::string("  X"));
        CHECK_EQ(line(i, 0), std::string("0123456789"));
    }
    {   // wrap at the bottom scrolls; with the wrap mode off the last column is overwritten and nothing scrolls
        Emulator e(2, 4);
        e.feed("abcdefghij");
        CHECK_EQ(screen(e), std::string("efgh|ij|"));
        Emulator f(2, 4);
        f.feed("\x1b[?7l" "abcdefghij");
        CHECK_EQ(screen(f), std::string("abcj||"));
        CHECK(at(f, 0, 3));
        f.feed("\x1b[?7h" "k");                                // wrap mode back on: the last column is again where the wrap happens
        CHECK_EQ(line(f, 0), std::string("abck"));
    }
    {   // control characters: BS stops at column 0, VT and FF are line feeds, CR, LF, tab stops every 8 columns
        Emulator e(5, 40);
        e.feed("\b\b");              CHECK(at(e, 0, 0));
        e.feed("ab\x0b");            CHECK(at(e, 1, 2));    // VT = LF (the column does not change)
        e.feed("\x0c");              CHECK(at(e, 2, 2));    // FF = LF
        e.feed("\r");                CHECK(at(e, 2, 0));
        e.feed("\t");                CHECK(at(e, 2, 8));
        e.feed("\t\t");              CHECK(at(e, 2, 24));
        e.feed("\x1b[1;37H\t");      CHECK(at(e, 0, 39));   // a tab beyond the last stop goes to the last column
        e.feed("\t");                CHECK(at(e, 0, 39));   // and stays there
        e.feed("\r\x1b[3C\t");       CHECK(at(e, 0, 8));    // from column 3 to the next stop
        e.feed("\r\x07\x07");        CHECK_EQ(e.bell_count(), 2);
        e.feed("\x00\x7f");          CHECK(at(e, 0, 0));    // NUL and DEL are ignored
    }
    {   // tab stops: HTS sets one, TBC 0 clears the one under the cursor, TBC 3 clears all, RIS puts back every 8th
        Emulator e(2, 40);
        e.feed("\x1b[4G\x1bH");                              // a stop at column 3
        e.feed("\r\t");              CHECK(at(e, 0, 3));
        e.feed("\t");                CHECK(at(e, 0, 8));
        e.feed("\x1b[9G\x1b[0g");                            // clear the one at column 8
        e.feed("\r\t\t");            CHECK(at(e, 0, 16));    // 3 then 16
        e.feed("\x1b[3g");                                   // clear them all
        e.feed("\r\t");              CHECK(at(e, 0, 39));
        e.feed("\x1b[11G\x1bH\r\t"); CHECK(at(e, 0, 10));    // a new one
        e.feed("\x1b" "c");                                  // RIS
        e.feed("\t");                CHECK(at(e, 0, 8));
        e.feed("\x1b[2g");           CHECK(true);            // TBC 2: unknown, ignored (must not clear anything)
        e.feed("\t");                CHECK(at(e, 0, 16));
    }
    {   // IND, NEL, RI inside the screen and at the margins
        Emulator e(4, 8);
        e.feed("a\x1b" "Db");                                // IND: down, same column
        CHECK_EQ(line(e, 1), std::string(" b"));
        e.feed("\x1b" "Ec");                                 // NEL: down and to the first column
        CHECK_EQ(line(e, 2), std::string("c"));
        e.feed("\x1b" "M" "d");                              // RI: up, same column (the d replaces b)
        CHECK_EQ(line(e, 1), std::string(" d"));
        e.feed("\x1b[1;1H\x1b" "M");                         // RI at the top scrolls the screen down
        CHECK_EQ(line(e, 0), std::string());
        CHECK_EQ(line(e, 1), std::string("a"));
        CHECK_EQ(line(e, 2), std::string(" d"));
        CHECK_EQ(line(e, 3), std::string("c"));
        e.feed("\x1b[4;1H\x1b" "D");                         // IND at the bottom scrolls up
        CHECK_EQ(line(e, 0), std::string("a"));
        CHECK_EQ(line(e, 3), std::string());
    }
    {   // scroll region: DECSTBM, scrolling only inside it, the cursor homing, invalid regions ignored
        Emulator e(8, 6);
        for (int r = 0; r < 8; r++) e.feed("\x1b[" + std::to_string(r + 1) + ";1H" + std::to_string(r));
        e.feed("\x1b[3;6r");                                 // lines 3..6 (1-based)
        CHECK(at(e, 0, 0));                                  // DECSTBM homes the cursor
        e.feed("\x1b[6;1H\n");                               // LF at the bottom margin: only the region scrolls
        CHECK_EQ(screen(e), std::string("0|1|3|4|5||6|7|"));
        e.feed("\x1b[3;1H\x1b" "M");                         // RI at the top margin: region scrolls down
        CHECK_EQ(screen(e), std::string("0|1||3|4|5|6|7|"));
        e.feed("\x1b[8;1H\n");                               // LF below the region at the bottom of the screen: the screen does not scroll
        CHECK_EQ(screen(e), std::string("0|1||3|4|5|6|7|"));
        CHECK(at(e, 7, 0));
        e.feed("\x1b[2;1H\x1b" "M");                         // RI above the region: moves up, nothing scrolls
        CHECK(at(e, 0, 0));
        e.feed("\x1b[1;1H\x1b" "M");                         // RI at the top row but above the region: stays
        CHECK(at(e, 0, 0));
        CHECK_EQ(screen(e), std::string("0|1||3|4|5|6|7|"));
        e.feed("\x1b[4;4r");                                 // top == bottom: not a region, ignored (and the cursor is not homed)
        e.feed("\x1b[6;1H\n");
        CHECK_EQ(screen(e), std::string("0|1|3|4|5||6|7|"));   // the 3..6 region is still in force
        e.feed("\x1b[r");                                    // no parameters: the whole screen
        CHECK(at(e, 0, 0));
        e.feed("\x1b[8;1H\n");
        CHECK_EQ(line(e, 0), std::string("1"));              // now the whole screen scrolls
        e.feed("\x1b[3;99r");                                // a bottom past the screen is the last line
        e.feed("\x1b[8;1H\n");
        CHECK_EQ(line(e, 0), std::string("1"));              // line 1 is above the region: untouched
    }
    {   // CUU and CUD stop at the margins when the cursor is inside the region, and at the screen edge when outside it
        Emulator e(10, 5);
        e.feed("\x1b[4;7r");
        e.feed("\x1b[5;1H\x1b[9A");
        CHECK(at(e, 3, 0));                                  // stopped at the top margin
        e.feed("\x1b[9B");
        CHECK(at(e, 6, 0));                                  // and at the bottom margin
        e.feed("\x1b[2;1H\x1b[9B");
        CHECK(at(e, 6, 0));                                  // above the region, moving down: it passes the top margin and stops at the bottom one
        e.feed("\x1b[9;1H\x1b[99A");
        CHECK(at(e, 3, 0));                                  // below the region, moving up: stops at the top margin
        e.feed("\x1b[2;1H\x1b[99A");
        CHECK(at(e, 0, 0));                                  // above it, moving up: the screen edge
        e.feed("\x1b[9;1H\x1b[99B");
        CHECK(at(e, 9, 0));                                  // below it, moving down: the screen edge
    }
    {   // origin mode: home and CUP are relative to the region, the cursor cannot leave it, CPR is relative
        Emulator e(10, 20);
        e.feed("\x1b[3;7r\x1b[?6h");
        CHECK(at(e, 2, 0));                                  // homed into the region
        e.feed("\x1b[1;1H");         CHECK(at(e, 2, 0));
        e.feed("\x1b[3;5H");         CHECK(at(e, 4, 4));
        e.feed("\x1b[99;1H");        CHECK(at(e, 6, 0));     // clamped to the bottom margin
        e.feed("\x1b[99A");          CHECK(at(e, 2, 0));
        e.feed("\x1b[99B");          CHECK(at(e, 6, 0));
        e.feed("\x1b[6n");
        CHECK_EQ(reply(e), std::string("\x1b[5;1R"));        // row 5 of the region (rows 3..7), not row 7 of the screen
        e.feed("\x1b[4d");           CHECK(at(e, 5, 0));     // VPA is relative as well
        e.feed("\x1b[H" "xy");
        CHECK_EQ(line(e, 2), std::string("xy"));
        e.feed("\x1b[?6l");                                  // leaving origin mode homes to the real top left
        CHECK(at(e, 0, 0));
        e.feed("\x1b[1;1H");         CHECK(at(e, 0, 0));
        e.feed("\x1b[9;1H");         CHECK(at(e, 8, 0));     // and the cursor may be outside the region again
        e.feed("\x1b[6n");
        CHECK_EQ(reply(e), std::string("\x1b[9;1R"));
    }
    {   // origin mode with the whole screen as region is the same as no origin mode
        Emulator e(5, 10);
        e.feed("\x1b[?6h\x1b[5;5H");
        CHECK(at(e, 4, 4));
        e.feed("\x1b[99;99H");
        CHECK(at(e, 4, 9));
    }
    {   // DECSC / DECRC: position, attributes, character set, origin mode
        Emulator e(6, 20);
        e.feed("\x1b[3;5H\x1b[1;4;31m\x1b(0\x1b" "7");       // save at (3,5): bold underline red, G0 line drawing
        e.feed("\x1b[1;1H\x1b[0m\x1b(B" "x");
        CHECK_EQ(e.cell(0, 0).attrs, (uint16_t)0);
        e.feed("\x1b" "8" "q");                              // restore: back to (3,5) with the pen and the set
        CHECK(at(e, 2, 5));
        CHECK_EQ(e.cell(2, 4).ch, (char32_t)U'─');
        CHECK(e.cell(2, 4).attrs & ATTR_BOLD);
        CHECK(e.cell(2, 4).attrs & ATTR_UNDERLINE);
        CHECK(e.cell(2, 4).fg == Color::index(1));
        e.feed("\x1b[1;1H\x1b" "8");                         // a second restore gives the same state: DECSC is one slot, not a stack
        CHECK(at(e, 2, 4));
        Emulator f(6, 20);
        f.feed("\x1b[4;4H\x1b[1m" "ab");                    // restore with nothing saved: home, plain
        f.feed("\x1b" "8");
        CHECK(at(f, 0, 0));
        f.feed("c");
        CHECK_EQ(f.cell(0, 0).attrs, (uint16_t)0);
        Emulator g(10, 20);
        g.feed("\x1b[3;7r\x1b[?6h\x1b[2;3H\x1b" "7");        // saved in origin mode, relative to the region
        g.feed("\x1b[?6l\x1b[1;1H\x1b" "8");
        CHECK(at(g, 3, 2));                                  // the saved position, and origin mode is on again
        g.feed("\x1b[1;1H");
        CHECK(at(g, 2, 0));
    }

    // ================================================================================================ menu 1: screen features

    {   // DECCOLM (?3) without the program having allowed it (mode 40): it clears the screen, homes the cursor and keeps its size (see
        // emulator_tests for the switch itself, which narrows the screen within the pane)
        Emulator e(5, 20);
        e.feed("abc\r\ndef\x1b[3;2H\x1b[?3h");
        CHECK_EQ(e.cols(), 20);
        CHECK_EQ(e.rows(), 5);
        CHECK_EQ(screen(e), std::string("|||||"));
        CHECK(at(e, 0, 0));
        e.feed("xyz\x1b[2;4H\x1b[?3l");
        CHECK_EQ(e.cols(), 20);
        CHECK_EQ(screen(e), std::string("|||||"));
        CHECK(at(e, 0, 0));
    }
    {   // DECSCNM: reverse video of the whole screen, the contents are untouched
        Emulator e(3, 10);
        e.feed("abc");
        CHECK(!e.reverse_video());
        e.feed("\x1b[?5h");
        CHECK(e.reverse_video());
        CHECK_EQ(line(e, 0), std::string("abc"));
        CHECK(e.cell(0, 0).attrs == 0);
        e.feed("\x1b[?5l");
        CHECK(!e.reverse_video());
    }
    {   // DECSCLM (?4, smooth scrolling) and DECARM (?8, auto repeat) and DECINLM (?9) change nothing on screen: accepted silently
        Emulator e(3, 10);
        e.feed("\x1b[?4h\x1b[?8h\x1b[?4l\x1b[?8l" "ok");
        CHECK_EQ(line(e, 0), std::string("ok"));
    }
    {   // double-height and double-width lines (DECDHL, DECDWL, DECSWL) are not drawn, but the sequence is consumed
        Emulator e(4, 10);
        e.feed("\x1b#3" "top\r\n\x1b#4" "bot\r\n\x1b#6" "wide\r\n\x1b#5" "std");
        CHECK_EQ(screen(e), std::string("top|bot|wide|std|"));
    }
    {   // SGR on the VT100: bold, underline, blink, reverse, and 0 / none resets
        Emulator e(2, 20);
        e.feed("\x1b[1mB\x1b[4mU\x1b[5mK\x1b[7mR\x1b[m" "N");
        CHECK(e.cell(0, 0).attrs & ATTR_BOLD);
        CHECK(e.cell(0, 1).attrs & ATTR_UNDERLINE);
        CHECK(e.cell(0, 1).attrs & ATTR_BOLD);
        CHECK(e.cell(0, 2).attrs & ATTR_BLINK);
        CHECK(e.cell(0, 3).attrs & ATTR_REVERSE);
        CHECK_EQ(e.cell(0, 4).attrs, (uint16_t)0);
        e.feed("\x1b[1;7m" "x\x1b[0m" "y");                 // several in one, then reset with 0
        CHECK_EQ(e.cell(0, 5).attrs, (uint16_t)(ATTR_BOLD | ATTR_REVERSE));
        CHECK_EQ(e.cell(0, 6).attrs, (uint16_t)0);
    }

    // ================================================================================================ menu 2: screen features

    {   // ED and EL in all their forms; the cursor does not move
        Emulator e(3, 8);
        auto fill = [&]() { e.feed("\x1b[1;1Haaaaaaaa\x1b[2;1Hbbbbbbbb\x1b[3;1Hcccccccc"); };
        fill();
        e.feed("\x1b[2;4H\x1b[K");      CHECK_EQ(screen(e), std::string("aaaaaaaa|bbb|cccccccc|"));
        CHECK(at(e, 1, 3));
        fill();
        e.feed("\x1b[2;4H\x1b[0K");     CHECK_EQ(line(e, 1), std::string("bbb"));
        fill();
        e.feed("\x1b[2;4H\x1b[1K");     CHECK_EQ(line(e, 1), std::string("    bbbb"));    // up to and including the cursor cell
        fill();
        e.feed("\x1b[2;4H\x1b[2K");     CHECK_EQ(screen(e), std::string("aaaaaaaa||cccccccc|"));
        CHECK(at(e, 1, 3));
        fill();
        e.feed("\x1b[2;4H\x1b[J");      CHECK_EQ(screen(e), std::string("aaaaaaaa|bbb||"));
        fill();
        e.feed("\x1b[2;4H\x1b[0J");     CHECK_EQ(screen(e), std::string("aaaaaaaa|bbb||"));
        fill();
        e.feed("\x1b[2;4H\x1b[1J");     CHECK_EQ(screen(e), std::string("|    bbbb|cccccccc|"));
        fill();
        e.feed("\x1b[2;4H\x1b[2J");     CHECK_EQ(screen(e), std::string("|||"));
        CHECK(at(e, 1, 3));                                                              // ED 2 does not home the cursor
    }
    {   // erasing uses the current background colour (xterm: back colour erase), also for scrolling and IL / DL
        Emulator e(3, 6);
        e.feed("abcdef\x1b[44m\x1b[1;3H\x1b[K");
        CHECK(e.cell(0, 1).bg == Color());
        CHECK(e.cell(0, 2).bg == Color::index(4));
        CHECK(e.cell(0, 5).bg == Color::index(4));
        CHECK_EQ(e.cell(0, 5).ch, (char32_t)U' ');
        e.feed("\x1b[2J");
        CHECK(e.cell(2, 5).bg == Color::index(4));
        e.feed("\x1b[0m\x1b[2J");
        CHECK(e.cell(2, 5).bg == Color());
    }
    {   // IL and DL inside a region: lines shift within it and are lost off its edge, nothing outside moves
        Emulator e(8, 4);
        for (int r = 0; r < 8; r++) e.feed("\x1b[" + std::to_string(r + 1) + ";1H" + std::to_string(r));
        e.feed("\x1b[3;6r\x1b[4;1H\x1b[L");                  // IL at row 4 (1-based) in region 3..6
        CHECK_EQ(screen(e), std::string("0|1|2||3|4|6|7|"));
        e.feed("\x1b[4;1H\x1b[2M");                          // DL 2 there
        CHECK_EQ(line(e, 0), std::string("0"));
        CHECK_EQ(line(e, 2), std::string("2"));
        CHECK_EQ(line(e, 3), std::string("4"));
        CHECK_EQ(line(e, 4), std::string());
        CHECK_EQ(line(e, 5), std::string());
        CHECK_EQ(line(e, 6), std::string("6"));
        CHECK_EQ(line(e, 7), std::string("7"));
        e.feed("\x1b[1;1H\x1b[M");                           // outside the region: nothing happens
        CHECK_EQ(line(e, 0), std::string("0"));
        CHECK_EQ(line(e, 1), std::string("1"));
        e.feed("\x1b[8;1H\x1b[L");
        CHECK_EQ(line(e, 7), std::string("7"));
        e.feed("\x1b[3;1H\x1b[99L");                         // more lines than the region has: clears from the cursor to the bottom margin
        CHECK_EQ(line(e, 2), std::string());
        CHECK_EQ(line(e, 5), std::string());
        CHECK_EQ(line(e, 6), std::string("6"));
    }
    {   // DECALN also homes the cursor; it works with a region set and with colours
        Emulator e(4, 6);
        e.feed("\x1b[2;3r\x1b[1;31mx\x1b#8");
        CHECK(at(e, 0, 0));
        for (int r = 0; r < 4; r++) CHECK_EQ(line(e, r), std::string(6, 'E'));
    }
    {   // a full reset puts everything back: screen, cursor, modes, pen, character sets, scroll region, tab stops, alternate screen
        Emulator e(5, 10, 100);
        e.feed("\x1b[?1049h\x1b[3;4r\x1b[?6h\x1b[?7l\x1b[4h\x1b[1;31m\x1b(0\x1b[3g\x1b[20h" "junk\r\n");
        e.feed("\x1b" "c");
        CHECK(!e.alt_screen());
        CHECK(at(e, 0, 0));
        CHECK_EQ(screen(e), std::string("|||||"));
        e.feed("0123456789A");                               // wrap is on again (a wrap at column 10) and there is no region, no insert mode
        CHECK_EQ(line(e, 0), std::string("0123456789"));
        CHECK_EQ(line(e, 1), std::string("A"));
        CHECK_EQ(e.cell(1, 0).attrs, (uint16_t)0);
        CHECK(e.cell(1, 0).fg == Color());
        e.feed("\x1b[5;1H\n");
        CHECK_EQ(line(e, 0), std::string("A"));              // the whole screen scrolls: no region left
        e.feed("\x1b[1;1Hq\t");
        CHECK_EQ(e.cell(0, 0).ch, (char32_t)U'q');           // and the line drawing set is gone
        CHECK(at(e, 0, 8));                                  // default tab stops are back
        e.feed("\r\nl\n");                                   // LNM is off again: LF does not return the carriage
        CHECK(at(e, 2, 1));
    }

    // ================================================================================================ menu 3: character sets

    {   // DEC special graphics in G0: every character of 0x5F..0x7E
        static const char32_t TABLE[32] = {
            U' ',  U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°', U'±', U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼',
            U'⎺', U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴', U'┬', U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·'};
        Emulator e(2, 40);
        e.feed("\x1b(0");
        for (int i = 0; i < 32; i++) e.feed(std::string(1, (char)(0x5F + i)));
        for (int i = 0; i < 32; i++) {
            const char32_t got = e.cell(0, i).ch;
            if (i == 0) CHECK(got == U' ' || got == 0xA0);   // 0x5F: a blank (xterm: no-break space)
            else CHECK(got == TABLE[i]);
        }
        e.feed("\x1b(B\r\n");
        e.feed("\x1b(0" "AZ09 \x1b(B" "q");                 // outside 0x5F..0x7E nothing changes; back to ASCII after ESC ( B
        CHECK_EQ(line(e, 1), std::string("AZ09 q"));
    }
    {   // G0 and G1 with SO and SI; the power-up state is ASCII in both
        Emulator e(3, 20);
        e.feed("q\x0eq\x0fq");
        CHECK_EQ(line(e, 0), std::string("qqq"));            // G1 is ASCII too until it is designated
        e.feed("\r\n\x1b)0" "q\x0eq\x0fq");                  // G1 = graphics: only the SO part is drawn
        CHECK_EQ(line(e, 1), std::string("q─q"));
        e.feed("\r\n\x1b(0\x1b)B" "q\x0eq\x0fq");            // G0 graphics, G1 ASCII
        CHECK_EQ(line(e, 2), std::string("─q─"));
    }
    {   // the shifted state and the sets are part of what DECSC saves; a line feed or a clear does not change them
        Emulator e(3, 10);
        e.feed("\x1b)0\x0e" "q\x1b" "7\x0f" "q\x1b" "8" "q");   // q (graphics), save, SI, q (ASCII), restore: back at column 1 and shifted out again
        CHECK_EQ(line(e, 0), std::string("──"));
        e.feed("\x1b[2J\x1b[H" "q");
        CHECK_EQ(line(e, 0), std::string("─"));              // the set is still selected after erasing the screen
    }
    {   // the UK set (ESC ( A): the pound sign replaces #, everything else is ASCII
        Emulator e(2, 20);
        e.feed("\x1b(A" "#$%a~");
        CHECK_EQ(line(e, 0), std::string("£$%a~"));
        e.feed("\x1b(B" "#");
        CHECK_EQ(line(e, 0), std::string("£$%a~#"));
        e.feed("\r\n\x1b)A\x0e" "#\x0f" "#");
        CHECK_EQ(line(e, 1), std::string("£#"));
    }
    {   // a graphics set does not touch the control sequences, and wide characters pass through
        Emulator e(2, 20);
        e.feed("\x1b(0\x1b[1;3Hlqk\x1b[2;1H" "中A");
        CHECK_EQ(line(e, 0), std::string("  ┌─┐"));
        CHECK_EQ(line(e, 1), std::string("中A"));
    }

    // ================================================================================================ menu 6: reports

    {   // DSR: status and cursor position
        Emulator e(24, 80);
        e.feed("\x1b[5n");
        CHECK_EQ(reply(e), std::string("\x1b[0n"));
        e.feed("\x1b[12;40H\x1b[6n");
        CHECK_EQ(reply(e), std::string("\x1b[12;40R"));
        e.feed("\x1b[n");                                    // no parameter is 0: not a request
        CHECK_EQ(reply(e), std::string());
        e.feed("\x1b[7n");                                   // unknown requests are not answered
        CHECK_EQ(reply(e), std::string());
        e.feed("\x1b[5n\x1b[6n");                            // two requests, two answers, in order
        CHECK_EQ(reply(e), std::string("\x1b[0n\x1b[12;40R"));
        CHECK_EQ(reply(e), std::string());                   // taking the reply empties it
    }
    {   // the DEC-private reports xterm gives: extended cursor position and printer status
        Emulator e(24, 80);
        e.feed("\x1b[3;4H\x1b[?6n");
        CHECK_EQ(reply(e), std::string("\x1b[?3;4R"));
        e.feed("\x1b[?15n");
        CHECK_EQ(reply(e), std::string("\x1b[?13n"));        // no printer
    }
    {   // DA and DECID: a VT100 with the advanced video option
        Emulator e(24, 80);
        e.feed("\x1b[c");        CHECK_EQ(reply(e), std::string("\x1b[?1;2c"));
        e.feed("\x1b[0c");       CHECK_EQ(reply(e), std::string("\x1b[?1;2c"));
        e.feed("\x1bZ");         CHECK_EQ(reply(e), std::string("\x1b[?1;2c"));       // DECID is the same answer
        e.feed("\x1b[1c");       CHECK_EQ(reply(e), std::string());                   // only DA 0 is a request
    }
    {   // DECREQTPARM: the terminal parameters report. 0 asks for a report that is also sent unsolicited (reply code 2);
        // 1 asks for one only on request (reply code 3). 1;1;1;112;112;1;0 = no parity, 8 bits, 9600 baud both ways, multiplier 1
        Emulator e(24, 80);
        e.feed("\x1b[0x");       CHECK_EQ(reply(e), std::string("\x1b[2;1;1;112;112;1;0x"));
        e.feed("\x1b[x");        CHECK_EQ(reply(e), std::string("\x1b[2;1;1;112;112;1;0x"));
        e.feed("\x1b[1x");       CHECK_EQ(reply(e), std::string("\x1b[3;1;1;112;112;1;0x"));
    }
    {   // a report goes out even when it is split across two reads
        Emulator e(24, 80);
        e.feed("\x1b[");
        e.feed("6");
        e.feed("n");
        CHECK_EQ(reply(e), std::string("\x1b[1;1R"));
    }

    // ================================================================================================ the parser, as vttest checks it

    {   // control characters inside a control sequence are carried out, and the sequence goes on
        Emulator e(5, 20);
        e.feed("\x1b[2;\r\n3\x08H");                         // CUP 2;3 with a CR, a LF and a BS in the middle
        CHECK(at(e, 1, 2));                                  // the sequence still ended as CUP 2;3
        CHECK_EQ(screen(e), std::string("|||||"));
        Emulator f(5, 20);
        f.feed("ab\x1b[3\x08" "C" "x");                      // CUF 3 with a BS inside: BS moves 1 left (col 1), CUF moves 3 right (col 4)
        CHECK_EQ(line(f, 0), std::string("ab  x"));
    }
    {   // CAN and SUB abort a sequence; ESC inside one starts a new one
        Emulator e(3, 20);
        e.feed("a\x1b[3\x18" "b");                           // CAN: the sequence is dropped and b is printed
        CHECK_EQ(line(e, 0), std::string("ab"));
        e.feed("\x1b[3\x1a" "c");                            // SUB: same (a real VT100 also shows an error character; xterm does not)
        CHECK(line(e, 0).find('c') != std::string::npos);
        e.feed("\x1b[1G" "x");
        e.feed("\x1b[3\x1b[2G" "d");                         // an ESC inside starts a fresh CSI: CHA 2
        CHECK_EQ(line(e, 0).substr(0, 2), std::string("xd"));
    }
    {   // unknown sequences are skipped to their end, not printed
        Emulator e(3, 20);
        e.feed("a\x1b[?99;1$zb\x1b%Gc\x1b$Bd\x1b[=5ue");
        CHECK_EQ(line(e, 0), std::string("abcde"));
    }
    {   // an incomplete sequence at the end of one read is finished by the next
        Emulator e(3, 20);
        e.feed("ab\x1b");
        e.feed("[");
        e.feed("1;");
        e.feed("1");
        e.feed("H" "c");
        CHECK_EQ(line(e, 0), std::string("cb"));
    }

    // ================================================================================================ ISO 6429 and xterm additions that VT100 programs meet

    {   // ECH, REP, CNL, CPL, CHA, VPA, HPA, VPR, HPR, SU, SD, CHT, CBT
        Emulator e(6, 20);
        e.feed("abcdefghij\x1b[1;3H\x1b[3X");
        CHECK_EQ(line(e, 0), std::string("ab   fghij"));
        CHECK(at(e, 0, 2));                                  // ECH does not move the cursor
        e.feed("\x1b[1;1H" "x\x1b[3b");                      // REP repeats the last printed character
        CHECK_EQ(line(e, 0), std::string("xxxx fghij"));
        CHECK(at(e, 0, 4));
        e.feed("\x1b[2E");  CHECK(at(e, 2, 0));
        e.feed("\x1b[1F");  CHECK(at(e, 1, 0));
        e.feed("\x1b[5G");  CHECK(at(e, 1, 4));
        e.feed("\x1b[4d");  CHECK(at(e, 3, 4));
        e.feed("\x1b[3`");  CHECK(at(e, 3, 2));
        e.feed("\x1b[2a");  CHECK(at(e, 3, 4));
        e.feed("\x1b[1e");  CHECK(at(e, 4, 4));
        e.feed("\x1b[1;1H\x1b[2I");  CHECK(at(e, 0, 16));    // CHT 2: two stops on
        e.feed("\x1b[1Z");           CHECK(at(e, 0, 8));
        Emulator f(4, 5);
        f.feed("1\r\n2\r\n3\r\n4");
        f.feed("\x1b[1S");   CHECK_EQ(screen(f), std::string("2|3|4||"));
        f.feed("\x1b[2T");   CHECK_EQ(screen(f), std::string("||2|3|"));
        CHECK(at(f, 3, 1));                                  // SU and SD leave the cursor where it is
    }

    // ================================================================================================ DECSTR

    {   // the soft reset: the pen, the modes, the sets, the region and the saved cursor go back; the screen and the cursor stay
        Emulator e(5, 10);
        e.feed("abc\x1b[3;4r\x1b[?6h\x1b[4h\x1b[1;31m\x1b(0\x1b[?25l");
        e.feed("\x1b[!p");
        CHECK_EQ(line(e, 0), std::string("abc"));
        CHECK(e.cursor().visible);
        e.feed("\x1b[1;1H" "q");                             // origin mode off (CUP is absolute), insert off, ASCII, plain
        CHECK_EQ(e.cell(0, 0).ch, (char32_t)U'q');
        CHECK_EQ(line(e, 0), std::string("qbc"));
        CHECK_EQ(e.cell(0, 0).attrs, (uint16_t)0);
        CHECK(e.cell(0, 0).fg == Color());
        e.feed("\x1b[5;1H\n");
        CHECK_EQ(line(e, 0), std::string());                 // no region any more: the whole screen scrolled
    }

    if (failures == 0) std::printf("all vt100 tests passed\n");
    return failures == 0 ? 0 : 1;
}
