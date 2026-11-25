// The VT52 mode: entered from ANSI mode with CSI ? 2 l (DECANM reset), left with ESC <. In it the terminal understands
// only the VT52 escape sequences; an ESC followed by anything else is dropped together with that character, and CSI
// sequences are not interpreted at all. There is no scrolling region and no auto wrap.
//
// Where the VT52 manual and xterm differ, or the manual is silent, the test does not assert (comments say which).
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

const char* VT52 = "\x1b[?2l";        // enter
const char* ANSI = "\x1b<";           // leave

// a screen of 5 rows of 10 characters, each row with its own letter, in VT52 mode, cursor at home
void filled(Emulator& e) {
    e.feed("\x1b[2J\x1b[H");
    for (int r = 0; r < 5; r++) e.feed("\x1b[" + std::to_string(r + 1) + ";1H" + std::string(10, (char)('a' + r)));
    e.feed("\x1b[H");
    e.feed(VT52);
}

}  // namespace

int main() {
    // ---- the mode switch
    {
        Emulator e(5, 10);
        e.feed(VT52);
        e.feed("\x1b[2J");                                   // no CSI here: ESC [ is dropped and "2J" is text
        CHECK_EQ(line(e, 0), std::string("2J"));
        CHECK(at(e, 0, 2));
        e.feed(ANSI);
        e.feed("\x1b[2J\x1b[H");                             // and ANSI again
        CHECK_EQ(screen(e), std::string("|||||"));
        CHECK(at(e, 0, 0));
    }
    {   // ESC < in ANSI mode is an unknown sequence: nothing happens, nothing is printed
        Emulator e(3, 10);
        e.feed("a\x1b<" "b");
        CHECK_EQ(line(e, 0), std::string("ab"));
        e.feed("\x1b[?2l");                                  // enter
        e.feed("c");
        CHECK_EQ(line(e, 0), std::string("abc"));
        e.feed("\x1b<\x1b<" "d");                            // leaving twice is harmless
        CHECK_EQ(line(e, 0), std::string("abcd"));
        e.feed("\x1b[?2h");                                  // DECANM set while in ANSI mode: stays ANSI
        e.feed("\x1b[1;1H" "x");
        CHECK_EQ(line(e, 0), std::string("xbcd"));
    }
    {   // the cursor does not move when the mode is entered or left
        Emulator e(5, 10);
        e.feed("\x1b[3;4H");
        e.feed(VT52);
        CHECK(at(e, 2, 3));
        e.feed(ANSI);
        CHECK(at(e, 2, 3));
    }

    // ---- cursor movement: ESC A B C D H
    {
        Emulator e(5, 10);
        e.feed(VT52);
        e.feed("\x1bY#&");                                   // row 3, column 6 (the characters '#' and '&' are 35 and 38; minus 32)
        CHECK(at(e, 3, 6));
        e.feed("\x1b" "A");   CHECK(at(e, 2, 6));
        e.feed("\x1b" "B");   CHECK(at(e, 3, 6));
        e.feed("\x1b" "C");   CHECK(at(e, 3, 7));
        e.feed("\x1b" "D");   CHECK(at(e, 3, 6));
        e.feed("\x1b" "H");   CHECK(at(e, 0, 0));
    }
    {   // they stop at the edges and never scroll
        Emulator e(3, 4);
        e.feed(VT52);
        e.feed("ab\x1b" "A\x1b" "A\x1b" "A");
        CHECK(at(e, 0, 2));
        e.feed("\x1b" "D\x1b" "D\x1b" "D\x1b" "D");
        CHECK(at(e, 0, 0));
        e.feed("\x1b" "C\x1b" "C\x1b" "C\x1b" "C\x1b" "C");
        CHECK(at(e, 0, 3));
        e.feed("\x1b" "B\x1b" "B\x1b" "B\x1b" "B");
        CHECK(at(e, 2, 3));
        CHECK_EQ(screen(e), std::string("ab|||"));            // nothing scrolled
        CHECK_EQ(e.scrollback_size(), (size_t)0);
    }
    {   // the control characters still work
        Emulator e(3, 20);
        e.feed(VT52);
        e.feed("ab\r\ncd\tx\bX\x07");
        CHECK_EQ(line(e, 0), std::string("ab"));
        CHECK_EQ(line(e, 1), std::string("cd      X"));
        CHECK_EQ(e.bell_count(), 1);
    }
    {   // no auto wrap: characters past the right edge pile up on the last column
        Emulator e(2, 5);
        e.feed(VT52);
        e.feed("abcdefgh");
        CHECK_EQ(line(e, 0), std::string("abcdh"));
        CHECK_EQ(line(e, 1), std::string());
        CHECK(at(e, 0, 4));
    }
    {   // a line feed at the bottom scrolls (VT52: the screen scrolls up one line)
        Emulator e(3, 4);
        e.feed(VT52);
        e.feed("a\r\nb\r\nc\r\nd");
        CHECK_EQ(screen(e), std::string("b|c|d|"));
    }

    // ---- ESC I reverse line feed
    {
        Emulator e(4, 5);
        e.feed(VT52);
        e.feed("a\r\nb\r\nc\r\nd\x1b" "H\x1b" "I");           // at the top: the screen scrolls down
        CHECK_EQ(screen(e), std::string("|a|b|c|"));
        CHECK(at(e, 0, 0));
        e.feed("\x1bY\"\x20\x1b" "I");                       // from row 2: just up
        CHECK(at(e, 1, 0));
        CHECK_EQ(screen(e), std::string("|a|b|c|"));
    }

    // ---- ESC J and ESC K
    {
        Emulator e(5, 10);
        filled(e);
        e.feed("\x1bY\"%\x1b" "K");                          // row 2, column 5: erase to the end of the line, the cursor cell included
        CHECK_EQ(line(e, 2), std::string("ccccc"));
        CHECK_EQ(line(e, 3), std::string(10, 'd'));
        CHECK(at(e, 2, 5));
        e.feed("\x1b" "J");                                  // erase to the end of the screen
        CHECK_EQ(screen(e), std::string(std::string(10, 'a') + "|" + std::string(10, 'b') + "|ccccc|||"));
        CHECK(at(e, 2, 5));
        e.feed("\x1b" "H\x1b" "J");
        CHECK_EQ(screen(e), std::string("|||||"));
    }

    // ---- ESC Y direct cursor addressing
    {
        Emulator e(24, 80);
        e.feed(VT52);
        e.feed("\x1bY  ");           CHECK(at(e, 0, 0));
        e.feed("\x1bY\x37\x4f");     CHECK(at(e, 23, 47));   // '7' = 55 - 32 = 23, 'O' = 79 - 32 = 47
        e.feed("\x1bY" "(" "A");     CHECK(at(e, 8, 33));
        // out of range values: the cursor goes to the edge (the VT52 manual does not say; xterm clamps)
        e.feed("\x1bY\x7e\x7e");     CHECK(at(e, 23, 79));      // (0x7f itself is DEL, which the parser ignores everywhere)
        e.feed("\x1bY\x30 ");        CHECK(at(e, 16, 0));
        e.feed("\x1bY\x60\x20");     CHECK(at(e, 23, 0));
        e.feed("\x1bY \x70");        CHECK(at(e, 0, 79));
    }
    {   // the two bytes after ESC Y are positions even when they look like controls or a letter, and the parts may arrive separately
        Emulator e(24, 80);
        e.feed(VT52);
        e.feed("\x1b");
        e.feed("Y");
        e.feed("\x28");
        e.feed("\x30");
        CHECK(at(e, 8, 16));
        e.feed("Q");                                          // an ordinary character again
        CHECK_EQ(e.cell(8, 16).ch, (char32_t)U'Q');
        CHECK(at(e, 8, 17));
    }
    {   // a position clears a pending wrap that never happens: after a character in the last column the next one overwrites
        Emulator e(3, 5);
        e.feed(VT52);
        e.feed("\x1bY\x20\x24" "AB");                        // row 0, column 4: A, then B on top of it
        CHECK_EQ(line(e, 0), std::string("    B"));
        CHECK(at(e, 0, 4));
    }

    // ---- ESC Z identify: ESC / Z
    {
        Emulator e(3, 10);
        e.feed(VT52);
        e.feed("\x1b" "Z");
        CHECK_EQ(e.take_reply(), std::string("\x1b/Z"));
        e.feed(ANSI);
        e.feed("\x1b" "Z");                                  // in ANSI mode DECID gives the DA answer
        CHECK_EQ(e.take_reply(), std::string("\x1b[?1;2c"));
    }

    // ---- ESC F and ESC G: the graphics character set
    {
        Emulator e(3, 40);
        e.feed(VT52);
        e.feed("\x1b" "F");
        e.feed("qlkxmj");
        e.feed("\x1b" "G");
        e.feed("qlk");
        // The VT52 graphics set is not the DEC special graphics set (it has fractions, arrows and "scan line" characters),
        // but the line characters of the VT100 set are what programs expect to see for the box drawing letters: accept
        // those. What is certain: the letters change, and after ESC G they are plain again.
        CHECK(e.cell(0, 0).ch != U'q');
        CHECK(e.cell(0, 0).ch == U'─' || (e.cell(0, 0).ch >= 0x23BA && e.cell(0, 0).ch <= 0x23BD));
        CHECK(e.cell(0, 1).ch != U'l');
        CHECK(e.cell(0, 2).ch != U'k');
        CHECK_EQ(e.cell(0, 6).ch, (char32_t)U'q');
        CHECK_EQ(e.cell(0, 7).ch, (char32_t)U'l');
        CHECK_EQ(e.cell(0, 8).ch, (char32_t)U'k');
        e.feed("\r\n\x1b" "F" "AZ09 [\\]\x1b" "G");            // the characters below 0x5E are not part of the set
        CHECK_EQ(line(e, 1), std::string("AZ09 [\\]"));
    }

    // ---- ESC = and ESC >: alternate keypad
    {
        Emulator e(3, 10);
        e.feed(VT52);
        CHECK(!e.application_keypad());
        e.feed("\x1b=");
        CHECK(e.application_keypad());
        e.feed("\x1b>");
        CHECK(!e.application_keypad());
    }

    // ---- what is not VT52: CSI is not interpreted, unknown escapes drop their final character
    {
        Emulator e(5, 20);
        e.feed(VT52);
        e.feed("\x1b[1;31mred\x1b[0m");
        CHECK_EQ(line(e, 0), std::string("1;31mred0m"));     // ESC [ gone, the rest is text
        CHECK(e.cell(0, 0).fg == Color());                   // and no colour was set
        e.feed("\x1b[5;5H");
        CHECK_EQ(line(e, 0), std::string("1;31mred0m5;5H"));
        CHECK(at(e, 0, 14));                                 // the cursor did not move by the CSI: it is just after the text
    }
    {   // an unknown ESC X is dropped with its character
        Emulator e(3, 20);
        e.feed(VT52);
        // (the ANSI-only sequences ESC 7, ESC 8 and ESC c are unknown here too; ESC % # ( with their own final character are
        // left out: the manual does not say whether the byte after them is part of the sequence)
        e.feed("a\x1b" "Qb\x1b" "7c\x1b" "8d\x1b" "ce");
        CHECK_EQ(line(e, 0), std::string("abcde"));
        CHECK(at(e, 0, 5));
        e.feed(ANSI);
        e.feed("\x1b[1;1H\x1b" "7\x1b[2;2H\x1b" "8x");      // the ANSI sequences are back (DECSC / DECRC here)
        CHECK_EQ(line(e, 0), std::string("xbcde"));
    }
    // ---- the sequences are the same when a read ends in the middle of one, at every split point (also in the one that enters the mode)
    // (what the region, origin mode, character sets and tab stops of ANSI mode mean after entering VT52 mode is left open: the manual
    // has none of them, xterm keeps some)
    {
        const std::string text = std::string(VT52) + "\x1b" "Y%+Z" "\x1b" "Y  W";
        for (size_t cut = 0; cut <= text.size(); cut++) {
            Emulator e(10, 20);
            e.feed(text.substr(0, cut));
            e.feed(text.substr(cut));
            CHECK_EQ(e.cell(5, 11).ch, (char32_t)U'Z');      // '%' = 37 - 32 = 5, '+' = 43 - 32 = 11
            CHECK_EQ(e.cell(0, 0).ch, (char32_t)U'W');
        }
    }

    if (failures == 0) std::printf("all vt52 tests passed\n");
    return failures == 0 ? 0 : 1;
}
