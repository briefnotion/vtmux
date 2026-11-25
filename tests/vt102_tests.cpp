// What the VT102 added to the VT100: insert and delete character (ICH, DCH), insert and delete line (IL, DL), the insert
// mode (IRM, CSI 4 h) and the new line mode (LNM, CSI 20 h). Written from the VT102 manual and from what xterm does.
//
// Ambiguous (terminals differ, so no test asserts it): the column the cursor is left in after IL and DL. The VT102
// manual and ECMA-48 move it to column 1; xterm and tmux leave it. See tests/conformance/FINDINGS.md.
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

}  // namespace

int main() {
    // ================================================================================================ ICH: insert character

    {
        Emulator e(2, 10);
        e.feed("0123456789\x1b[1;4H\x1b[2@");
        CHECK_EQ(line(e, 0), std::string("012  34567"));     // two blanks in at column 4, the rest moves right, 8 and 9 fall off
        CHECK(at(e, 0, 3));                                  // the cursor stays
        e.feed("\x1b[@");                                    // no parameter is 1
        CHECK_EQ(line(e, 0), std::string("012   3456"));
        e.feed("\x1b[0@");                                   // 0 is 1
        CHECK_EQ(line(e, 0), std::string("012    345"));
        e.feed("\x1b[99@");                                  // more than fit: the rest of the line is blank from the cursor
        CHECK_EQ(line(e, 0), std::string("012"));
        CHECK(at(e, 0, 3));
        e.feed("\x1b[2;1H" "abc\x1b[10G\x1b[5@");            // at the last column: that one cell is blanked
        CHECK_EQ(line(e, 1), std::string("abc"));
        e.feed("\x1b[2;1H\x1b[2@");
        CHECK_EQ(line(e, 1), std::string("  abc"));
        CHECK_EQ(line(e, 0), std::string("012"));            // the other line is untouched
    }
    {   // ICH inserts blanks with the current background, and keeps the attributes of what it moves
        Emulator e(1, 8);
        e.feed("\x1b[1mab\x1b[0mcd\x1b[1;2H\x1b[44m\x1b[2@");
        CHECK_EQ(line(e, 0), std::string("a  bcd"));
        CHECK(e.cell(0, 1).bg == Color::index(4));
        CHECK(e.cell(0, 2).bg == Color::index(4));
        CHECK(e.cell(0, 0).bg == Color());
        CHECK(e.cell(0, 3).bg == Color());
        CHECK(e.cell(0, 3).attrs & ATTR_BOLD);               // the b that moved is still bold
        CHECK(!(e.cell(0, 4).attrs & ATTR_BOLD));
    }
    // (ICH, DCH and the other editing sequences with the wrap pending at the right margin: xterm leaves the cursor on the last
    // column, tmux and pyte one past it, so they disagree about what is deleted and whether the next character wraps: not asserted,
    // see tests/conformance/FINDINGS.md "the deferred wrap")

    {   // a wide character cut by the insert is blanked whole
        Emulator e(1, 6);
        e.feed("ab中cd\x1b[1;3H\x1b[1@");                    // the wide char starts at column 3 (0-based 2), so it is pushed whole
        CHECK_EQ(e.cell(0, 2).ch, (char32_t)U' ');
        CHECK_EQ(e.cell(0, 3).ch, (char32_t)0x4E2D);
        CHECK(e.cell(0, 3).attrs & ATTR_WIDE);
        CHECK(e.cell(0, 4).attrs & ATTR_WIDE_TAIL);
        CHECK_EQ(e.cell(0, 5).ch, (char32_t)U'c');           // 'd' fell off the end
    }

    // ================================================================================================ DCH: delete character

    {
        Emulator e(2, 10);
        e.feed("0123456789\x1b[1;4H\x1b[2P");
        CHECK_EQ(line(e, 0), std::string("01256789"));
        CHECK(at(e, 0, 3));
        e.feed("\x1b[P");
        CHECK_EQ(line(e, 0), std::string("0126789"));
        e.feed("\x1b[0P");
        CHECK_EQ(line(e, 0), std::string("012789"));
        e.feed("\x1b[99P");
        CHECK_EQ(line(e, 0), std::string("012"));
        CHECK(at(e, 0, 3));
        e.feed("\x1b[2;1Habcdef\x1b[2;3H\x1b[3P");
        CHECK_EQ(line(e, 1), std::string("abf"));
    }
    {   // DCH fills from the right with the current background
        Emulator e(1, 8);
        e.feed("abcdefgh\x1b[44m\x1b[1;2H\x1b[3P");
        CHECK_EQ(line(e, 0), std::string("aefgh"));
        CHECK(e.cell(0, 4).bg == Color());
        CHECK(e.cell(0, 5).bg == Color::index(4));
        CHECK(e.cell(0, 7).bg == Color::index(4));
    }
    {   // DCH next to a wide character blanks the half that would be left
        Emulator e(1, 8);
        e.feed("a中b\x1b[1;3H\x1b[1P");                       // the cursor is on the second half of the wide character
        CHECK_EQ(e.cell(0, 0).ch, (char32_t)U'a');
        CHECK_EQ(e.cell(0, 1).ch, (char32_t)U' ');           // the first half is blanked: no half character stays
        CHECK_EQ(e.cell(0, 2).ch, (char32_t)U'b');
    }

    // ================================================================================================ IL, DL

    {   // IL: lines from the cursor down move down, the last one is lost, the new one is blank
        Emulator e(5, 6);
        for (int r = 0; r < 5; r++) e.feed("\x1b[" + std::to_string(r + 1) + ";1H" + std::string(1, (char)('A' + r)));
        e.feed("\x1b[2;1H\x1b[L");
        CHECK_EQ(screen(e), std::string("A||B|C|D|"));
        e.feed("\x1b[2;1H\x1b[2L");
        CHECK_EQ(line(e, 0), std::string("A"));
        CHECK_EQ(line(e, 1), std::string());
        CHECK_EQ(line(e, 2), std::string());
        CHECK_EQ(line(e, 3), std::string());
        CHECK_EQ(line(e, 4), std::string("B"));
        e.feed("\x1b[1;1H\x1b[99L");                         // more than there are: the whole screen is cleared
        CHECK_EQ(screen(e), std::string("|||||"));
    }
    {   // DL: lines below move up, blank lines come in at the bottom
        Emulator e(5, 6);
        for (int r = 0; r < 5; r++) e.feed("\x1b[" + std::to_string(r + 1) + ";1H" + std::string(1, (char)('A' + r)));
        e.feed("\x1b[2;1H\x1b[M");
        CHECK_EQ(screen(e), std::string("A|C|D|E||"));
        e.feed("\x1b[2;1H\x1b[2M");
        CHECK_EQ(screen(e), std::string("A|E||||"));
        e.feed("\x1b[5;1H\x1b[M");                           // on the last line: that line goes (it was blank)
        CHECK_EQ(screen(e), std::string("A|E||||"));
        e.feed("\x1b[1;1H\x1b[0M");                          // 0 is 1
        CHECK_EQ(screen(e), std::string("E|||||"));
    }
    {   // IL and DL work only inside the scroll region, and keep within it
        Emulator e(7, 4);
        for (int r = 0; r < 7; r++) e.feed("\x1b[" + std::to_string(r + 1) + ";1H" + std::string(1, (char)('A' + r)));
        e.feed("\x1b[3;5r");                                 // rows C D E
        e.feed("\x1b[3;1H\x1b[L");
        CHECK_EQ(screen(e), std::string("A|B||C|D|F|G|"));
        e.feed("\x1b[5;1H\x1b[2L");                          // at the bottom margin: only that line is blanked
        CHECK_EQ(line(e, 4), std::string());
        CHECK_EQ(line(e, 5), std::string("F"));
        e.feed("\x1b[3;1H\x1b[M");
        CHECK_EQ(line(e, 2), std::string("C"));
        CHECK_EQ(line(e, 3), std::string());
        CHECK_EQ(line(e, 5), std::string("F"));
        e.feed("\x1b[1;1H\x1b[L");                           // above the region: nothing happens
        CHECK_EQ(line(e, 0), std::string("A"));
        CHECK_EQ(line(e, 1), std::string("B"));
        e.feed("\x1b[7;1H\x1b[M");                           // below it: nothing either
        CHECK_EQ(line(e, 6), std::string("G"));
    }
    {   // IL and DL do not put anything into the scrollback, even over the whole screen: only lines that scroll off the top do
        // (xterm and tmux agree on this: a deleted line is deleted, not scrolled)
        Emulator e(3, 4, 50);
        e.feed("a\r\nb\r\nc\x1b[1;1H\x1b[M\x1b[L");
        CHECK_EQ(e.scrollback_size(), (size_t)0);
    }
    {   // IL and DL use the current background for the lines they bring in
        Emulator e(3, 4);
        e.feed("a\r\nb\r\nc\x1b[43m\x1b[1;1H\x1b[M");
        CHECK(e.cell(2, 0).bg == Color::index(3));
        CHECK(e.cell(2, 3).bg == Color::index(3));
        e.feed("\x1b[1;1H\x1b[L");
        CHECK(e.cell(0, 3).bg == Color::index(3));
    }

    // ================================================================================================ IRM: insert mode

    {
        Emulator e(2, 10);
        e.feed("abcdef\x1b[1;3H\x1b[4h" "XY");
        CHECK_EQ(line(e, 0), std::string("abXYcdef"));       // characters are inserted, the rest moves right
        CHECK(at(e, 0, 4));
        e.feed("\x1b[4l" "Z");                               // replace mode again
        CHECK_EQ(line(e, 0), std::string("abXYZdef"));
    }
    {   // what moves past the right edge is lost, the wrap still happens at the last column
        Emulator e(2, 6);
        e.feed("abcdef\x1b[1;1H\x1b[4h" "XY");
        CHECK_EQ(line(e, 0), std::string("XYabcd"));
        CHECK_EQ(line(e, 1), std::string());
        e.feed("1234");                                      // writing on to the end: the last column is where the wrap waits
        CHECK_EQ(line(e, 0), std::string("XY1234"));
        e.feed("5");
        CHECK_EQ(line(e, 1), std::string("5"));
        CHECK(at(e, 1, 1));
    }
    {   // insert mode inserts the pen's attributes and background for the new character only
        Emulator e(1, 8);
        e.feed("abcd\x1b[1;2H\x1b[4h\x1b[1;31mX");
        CHECK_EQ(line(e, 0), std::string("aXbcd"));
        CHECK(e.cell(0, 1).attrs & ATTR_BOLD);
        CHECK(e.cell(0, 1).fg == Color::index(1));
        CHECK(!(e.cell(0, 2).attrs & ATTR_BOLD));
        CHECK(e.cell(0, 2).fg == Color());
    }
    {   // a wide character is inserted as two cells, and cut when it does not fit
        Emulator e(1, 6);
        e.feed("abcd\x1b[1;1H\x1b[4h" "中");
        CHECK_EQ(e.cell(0, 0).ch, (char32_t)0x4E2D);
        CHECK(e.cell(0, 1).attrs & ATTR_WIDE_TAIL);
        CHECK_EQ(e.cell(0, 2).ch, (char32_t)U'a');
        CHECK_EQ(e.cell(0, 5).ch, (char32_t)U'd');
    }
    {   // IRM does not change what the cursor movement and the controls do
        Emulator e(2, 10);
        e.feed("\x1b[4h" "ab\tc\r\nd");
        CHECK_EQ(line(e, 0), std::string("ab      c"));
        CHECK_EQ(line(e, 1), std::string("d"));
    }

    // ================================================================================================ LNM: line feed / new line mode

    {   // in new line mode the line feed also returns the carriage; VT and FF are line feeds too
        Emulator e(5, 20);
        e.feed("ab\n" "c");
        CHECK_EQ(line(e, 1), std::string("  c"));            // normal mode: LF keeps the column
        e.feed("\x1b[20h");
        e.feed("\r\n" "dd\n" "e");
        CHECK_EQ(line(e, 2), std::string("dd"));
        CHECK_EQ(line(e, 3), std::string("e"));
        CHECK(at(e, 3, 1));
        e.feed("\x0b" "f");
        CHECK_EQ(line(e, 4), std::string("f"));
        e.feed("\x1b[20l\n" "g");
        CHECK_EQ(line(e, 3), std::string("f"));              // scrolled (the screen was full): the line moved up
        CHECK_EQ(e.cell(4, 1).ch, (char32_t)U'g');           // and LF alone again keeps the column (1, after the f)
    }
    {   // IND (ESC D) is a line feed without the carriage return even in new line mode; NEL (ESC E) always returns the carriage
        Emulator e(4, 10);
        e.feed("\x1b[20h" "ab\x1b" "Dc\x1b" "Ed");
        CHECK_EQ(line(e, 1), std::string("  c"));
        CHECK_EQ(line(e, 2), std::string("d"));
    }
    {   // a full reset clears LNM (what DECSTR does to it is left open: xterm leaves it alone)
        Emulator e(3, 10);
        e.feed("\x1b[20h\x1b" "c" "ab\n" "c");
        CHECK_EQ(line(e, 1), std::string("  c"));
    }

    if (failures == 0) std::printf("all vt102 tests passed\n");
    return failures == 0 ? 0 : 1;
}
