// vtdump: feed the bytes of a file into vtmux::Emulator and print the screen in a stable text format, so the
// conformance runner (tests/conformance/) can compare it with tmux and pyte.
//
//   vtdump ROWS COLS FILE [--chunk N] [--attrs] [--trace] [--resize-fuzz SEED] [--history]
//
// Output (all row and column numbers are 0-based):
//   ROWS lines   the screen rows as UTF-8, the second cell of a wide character skipped, trailing spaces trimmed
//   cursor ROW COL VISIBLE        VISIBLE is 1 or 0
//   alt 0|1                       whether the alternate screen is shown
//   scrollback N                  lines that went off the top of the main screen
//   invariant TEXT               only when something that must hold for any input does not (cursor on the screen, wide
//                                characters whole: a first half is followed by its second half and a second half is
//                                preceded by a first half, no character of width 0 in a cell)
//   reply ESCAPED                 only when the emulator answered something (\xHH escapes, printable ASCII as is)
//   with --history, the lines that scrolled off the top ("hist TEXT", oldest first) before the cursor line.
//   with --resize-fuzz SEED, the input comes in pieces of random size and between pieces the screen is sometimes resized to
//   a random size (1 to twice the original): for finding crashes and broken invariants, not for comparing.
//   with --trace, three lines "pending LO-HI LO-HI ...", "col0 ...", "lastcol ...", "outside ..." and "bg ..." ("-" for none), and "curs R:C R:C ...": the byte offsets (bytes
//   consumed so far) at which the cursor was in the deferred-wrap state (the last column written, the next printable
//   character will wrap), in column 0 of a row below the first, in the last column, and on a row outside a scroll region that
//   is not the whole screen (found by asking a copy: origin mode and CUP to row 1 and to row 9999), and those at which the
//   pen has a background colour (a probe character is written to a copy and its cell looked at). "curs" is the cursor
//   position after each byte, offsets 0 to N. The wrap state is found by feeding probe
//   characters to a copy of the emulator, so no access to its private state is needed.
//   with --attrs, one line per cell that is not entirely default (blank, default colours, no attributes):
//   cell ROW COL U+XXXX fg=KIND:A.B.C bg=KIND:A.B.C attrs=bold,dim,...    (KIND d default, i indexed, r rgb)
//
// Build: g++ -std=c++17 -O1 -I. -o /tmp/vtdump tools/vtdump.cpp src/vtparser.cpp src/emulator.cpp src/width.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/emulator.h"

using namespace vtmux;

static void append_utf8(std::string& s, char32_t cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

static std::string color_text(const Color& c) {
    char buf[40];
    if (c.kind == Color::Default) return "d";
    if (c.kind == Color::Indexed) { std::snprintf(buf, sizeof buf, "i:%d", c.a); return buf; }
    std::snprintf(buf, sizeof buf, "r:%d.%d.%d", c.a, c.b, c.c);
    return buf;
}

static std::string attr_text(uint16_t a) {
    static const struct { uint16_t bit; const char* name; } NAMES[] = {
        {ATTR_BOLD, "bold"}, {ATTR_DIM, "dim"}, {ATTR_ITALIC, "italic"}, {ATTR_UNDERLINE, "underline"}, {ATTR_BLINK, "blink"},
        {ATTR_REVERSE, "reverse"}, {ATTR_HIDDEN, "hidden"}, {ATTR_STRIKE, "strike"}, {ATTR_WIDE, "wide"}, {ATTR_WIDE_TAIL, "tail"}};
    std::string s;
    for (const auto& n : NAMES) if (a & n.bit) { if (!s.empty()) s += ','; s += n.name; }
    return s.empty() ? "-" : s;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: vtdump ROWS COLS FILE [--chunk N] [--attrs]\n"); return 2; }
    const int rows = std::atoi(argv[1]), cols = std::atoi(argv[2]);
    size_t chunk = 0;
    bool attrs = false, trace = false, history = false;
    long fuzz_seed = -1;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "--attrs")) attrs = true;
        else if (!std::strcmp(argv[i], "--trace")) trace = true;
        else if (!std::strcmp(argv[i], "--history")) history = true;
        else if (!std::strcmp(argv[i], "--resize-fuzz") && i + 1 < argc) fuzz_seed = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) chunk = (size_t)std::atol(argv[++i]);
        else { std::fprintf(stderr, "vtdump: unknown argument %s\n", argv[i]); return 2; }
    }
    if (rows < 1 || cols < 1) { std::fprintf(stderr, "vtdump: bad size\n"); return 2; }
    FILE* f = std::strcmp(argv[3], "-") == 0 ? stdin : std::fopen(argv[3], "rb");
    if (!f) { std::perror(argv[3]); return 2; }
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    if (f != stdin) std::fclose(f);

    // while tracing, history is kept only where the probe could not tell otherwise (one row or one column), so the copies stay small
    Emulator e(rows, cols, !trace ? 2000 : (rows == 1 || cols == 1) ? 100000 : 0);
    std::string reply;
    if (chunk == 0) chunk = data.size() ? data.size() : 1;
    std::string pending_ranges, col0_ranges, last_ranges, outside_ranges, bg_ranges, curs_list;
    if (trace) {
        // The wrap is pending only in the last column, so the probes are needed only there (and everywhere for one column).
        auto pending_now = [&]() {
            const Cursor c = e.cursor();
            if (cols > 1 && c.col != cols - 1) return false;
            for (const char* probe : {"Z", "Y"}) {
                Emulator p = e;
                p.feed(probe, 1);
                if (p.scrollback_size() != e.scrollback_size()) return true;     // it scrolled: the probe wrapped first
                const Cell& k = p.cell(c.row, c.col);
                if (k.ch != (char32_t)probe[0]) return true;                     // the probe went somewhere else
                if (cols == 1)
                    for (int r = 0; r < rows; r++)
                        if (r != c.row && p.cell(r, 0) != e.cell(r, 0)) return true;
            }
            return false;
        };
        auto add = [](std::string& out, long& lo, bool on, size_t i) {            // collect the ranges of offsets where `on` holds
            if (on && lo < 0) lo = (long)i;
            if (!on && lo >= 0) { out += std::to_string(lo) + "-" + std::to_string(i - 1) + " "; lo = -1; }
        };
        long plo = -1, clo = -1, llo = -1, olo = -1, blo = -1;
        const std::string input = data;
        for (size_t i = 0; i <= input.size(); i++) {
            if (i > 0) { e.feed(input.data() + i - 1, 1); reply += e.take_reply(); }
            const Cursor cur = e.cursor();
            curs_list += std::to_string(cur.row) + ":" + std::to_string(cur.col) + " ";
            const bool pend = pending_now();
            bool has_bg = false, outside = false;
            if (pend) {                                                          // the colour the next character would be drawn with
                Emulator p = e;
                p.feed("Z", 1);
                for (int r = 0; r < rows && !has_bg; r++)
                    for (int cc = 0; cc < cols; cc++)
                        if (p.cell(r, cc).ch == U'Z' && e.cell(r, cc).ch != U'Z' && p.cell(r, cc).bg.kind != Color::Default) { has_bg = true; break; }
            }
            if (i < input.size() && input[i] == 0x1b) {                          // where an escape sequence starts: is the cursor outside the region?
                Emulator p = e;
                p.feed("\x1b[?6h\x1b[1;1H", 14);
                const int top = p.cursor().row;
                p.feed("\x1b[9999;1H", 10);
                const int bottom = p.cursor().row;
                outside = (top > 0 || bottom < rows - 1) && (cur.row < top || cur.row > bottom);
            }
            add(pending_ranges, plo, pend, i);
            add(col0_ranges, clo, cur.col == 0 && cur.row > 0, i);
            add(last_ranges, llo, cur.col == cols - 1, i);
            add(outside_ranges, olo, outside, i);
            add(bg_ranges, blo, has_bg, i);
        }
        const size_t end = input.size() + 1;
        add(pending_ranges, plo, false, end);
        add(col0_ranges, clo, false, end);
        add(last_ranges, llo, false, end);
        add(outside_ranges, olo, false, end);
        add(bg_ranges, blo, false, end);
        data.clear();                                                // already fed, byte by byte
    }
    if (fuzz_seed >= 0) {
        unsigned long long x = (unsigned long long)fuzz_seed * 6364136223846793005ULL + 1442695040888963407ULL;
        auto next = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
        for (size_t i = 0; i < data.size();) {
            const size_t n = std::min((size_t)(next() % 64 + 1), data.size() - i);
            e.feed(data.data() + i, n);
            i += n;
            reply += e.take_reply();
            if (next() % 4 == 0) e.resize((int)(next() % (2 * rows)) + 1, (int)(next() % (2 * cols)) + 1);
        }
    } else {
        for (size_t i = 0; i < data.size(); i += chunk) {
            e.feed(data.data() + i, std::min(chunk, data.size() - i));
            reply += e.take_reply();
        }
    }

    for (int r = 0; r < e.rows(); r++) {
        std::string s;
        for (int c = 0; c < e.cols(); c++) {
            const Cell& k = e.cell(r, c);
            if (k.attrs & ATTR_WIDE_TAIL) continue;
            append_utf8(s, k.ch);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        std::printf("%s\n", s.c_str());
    }
    const Cursor cur = e.cursor();
    std::printf("cursor %d %d %d\n", cur.row, cur.col, cur.visible ? 1 : 0);
    std::printf("alt %d\n", e.alt_screen() ? 1 : 0);
    std::printf("scrollback %zu\n", e.scrollback_size());
    {
        std::string bad;
        const Cursor cu = e.cursor();
        if (cu.row < 0 || cu.row >= e.rows() || cu.col < 0 || cu.col >= e.cols()) bad += " cursor-outside-screen";
        for (int r = 0; r < e.rows(); r++)
            for (int c = 0; c < e.cols(); c++) {
                const Cell& k = e.cell(r, c);
                if ((k.attrs & ATTR_WIDE) && (c + 1 >= e.cols() || !(e.cell(r, c + 1).attrs & ATTR_WIDE_TAIL))) bad += " wide-without-tail@" + std::to_string(r) + "," + std::to_string(c);
                if ((k.attrs & ATTR_WIDE_TAIL) && (c == 0 || !(e.cell(r, c - 1).attrs & ATTR_WIDE))) bad += " tail-without-wide@" + std::to_string(r) + "," + std::to_string(c);
            }
        if (e.scrollback_size() > e.scrollback_limit()) bad += " scrollback-over-limit";
        if (!bad.empty()) std::printf("invariant%s\n", bad.c_str());
    }
    if (trace) {
        std::printf("pending %s\n", pending_ranges.empty() ? "-" : pending_ranges.c_str());
        std::printf("col0 %s\n", col0_ranges.empty() ? "-" : col0_ranges.c_str());
        std::printf("lastcol %s\n", last_ranges.empty() ? "-" : last_ranges.c_str());
        std::printf("outside %s\n", outside_ranges.empty() ? "-" : outside_ranges.c_str());
        std::printf("bg %s\n", bg_ranges.empty() ? "-" : bg_ranges.c_str());
        std::printf("curs %s\n", curs_list.c_str());
    }
    if (!reply.empty()) {
        std::string esc;
        for (unsigned char ch : reply) {
            char t[8];
            if (ch >= 0x20 && ch < 0x7F && ch != '\\') { esc += (char)ch; }
            else { std::snprintf(t, sizeof t, "\\x%02x", ch); esc += t; }
        }
        std::printf("reply %s\n", esc.c_str());
    }
    if (history) {
        for (size_t i = 0; i < e.scrollback_size(); i++) {
            std::string t;
            for (const Cell& k : e.scrollback_line(i)) { if (!(k.attrs & ATTR_WIDE_TAIL)) append_utf8(t, k.ch); }
            while (!t.empty() && t.back() == ' ') t.pop_back();
            std::printf("hist %s\n", t.c_str());
        }
    }
    if (attrs) {
        for (int r = 0; r < e.rows(); r++)
            for (int c = 0; c < e.cols(); c++) {
                const Cell& k = e.cell(r, c);
                if (k.ch == U' ' && k.fg.kind == Color::Default && k.bg.kind == Color::Default && k.attrs == 0) continue;
                std::printf("cell %d %d U+%04X fg=%s bg=%s attrs=%s\n", r, c, (unsigned)k.ch, color_text(k.fg).c_str(), color_text(k.bg).c_str(), attr_text(k.attrs).c_str());
            }
    }
    return 0;
}
