// Tests for the emulator: exact behaviours of every part, and invariants that must hold for any input at all.
// sources: ../src/vtparser.cpp ../src/emulator.cpp ../src/width.cpp
#include <random>
#include <string>

#include "check.h"
#include "../src/emulator.h"
#include "../src/width.h"

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

// a row as text, with the second half of a wide character left out and trailing spaces cut
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

int g_caller_line = 0;
#define INV_CHECK(c) do { if (!(c)) { std::printf("FAIL invariant (called at line %d): %s\n", g_caller_line, #c); failures++; } } while (0)
// every invariant that must hold whatever was fed
void check_invariants_at(const Emulator& e) {
    const Cursor c = e.cursor();
    INV_CHECK(c.row >= 0 && c.row < e.rows());
    INV_CHECK(c.col >= 0 && c.col < e.cols());
    for (int r = 0; r < e.rows(); r++) {
        for (int k = 0; k < e.cols(); k++) {
            const Cell& cell = e.cell(r, k);
            if (cell.attrs & ATTR_WIDE) {
                INV_CHECK(k + 1 < e.cols());
                if (k + 1 < e.cols()) INV_CHECK(e.cell(r, k + 1).attrs & ATTR_WIDE_TAIL);
            }
            if (cell.attrs & ATTR_WIDE_TAIL) {
                INV_CHECK(k > 0);
                if (k > 0) INV_CHECK(e.cell(r, k - 1).attrs & ATTR_WIDE);
            }
        }
    }
    INV_CHECK(e.scrollback_size() <= e.scrollback_limit());
}

#define check_invariants(e) do { g_caller_line = __LINE__; check_invariants_at(e); } while (0)

bool same_screen(const Emulator& a, const Emulator& b) {
    if (a.rows() != b.rows() || a.cols() != b.cols() || a.cursor() != b.cursor() || a.alt_screen() != b.alt_screen()) return false;
    for (int r = 0; r < a.rows(); r++)
        for (int c = 0; c < a.cols(); c++)
            if (a.cell(r, c) != b.cell(r, c)) return false;
    return a.scrollback_size() == b.scrollback_size() && a.title() == b.title();
}

}  // namespace

int main() {
    // ---- OSC 52: the program sets the clipboard; only a valid write is kept, as the whole sequence for the real terminal
    {
        Emulator e(5, 20);
        CHECK(!e.has_clipboard());
        e.feed("\x1b]52;c;aGVsbG8=\x07");
        CHECK(e.has_clipboard());
        std::vector<std::string> w = e.take_clipboard();
        CHECK_EQ(w.size(), (size_t)1);
        CHECK_EQ(w[0], std::string("\x1b]52;c;aGVsbG8=\x07"));
        CHECK(!e.has_clipboard());
        e.feed("\x1b]52;;aGk=\x1b\\");                                     // ST, and no selection named: c
        w = e.take_clipboard();
        CHECK_EQ(w.size(), (size_t)1);
        CHECK_EQ(w[0], std::string("\x1b]52;c;aGk=\x07"));
        e.feed("\x1b]52;pc;aGk=\x07");                                     // other selections are passed on as they are
        w = e.take_clipboard();
        CHECK(w.size() == 1 && w[0] == "\x1b]52;pc;aGk=\x07");
        // not kept: a query, a clear, the "!" micro sends first, a broken selection, text that is not base64, no data field at all
        e.feed("\x1b]52;c;?\x07\x1b]52;c;\x07\x1b]52;c;!\x07\x1b]52;x;aGk=\x07\x1b]52;c;aGk\x1b]0;t\x07\x1b]52;c\x07\x1b]52;c;a b\x07");
        CHECK(!e.has_clipboard());
        CHECK_EQ(e.title(), std::string("t"));                               // (and the string with an ESC in it ended there)
        // nothing in the data can end the sequence early or add one of its own
        e.feed("\x1b]52;c;aGk=\x07\x1b]52;c;aG\x1b\\k\x07");
        w = e.take_clipboard();
        CHECK(w.size() >= 1 && w[0] == "\x1b]52;c;aGk=\x07");
        for (const std::string& one : w) CHECK(one.find('\x1b', 1) == std::string::npos);
        // a flood: only the last four wait
        for (int i = 0; i < 10; i++) e.feed("\x1b]52;c;" + std::string(1, (char)('A' + i)) + "A==\x07");
        w = e.take_clipboard();
        CHECK_EQ(w.size(), (size_t)4);
        CHECK_EQ(w[3], std::string("\x1b]52;c;JA==\x07"));
        // a long one, split anywhere
        const std::string big(300000, 'A');
        Emulator f(5, 20);
        const std::string msg = "\x1b]52;c;" + big + "\x07";
        for (size_t i = 0; i < msg.size(); i += 7777) f.feed(msg.substr(i, 7777));
        w = f.take_clipboard();
        CHECK(w.size() == 1 && w[0].size() == big.size() + 8);
    }

    // ---- DECCOLM: 80 or 132 columns of the pane, but only when the program allowed it (mode 40, as xterm needs)
    {
        Emulator e(5, 100);
        e.feed("abc\x1b[?3l");                                  // not allowed: it clears and keeps the width (this is what `reset` sends)
        CHECK_EQ(e.cols(), 100);
        e.feed("\x1b[?40h\x1b[?3l");
        CHECK_EQ(e.cols(), 80);
        CHECK_EQ(e.rows(), 5);
        e.feed("x");
        CHECK_EQ((int)e.cell(0, 0).ch, (int)U'x');
        e.feed("\x1b[1;100H");                                  // the cursor cannot go past the 80th column
        CHECK_EQ(e.cursor().col, 79);
        e.feed("\x1b[?3h");                                     // 132 does not fit a pane of 100: it takes all of it
        CHECK_EQ(e.cols(), 100);
        e.feed("\x1b[?3l\x1b[?40l");                           // mode 40 off: the pane's width is back
        CHECK_EQ(e.cols(), 100);
        e.feed("\x1b[?40h\x1b[?3l\x1b" "c");                     // a full reset too
        CHECK_EQ(e.cols(), 100);
        e.feed("\x1b[?3l");                                     // and the permission went with it
        CHECK_EQ(e.cols(), 100);

        Emulator w(4, 200);
        w.feed("\x1b[?40h\x1b[?3h");
        CHECK_EQ(w.cols(), 132);
        w.feed("\x1b[?3l");
        CHECK_EQ(w.cols(), 80);
        w.resize(4, 150);                                       // the pane is resized: that is the width now, whatever DECCOLM said
        CHECK_EQ(w.cols(), 150);
        w.feed("\x1b[?3l");
        CHECK_EQ(w.cols(), 80);                                 // (the permission stays until it is taken away)
        w.resize(4, 60);
        w.feed("\x1b[?3l");
        CHECK_EQ(w.cols(), 60);                                 // a pane narrower than 80 keeps its width
    }

    // ---- found by the conformance run: history with a top margin at row 1; SGR out of range ignored; VPR stops at the margin
    {
        Emulator e(3, 4);
        e.feed("a\r\nb\r\nc\x1b[1;2r\x1b[2;1H\n\n");          // a region from the first row to the second: two lines scroll off its top
        CHECK_EQ(e.scrollback_size(), (size_t)2);
        Emulator d(3, 4);
        d.feed("a\r\nb\r\nc\x1b[1;1H\x1b[2M");                  // deleted lines are not scrolled off
        CHECK_EQ(d.scrollback_size(), (size_t)0);
        Emulator r(3, 4);
        r.feed("a\r\nb\r\nc\x1b[2;3r\x1b[2;1H\x1b[9e");
        CHECK_EQ(r.cursor().row, 2);
        Emulator g(2, 4);
        g.feed("\x1b[31m\x1b[38;5;300mG\x1b[48;2;300;0;0mH");
        CHECK(g.cell(0, 0).fg == Color::index(1));              // the red from before stays: the bad setting is ignored
        CHECK(g.cell(0, 1).bg == Color());
    }

    // ---- the character widths
    CHECK_EQ(char_width(U'a'), 1);
    CHECK_EQ(char_width(0xE9), 1);
    CHECK_EQ(char_width(0x4E2D), 2);                         // a Chinese character
    CHECK_EQ(char_width(0xFF21), 2);                         // a fullwidth A
    CHECK_EQ(char_width(0xFF71), 1);                         // a halfwidth katakana
    CHECK_EQ(char_width(0x3000), 2);                         // an ideographic space
    CHECK_EQ(char_width(0xAC00), 2);                         // a Hangul syllable
    CHECK_EQ(char_width(0x1F600), 2);                        // an emoji
    CHECK_EQ(char_width(0x301), 0);                          // a combining accent
    CHECK_EQ(char_width(0x200D), 0);                         // a zero-width joiner
    CHECK_EQ(char_width(0xFE0F), 0);                         // a variation selector
    CHECK_EQ(char_width(0x2500), 1);                         // a box drawing line
    CHECK_EQ(char_width(0x2588), 1);                         // a full block
    CHECK_EQ(char_width(0x0A), 0);                           // a control

    // ---- printing, and the wrap that waits
    {
        Emulator e(3, 10);
        e.feed("0123456789");
        CHECK_EQ(line(e, 0), std::string("0123456789"));
        CHECK(e.cursor().row == 0 && e.cursor().col == 9);   // still on the last column: the wrap is pending
        e.feed("A");
        CHECK_EQ(line(e, 1), std::string("A"));              // now it wrapped
        CHECK(e.cursor().row == 1 && e.cursor().col == 1);
        e.feed("\r\n");
        e.feed("0123456789\r");                               // a CR while the wrap is pending: back to the start, no wrap
        CHECK(e.cursor().row == 2 && e.cursor().col == 0);
        e.feed("Z");
        CHECK_EQ(line(e, 2), std::string("Z123456789"));
    }
    {   // the wrap does not happen with auto wrap off: the last column is overwritten
        Emulator e(2, 5);
        e.feed("\x1b[?7l" "abcdefgh");
        CHECK_EQ(line(e, 0), std::string("abcdh"));
        CHECK(e.cursor().row == 0 && e.cursor().col == 4);
        e.feed("\x1b[?7h" "xyz");
        CHECK_EQ(line(e, 0), std::string("abcdx"));
        CHECK_EQ(line(e, 1), std::string("yz"));
    }

    // ---- control characters
    {
        Emulator e(2, 20);
        e.feed("ab\bX");
        CHECK_EQ(line(e, 0), std::string("aX"));
        e.feed("\r\n\tT");
        CHECK(e.cursor().row == 1 && e.cursor().col == 9);   // a tab goes to column 8, T is drawn there
        CHECK_EQ(line(e, 1), std::string("        T"));
        e.feed("\x07\x07");
        CHECK_EQ(e.bell_count(), 2);
        e.feed("\x1b[?7l\r\n\x1b[3g");
        CHECK(true);
    }

    // ---- scrolling and the scrollback
    {
        Emulator e(3, 6, 100);
        e.feed("one\r\ntwo\r\nthree\r\nfour\r\nfive");
        CHECK_EQ(screen(e), std::string("three|four|five|"));
        CHECK_EQ(e.scrollback_size(), (size_t)2);
        std::string first;
        for (const Cell& c : e.scrollback_line(0)) first += utf8(c.ch);
        CHECK(first.rfind("one", 0) == 0);                    // the oldest is at 0
        std::string second;
        for (const Cell& c : e.scrollback_line(1)) second += utf8(c.ch);
        CHECK(second.rfind("two", 0) == 0);
        e.feed("\x1b[3J");
        CHECK_EQ(e.scrollback_size(), (size_t)0);
    }
    {   // the scrollback has a limit
        Emulator e(2, 5, 3);
        for (int i = 0; i < 20; i++) e.feed("x\r\n");
        CHECK_EQ(e.scrollback_size(), (size_t)3);
        Emulator none(2, 5, 0);
        none.feed("a\r\nb\r\nc\r\n");
        CHECK_EQ(none.scrollback_size(), (size_t)0);
    }

    // ---- moving the cursor
    {
        Emulator e(5, 10);
        e.feed("\x1b[3;4H");
        CHECK(e.cursor().row == 2 && e.cursor().col == 3);
        e.feed("\x1b[A\x1b[2D");
        CHECK(e.cursor().row == 1 && e.cursor().col == 1);
        e.feed("\x1b[99A\x1b[99D");
        CHECK(e.cursor().row == 0 && e.cursor().col == 0);
        e.feed("\x1b[99B\x1b[99C");
        CHECK(e.cursor().row == 4 && e.cursor().col == 9);
        e.feed("\x1b[H");
        CHECK(e.cursor().row == 0 && e.cursor().col == 0);
        e.feed("\x1b[;5H");
        CHECK(e.cursor().row == 0 && e.cursor().col == 4);
        e.feed("\x1b[0A\x1b[0B");                              // a 0 is a 1: up (stopped at the top), then down one
        CHECK(e.cursor().row == 1);
        e.feed("\x1b[A");
        e.feed("\x1b[3G");
        CHECK(e.cursor().col == 2);
        e.feed("\x1b[4d");
        CHECK(e.cursor().row == 3 && e.cursor().col == 2);
        e.feed("\x1b[2E");                                       // down two, to the first column
        CHECK(e.cursor().row == 4 && e.cursor().col == 0);
        e.feed("\x1b[3F");
        CHECK(e.cursor().row == 1 && e.cursor().col == 0);
        e.feed("\x1b[2;3f");
        CHECK(e.cursor().row == 1 && e.cursor().col == 2);
        e.feed("\x1b[500;500H");
        CHECK(e.cursor().row == 4 && e.cursor().col == 9);
    }

    // ---- erasing
    {
        Emulator e(3, 8);
        e.feed("aaaaaaaa\r\nbbbbbbbb\r\ncccccccc");
        e.feed("\x1b[2;4H\x1b[K");                               // erase to the end of the line
        CHECK_EQ(screen(e), std::string("aaaaaaaa|bbb|cccccccc|"));
        e.feed("\x1b[1K");                                       // erase to the start, the cursor cell included
        CHECK_EQ(line(e, 1), std::string(""));
        e.feed("\x1b[3;3H\x1b[J");                               // erase to the end of the screen
        CHECK_EQ(screen(e), std::string("aaaaaaaa||cc|"));
        e.feed("\x1b[1J");                                       // erase from the start to the cursor
        CHECK_EQ(line(e, 0), std::string(""));
        CHECK_EQ(line(e, 2), std::string(""));
        e.feed("\x1b[2J");
        CHECK_EQ(screen(e), std::string("|||"));
        e.feed("\x1b[1;1Habcdef\x1b[1;3H\x1b[2X");               // erase two characters
        CHECK_EQ(line(e, 0), std::string("ab  ef"));
        e.feed("\x1b[1;1H\x1b[2K");
        CHECK_EQ(line(e, 0), std::string(""));
    }
    {   // erasing keeps the colour of the background (the current pen), and not the other attributes
        Emulator e(2, 6);
        e.feed("\x1b[44;1;31m\x1b[K");
        const Cell& c = e.cell(0, 3);
        CHECK(c.ch == U' ' && c.bg == Color::index(4) && c.attrs == 0 && c.fg == Color());
        e.feed("\x1b[0m\x1b[2J");
        CHECK(e.cell(1, 1).bg == Color());
    }

    // ---- inserting, deleting, repeating
    {
        Emulator e(2, 8);
        e.feed("abcdefgh\x1b[1;3H\x1b[2@");                      // insert two blanks at column 3
        CHECK_EQ(line(e, 0), std::string("ab  cdef"));
        e.feed("\x1b[2P");                                       // delete them again
        CHECK_EQ(line(e, 0), std::string("abcdef"));
        e.feed("\x1b[100P");                                     // delete more than there are
        CHECK_EQ(line(e, 0), std::string("ab"));
        e.feed("\r\x1b[2;1Hxy\x1b[3b");                          // repeat the last character
        CHECK_EQ(line(e, 1), std::string("xyyyy"));
        e.feed("\x1b[4h\r\x1b[1;1H12\x1b[4l");                   // insert mode
        CHECK_EQ(line(e, 0), std::string("12ab"));
    }
    {
        Emulator e(5, 4);
        e.feed("1\r\n2\r\n3\r\n4\r\n5");
        e.feed("\x1b[2;1H\x1b[L");                               // insert a line at row 2
        CHECK_EQ(screen(e), std::string("1||2|3|4|"));
        e.feed("\x1b[M");                                        // delete it again
        CHECK_EQ(screen(e), std::string("1|2|3|4||"));
        e.feed("\x1b[2M");
        CHECK_EQ(screen(e), std::string("1|4||||"));
        e.feed("\x1b[1;1H\x1b[S");                               // scroll up
        CHECK_EQ(screen(e), std::string("4|||||"));
        e.feed("\x1b[T");                                        // scroll down
        CHECK_EQ(screen(e), std::string("|4||||"));
    }

    // ---- scroll regions
    {
        Emulator e(6, 4);
        e.feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        e.feed("\x1b[2;4r");                                     // lines 2 to 4 scroll, and the cursor goes home
        CHECK(e.cursor().row == 0 && e.cursor().col == 0);
        e.feed("\x1b[4;1H\n");                                   // a line feed at the bottom of the region scrolls it
        CHECK_EQ(screen(e), std::string("1|3|4||5|6|"));
        e.feed("\x1b[2;1H\x1bM");                                // a reverse index at the top of the region scrolls it down
        CHECK_EQ(screen(e), std::string("1||3|4|5|6|"));
        e.feed("\x1b[6;1H\n");                                   // below the region: the line feed just stays at the bottom, nothing scrolls
        CHECK_EQ(screen(e), std::string("1||3|4|5|6|"));
        CHECK_EQ(e.scrollback_size(), (size_t)0);                // a region that is not the screen keeps no scrollback
        e.feed("\x1b[r");
        CHECK(e.cursor().row == 0);
        e.feed("\x1b[5;2r");                                     // top below bottom: ignored
        e.feed("\x1b[6;1H\n");
        CHECK_EQ(e.scrollback_size(), (size_t)1);                // so the whole screen scrolls
    }
    {   // the origin mode: positions are inside the region
        Emulator e(6, 10);
        e.feed("\x1b[3;5r\x1b[?6h\x1b[1;1H");
        CHECK(e.cursor().row == 2 && e.cursor().col == 0);
        e.feed("\x1b[99;1H");
        CHECK(e.cursor().row == 4);                              // cannot leave the region
        e.feed("\x1b[6n");
        CHECK_EQ(e.take_reply(), std::string("\x1b[3;1R"));      // and the report is relative to it
        e.feed("\x1b[?6l");
        CHECK(e.cursor().row == 0);
        e.feed("\x1b[6n");
        CHECK_EQ(e.take_reply(), std::string("\x1b[1;1R"));
    }
    {   // up and down stop at the margins when the cursor is inside the region
        Emulator e(8, 4);
        e.feed("\x1b[3;6r\x1b[4;1H\x1b[10A");
        CHECK(e.cursor().row == 2);
        e.feed("\x1b[10B");
        CHECK(e.cursor().row == 5);
        e.feed("\x1b[1;1H\x1b[10B");                             // from outside the region (above it) the region's bottom is the limit only if inside; here it passes
        CHECK(e.cursor().row == 5);
    }

    // ---- tabs
    {
        Emulator e(2, 30);
        e.feed("\t");
        CHECK(e.cursor().col == 8);
        e.feed("\t\t");
        CHECK(e.cursor().col == 24);
        e.feed("\t\t");
        CHECK(e.cursor().col == 29);                             // the last column
        e.feed("\x1b[3g\r\t");
        CHECK(e.cursor().col == 29);                             // no stops left
        e.feed("\r\x1b[5C\x1bH\r\t");                            // set one at column 6
        CHECK(e.cursor().col == 5);
        e.feed("\x1b[0g\r\t");                                   // clear it again (it was set at the cursor, column 6: 1-based)
        CHECK(true);
        e.feed("\x1b[1;1H\x1b[Z");
        CHECK(e.cursor().col == 0);
    }
    {
        Emulator e(2, 40);
        e.feed("\x1b[3I");
        CHECK(e.cursor().col == 24);
        e.feed("\x1b[2Z");
        CHECK(e.cursor().col == 8);
    }

    // ---- colours and attributes
    {
        Emulator e(2, 20);
        e.feed("\x1b[1;3;4;5;7;8;9mX");
        const Cell& x = e.cell(0, 0);
        CHECK(x.attrs == (ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_BLINK | ATTR_REVERSE | ATTR_HIDDEN | ATTR_STRIKE));
        e.feed("\x1b[22;23;24;25;27;28;29mY");
        CHECK(e.cell(0, 1).attrs == 0);
        e.feed("\x1b[2mD\x1b[22mN");
        CHECK(e.cell(0, 2).attrs == ATTR_DIM && e.cell(0, 3).attrs == 0);
        e.feed("\x1b[31;42mA\x1b[39;49mB\x1b[91;102mC\x1b[0mD");
        CHECK(e.cell(0, 4).fg == Color::index(1) && e.cell(0, 4).bg == Color::index(2));
        CHECK(e.cell(0, 5).fg == Color() && e.cell(0, 5).bg == Color());
        CHECK(e.cell(0, 6).fg == Color::index(9) && e.cell(0, 6).bg == Color::index(10));
        CHECK(e.cell(0, 7).fg == Color() && e.cell(0, 7).attrs == 0);
        e.feed("\x1b[38;5;200;48;5;17mE\x1b[0m");
        CHECK(e.cell(0, 8).fg == Color::index(200) && e.cell(0, 8).bg == Color::index(17));
        e.feed("\x1b[38;2;10;20;30;48;2;40;50;60mF\x1b[0m");
        CHECK(e.cell(0, 9).fg == Color::rgb(10, 20, 30) && e.cell(0, 9).bg == Color::rgb(40, 50, 60));
        e.feed("\x1b[38:2::70:80:90mG\x1b[48:5:99mH\x1b[0m");     // the colon forms
        CHECK(e.cell(0, 10).fg == Color::rgb(70, 80, 90));
        CHECK(e.cell(0, 11).bg == Color::index(99));
        e.feed("\x1b[38:2:1:2:3mI\x1b[0m");                      // without the colour space
        CHECK(e.cell(0, 12).fg == Color::rgb(1, 2, 3));
        e.feed("\x1b[4:3mU\x1b[4:0mV\x1b[0m");                   // underline styles
        CHECK((e.cell(0, 13).attrs & ATTR_UNDERLINE) && !(e.cell(0, 14).attrs & ATTR_UNDERLINE));
        e.feed("\x1b[1;38;5mZ\x1b[0m");                          // a colour left unfinished is ignored, the bold is not
        CHECK((e.cell(0, 15).attrs & ATTR_BOLD) && e.cell(0, 15).fg == Color());
        e.feed("\x1b[m\x1b[31m\x1b[mQ");
        CHECK(e.cell(0, 16).fg == Color());                      // ESC [ m is a reset
    }
    {   // the palette
        int r, g, b;
        palette_rgb(1, r, g, b); CHECK(r == 205 && g == 0 && b == 0);
        palette_rgb(15, r, g, b); CHECK(r == 255 && g == 255 && b == 255);
        palette_rgb(16, r, g, b); CHECK(r == 0 && g == 0 && b == 0);
        palette_rgb(21, r, g, b); CHECK(r == 0 && g == 0 && b == 255);
        palette_rgb(196, r, g, b); CHECK(r == 255 && g == 0 && b == 0);
        palette_rgb(231, r, g, b); CHECK(r == 255 && g == 255 && b == 255);
        palette_rgb(232, r, g, b); CHECK(r == 8 && g == 8 && b == 8);
        palette_rgb(255, r, g, b); CHECK(r == 238 && g == 238 && b == 238);
    }

    // ---- the alternate screen
    {
        Emulator e(3, 6, 50);
        e.feed("main1\r\nmain2");
        const std::string before = screen(e);
        const Cursor c0 = e.cursor();
        e.feed("\x1b[?1049h");
        CHECK(e.alt_screen());
        CHECK_EQ(screen(e), std::string("||"  "|"));              // a clean screen
        e.feed("vim\r\nlines\r\nmore\r\nand more\r\n");
        CHECK_EQ(e.scrollback_size(), (size_t)0);                // the alternate screen keeps no scrollback
        e.feed("\x1b[?1049l");
        CHECK(!e.alt_screen());
        CHECK_EQ(screen(e), before);                             // the main screen is as it was
        CHECK(e.cursor() == c0);                                 // with the cursor where it was
    }
    {   // the cursor is saved and restored with the alternate screen, and the attributes with it
        Emulator e(3, 10);
        e.feed("\x1b[1;31mab\x1b[?1049h\x1b[0mxy\x1b[?1049lc");
        CHECK(e.cell(0, 2).attrs == ATTR_BOLD && e.cell(0, 2).fg == Color::index(1));
    }

    // ---- line drawing
    {
        Emulator e(2, 10);
        e.feed("\x1b(0lqqk\x1b(B x");
        CHECK_EQ(line(e, 0), utf8(0x250C) + utf8(0x2500) + utf8(0x2500) + utf8(0x2510) + " x");
        e.feed("\r\n\x0e" "x\x0f" "x");                           // shift out to G1 (which is ASCII until set)
        CHECK_EQ(line(e, 1), std::string("xx"));
        e.feed("\r\x1b)0\x0e" "x\x0f" "x");
        CHECK_EQ(line(e, 1), utf8(0x2502) + "x");
    }

    // ---- wide characters
    {
        Emulator e(2, 6);
        e.feed(utf8(0x4E2D) + "a" + utf8(0x6587));
        CHECK(e.cell(0, 0).attrs & ATTR_WIDE);
        CHECK(e.cell(0, 1).attrs & ATTR_WIDE_TAIL);
        CHECK(e.cell(0, 2).ch == U'a');
        CHECK(e.cursor().col == 5 && e.cursor().row == 0);
        CHECK_EQ(line(e, 0), utf8(0x4E2D) + "a" + utf8(0x6587));
        check_invariants(e);
        e.feed("\x1b[1;2Hz");                                    // overwriting the second half blanks the first
        CHECK(e.cell(0, 0).ch == U' ' && !(e.cell(0, 0).attrs & ATTR_WIDE));
        check_invariants(e);
        e.feed("\x1b[1;1H" + utf8(0x4E2D) + "\x1b[1;1Hq");       // overwriting the first half blanks the second
        CHECK(e.cell(0, 0).ch == U'q' && e.cell(0, 1).ch == U' ' && !(e.cell(0, 1).attrs & ATTR_WIDE_TAIL));
        check_invariants(e);
    }
    {   // it does not fit at the end of the line: it goes to the next one
        Emulator e(3, 5);
        e.feed("abcd" + utf8(0x4E2D) + "e");
        CHECK_EQ(line(e, 0), std::string("abcd"));
        CHECK_EQ(line(e, 1), utf8(0x4E2D) + "e");
        check_invariants(e);
    }
    {   // erasing or deleting through half of one blanks the other half
        Emulator e(2, 8);
        e.feed(utf8(0x4E2D) + utf8(0x6587) + "\x1b[1;2H\x1b[X");
        CHECK(e.cell(0, 0).ch == U' ' && e.cell(0, 1).ch == U' ');
        check_invariants(e);
        e.feed("\x1b[1;1H" + utf8(0x4E2D) + utf8(0x6587) + "\x1b[1;2H\x1b[P");
        check_invariants(e);
        e.feed("\x1b[1;1H" + utf8(0x4E2D) + utf8(0x6587) + "\x1b[1;2H\x1b[@");
        check_invariants(e);
    }
    {   // a combining mark has no cell of its own: it is dropped, and the cursor does not move
        Emulator e(1, 5);
        e.feed(std::string("e") + utf8(0x301) + "x");
        CHECK_EQ(line(e, 0), std::string("ex"));
    }

    // ---- saving the cursor
    {
        Emulator e(4, 10);
        e.feed("\x1b[2;3H\x1b[1;32m\x1b" "7" "\x1b[1;1H\x1b[0m" "x\x1b" "8" "y");
        CHECK(e.cursor().row == 1 && e.cursor().col == 3);
        CHECK(e.cell(1, 2).ch == U'y' && e.cell(1, 2).attrs == ATTR_BOLD && e.cell(1, 2).fg == Color::index(2));
        e.feed("\x1b[3;3H\x1b[s\x1b[1;1H\x1b[u");
        CHECK(e.cursor().row == 2 && e.cursor().col == 2);
    }

    // ---- what the program asks, and what comes back
    {
        Emulator e(5, 10);
        e.feed("\x1b[3;7H\x1b[6n");
        CHECK_EQ(e.take_reply(), std::string("\x1b[3;7R"));
        CHECK_EQ(e.take_reply(), std::string(""));               // taking empties it
        e.feed("\x1b[5n");
        CHECK_EQ(e.take_reply(), std::string("\x1b[0n"));
        e.feed("\x1b[c");
        CHECK_EQ(e.take_reply(), std::string("\x1b[?1;2c"));
        e.feed("\x1b[>c");
        CHECK_EQ(e.take_reply(), std::string("\x1b[>0;0;0c"));
        e.feed("\x1b[6n\x1b[5n");
        CHECK_EQ(e.take_reply(), std::string("\x1b[3;7R\x1b[0n"));
    }

    // ---- the modes a host needs
    {
        Emulator e(3, 10);
        CHECK(!e.application_cursor_keys() && !e.bracketed_paste() && e.mouse_tracking() == 0 && !e.mouse_sgr() && e.cursor().visible);
        e.feed("\x1b[?1h\x1b[?2004h\x1b[?1000h\x1b[?1006h\x1b[?1004h\x1b[?25l\x1b=");
        CHECK(e.application_cursor_keys() && e.bracketed_paste() && e.mouse_tracking() == 1000 && e.mouse_sgr() && e.focus_reporting());
        CHECK(!e.cursor().visible && e.application_keypad());
        e.feed("\x1b[?1002h");
        CHECK(e.mouse_tracking() == 1002);
        e.feed("\x1b[?1000l");                                   // turning off a mode that is not the current one changes nothing
        CHECK(e.mouse_tracking() == 1002);
        e.feed("\x1b[?1002l\x1b[?1l\x1b[?2004l\x1b[?25h\x1b>");
        CHECK(e.mouse_tracking() == 0 && !e.application_cursor_keys() && !e.bracketed_paste() && e.cursor().visible && !e.application_keypad());
        e.feed("\x1b[5 q");
        CHECK_EQ(e.cursor().shape, 5);
        e.feed("\x1b[?5h");
        CHECK(e.reverse_video());
    }

    // ---- the title
    {
        Emulator e(2, 10);
        e.feed("\x1b]0;my title\x07");
        CHECK_EQ(e.title(), std::string("my title"));
        e.feed("\x1b]2;two\x1b\\");
        CHECK_EQ(e.title(), std::string("two"));
        e.feed("\x1b]7;file:///x\x07");
        CHECK_EQ(e.title(), std::string("two"));                 // other strings change nothing
    }

    // ---- resetting
    {
        Emulator e(4, 8);
        e.feed("\x1b[1;31m\x1b[2;3r\x1b[?7l\x1b[?6hhello");
        e.feed("\x1b[!p");                                       // a soft reset: the pen, the region and the modes
        e.feed("\x1b[1;1HX");
        CHECK(e.cell(0, 0).attrs == 0 && e.cell(0, 0).fg == Color());
        e.feed("\x1b" "c");
        CHECK_EQ(screen(e), std::string("|||"  "|"));
        e.feed("\x1b#8");                                        // the screen alignment test
        CHECK_EQ(line(e, 3), std::string("EEEEEEEE"));
    }

    // ---- resizing
    {
        Emulator e(4, 8, 50);
        e.feed("ab\r\ncd\r\nef\r\ngh");
        e.resize(6, 5);
        CHECK_EQ(e.rows(), 6); CHECK_EQ(e.cols(), 5);
        CHECK_EQ(screen(e), std::string("ab|cd|ef|gh|||"));
        check_invariants(e);
        e.resize(3, 5);                                          // the cursor was on the fourth line: the top scrolls off
        CHECK_EQ(screen(e), std::string("cd|ef|gh|"));
        CHECK_EQ(e.scrollback_size(), (size_t)1);
        CHECK(e.cursor().row == 2);
        check_invariants(e);
        e.resize(3, 2);
        CHECK_EQ(screen(e), std::string("cd|ef|gh|"));
        e.resize(1, 1);
        CHECK_EQ(e.rows(), 1);
        check_invariants(e);
        e.feed("x");
        check_invariants(e);
    }
    {   // a wide character cut by a narrower screen
        Emulator e(1, 6);
        e.feed("ab" + utf8(0x4E2D));
        e.resize(1, 3);
        check_invariants(e);
        CHECK(!(e.cell(0, 2).attrs & ATTR_WIDE));
    }
    {   // resizing while the alternate screen is up
        Emulator e(5, 8);
        e.feed("main\x1b[?1049h\x1b[5;1Halt");
        e.resize(3, 6);
        check_invariants(e);
        e.feed("\x1b[?1049l");
        check_invariants(e);
    }

    // ---- VT52 mode, DECID, the terminal parameters, the UK set, DECCOLM
    {
        Emulator e(5, 10);
        e.feed("abc\x1b[?2l");                                   // enter VT52 mode
        e.feed("\x1b" "H" "X");                                  // home, then X
        CHECK_EQ(line(e, 0), std::string("Xbc"));
        e.feed("\x1b" "Y" "\x22\x24" "Z");                     // direct addressing: row 2 (0x22 - 32), column 4 (0x24 - 32)
        CHECK(e.cell(2, 4).ch == U'Z');
        e.feed("\x1b[A");                                        // CSI means nothing in VT52: ESC [ is dropped, the A is text
        CHECK(e.cell(2, 6).ch == U'A' || e.cell(2, 5).ch == U'A');
        e.feed("\x1b" "Z");
        CHECK_EQ(e.take_reply(), std::string("\x1b/Z"));
        e.feed("\x1b<");                                         // back to ANSI mode
        e.feed("\x1b[1;1H\x1b[31mq");
        CHECK(e.cell(0, 0).fg == Color::index(1));
        // no wrap in VT52: the last column is overwritten
        Emulator w(2, 4);
        w.feed("\x1b[?2labcdefg");
        CHECK_EQ(line(w, 0), std::string("abcg"));
    }
    {
        Emulator e(3, 8);
        e.feed("\x1b" "Z");
        CHECK_EQ(e.take_reply(), std::string("\x1b[?1;2c"));       // DECID in ANSI mode
        e.feed("\x1b[0x");
        CHECK_EQ(e.take_reply(), std::string("\x1b[2;1;1;112;112;1;0x"));
        e.feed("\x1b[1x");
        CHECK_EQ(e.take_reply(), std::string("\x1b[3;1;1;112;112;1;0x"));
        e.feed("\x1b(A#\x1b(B#");                                // the UK set: # is a pound sign
        CHECK_EQ(line(e, 0), utf8(0xA3) + "#");
        e.feed("\x1b[2J\x1b[3;5Hxyz\x1b[?3h");                  // DECCOLM clears the screen and homes the cursor
        CHECK_EQ(screen(e), std::string("|||"));                  // (three empty rows)
        CHECK(e.cursor().row == 0 && e.cursor().col == 0 && e.cols() == 8);
        e.feed("\x1b[?15n");
        CHECK_EQ(e.take_reply(), std::string("\x1b[?13n"));
    }

    // ---- dirty rows
    {
        Emulator e(4, 6);
        e.clear_dirty();
        CHECK(!e.any_dirty());
        e.feed("\x1b[2;1Hhi");
        CHECK(e.any_dirty() && e.row_dirty(1) && !e.row_dirty(0) && !e.row_dirty(2));
        e.clear_dirty();
        e.feed("\x1b[3;1H");                                     // moving the cursor changes no cell
        CHECK(!e.any_dirty());
        e.feed("\x1b[K");
        CHECK(e.row_dirty(2));
        e.clear_dirty();
        e.feed("\r\n\r\n\r\n\r\n");                              // scrolling dirties the region
        CHECK(e.row_dirty(0) && e.row_dirty(3));
        e.clear_dirty();
        e.feed("\x1b[?1049h");
        CHECK(e.row_dirty(0) && e.row_dirty(1) && e.row_dirty(2) && e.row_dirty(3));
        e.clear_dirty();
        e.resize(5, 7);
        CHECK(e.row_dirty(0) && e.row_dirty(4));
    }

    // ---- any bytes at all: nothing breaks, and splitting the input changes nothing
    {
        std::mt19937 rng(99);
        static const char* PIECES[] = {
            "hello", " world ", "\r\n", "\r", "\n", "\t", "\b", "\x07", "\x0e", "\x0f",
            "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[5A", "\x1b[7B", "\x1b[3C", "\x1b[9D", "\x1b[H", "\x1b[5;9H", "\x1b[12;40H", "\x1b[3;1H",
            "\x1b[J", "\x1b[1J", "\x1b[2J", "\x1b[K", "\x1b[1K", "\x1b[2K", "\x1b[3X", "\x1b[2L", "\x1b[3M", "\x1b[2@", "\x1b[4P", "\x1b[3S", "\x1b[2T", "\x1b[5b",
            "\x1b[2;7r", "\x1b[r", "\x1b[?6h", "\x1b[?6l", "\x1b[?7h", "\x1b[?7l", "\x1b[4h", "\x1b[4l", "\x1b[?1049h", "\x1b[?1049l", "\x1b[?47h", "\x1b[?47l",
            "\x1b[1m", "\x1b[0m", "\x1b[31m", "\x1b[42m", "\x1b[38;5;99m", "\x1b[48;2;1;2;3m", "\x1b[7m", "\x1b[4m", "\x1b[m",
            "\x1b(0", "\x1b(B", "\x1b)0", "\x1b" "7", "\x1b" "8", "\x1b" "M", "\x1b" "D", "\x1b" "E", "\x1b" "H", "\x1b[g", "\x1b[3g", "\x1b[2I", "\x1b[2Z", "\x1b[s", "\x1b[u",
            "\x1b]0;title\x07", "\x1b[6n", "\x1b[c", "\x1b[!p", "\x1b#8",
            "\xE4\xB8\xAD", "\xE6\x96\x87", "\xE2\x94\x80", "\xC3\xA9", "\xCC\x81", "\xF0\x9F\x98\x80", "\xFF", "\xE4\xB8",
        };
        const int N = (int)(sizeof PIECES / sizeof PIECES[0]);
        for (int trial = 0; trial < 300; trial++) {
            const int rows = 1 + (int)(rng() % 12), cols = 1 + (int)(rng() % 30);
            std::string stream;
            const int pieces = 10 + (int)(rng() % 200);
            for (int i = 0; i < pieces; i++) stream += PIECES[rng() % (unsigned)N];
            Emulator whole(rows, cols, 20), pieces_fed(rows, cols, 20);
            whole.feed(stream);
            size_t at = 0;
            while (at < stream.size()) {
                const size_t n = 1 + rng() % 9;
                pieces_fed.feed(stream.substr(at, n));
                at += n;
            }
            check_invariants(whole);
            CHECK(same_screen(whole, pieces_fed));
            CHECK_EQ(whole.take_reply(), pieces_fed.take_reply());
            // and a resize in the middle never breaks it
            whole.resize(1 + (int)(rng() % 12), 1 + (int)(rng() % 30));
            check_invariants(whole);
            whole.feed(stream);
            check_invariants(whole);
        }
        // raw random bytes, with escapes
        for (int trial = 0; trial < 200; trial++) {
            Emulator e(1 + (int)(rng() % 8), 1 + (int)(rng() % 20), 5);
            std::string s;
            const int n = 1 + (int)(rng() % 500);
            for (int i = 0; i < n; i++) s += (char)(rng() % 4 == 0 ? 0x1b : rng() % 256);
            e.feed(s);
            check_invariants(e);
        }
    }

    if (failures == 0) std::printf("all emulator tests passed\n");
    return failures == 0 ? 0 : 1;
}
