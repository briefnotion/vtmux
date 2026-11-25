// Tests for the VT parser: what events a stream of bytes makes, split any way.
// sources: ../src/vtparser.cpp
#include <random>
#include <sstream>
#include <string>

#include "check.h"
#include "../src/vtparser.h"

using namespace vtmux;

namespace {

// Writes every event as text, so a test can compare a whole stream at once.
struct Log : VtParserEvents {
    std::ostringstream out;
    void print(char32_t cp) override { out << "p" << (unsigned)cp << ' '; }
    void execute(uint8_t c) override { out << "x" << (unsigned)c << ' '; }
    void csi(const Params& p, char marker, std::string_view inter, char final) override {
        out << "csi[";
        if (marker) out << marker;
        for (size_t i = 0; i < p.size(); i++) out << (i ? (p.sub[i] ? ":" : ";") : "") << p.value[i];
        out << "|" << inter << "|" << final << "] ";
    }
    void esc(std::string_view inter, char final) override { out << "esc[" << inter << "|" << final << "] "; }
    void osc(std::string_view s) override { out << "osc[" << s << "] "; }
    std::string str() const { return out.str(); }
};

std::string run(const std::string& bytes) {
    VtParser p; Log log;
    p.feed(bytes, log);
    return log.str();
}

std::string run_in_pieces(const std::string& bytes, std::mt19937& rng, size_t max_piece) {
    VtParser p; Log log;
    size_t i = 0;
    while (i < bytes.size()) {
        const size_t n = 1 + rng() % max_piece;
        p.feed(bytes.substr(i, n), log);
        i += n;
    }
    return log.str();
}

std::string run_bytewise(const std::string& bytes) {
    VtParser p; Log log;
    for (char c : bytes) p.feed(&c, 1, log);
    return log.str();
}

}  // namespace

int main() {
    // ---- text
    CHECK_EQ(run("Hi"), std::string("p72 p105 "));
    CHECK_EQ(run("a\xC3\xA9z"), std::string("p97 p233 p122 "));                    // e acute
    CHECK_EQ(run("\xE2\x94\x80"), std::string("p9472 "));                          // a box line
    CHECK_EQ(run("\xF0\x9F\x98\x80"), std::string("p128512 "));                    // an emoji
    CHECK_EQ(run("\xE4\xB8\xAD"), std::string("p20013 "));                         // a Chinese character

    // ---- bad UTF-8 is a replacement character, not a crash or a hole
    CHECK_EQ(run("\xC0\x80"), std::string("p65533 "));                             // an over-long form of NUL: refused, as one bad character
    CHECK_EQ(run("\xE0\x80\x80"), std::string("p65533 "));                         // over-long
    CHECK_EQ(run("\xED\xA0\x80"), std::string("p65533 "));                         // a surrogate
    CHECK_EQ(run("\xF4\x90\x80\x80"), std::string("p65533 "));                     // beyond U+10FFFF
    CHECK_EQ(run("\x80"), std::string("p65533 "));                                 // a stray continuation
    CHECK_EQ(run("\xFF"), std::string("p65533 "));
    CHECK_EQ(run("\xE2\x94" "A"), std::string("p65533 p65 "));                     // cut short by an ordinary letter
    CHECK_EQ(run("\xE2\x94\n"), std::string("p65533 x10 "));                       // or by a control

    // ---- controls
    CHECK_EQ(run("a\b\t\n\r\x07\x0e\x0f"), std::string("p97 x8 x9 x10 x13 x7 x14 x15 "));
    CHECK_EQ(run("\x7f"), std::string(""));                                        // DEL is ignored

    // ---- control sequences
    CHECK_EQ(run("\x1b[A"), std::string("csi[||A] "));
    CHECK_EQ(run("\x1b[1;31m"), std::string("csi[1;31||m] "));
    CHECK_EQ(run("\x1b[m"), std::string("csi[||m] "));                             // no parameters at all
    CHECK_EQ(run("\x1b[;5H"), std::string("csi[-1;5||H] "));                       // a left out number
    CHECK_EQ(run("\x1b[1;;3m"), std::string("csi[1;-1;3||m] "));
    CHECK_EQ(run("\x1b[5;m"), std::string("csi[5;-1||m] "));                       // and one left out at the end
    CHECK_EQ(run("\x1b[?25l"), std::string("csi[?25||l] "));
    CHECK_EQ(run("\x1b[>0c"), std::string("csi[>0||c] "));
    CHECK_EQ(run("\x1b[?1049h"), std::string("csi[?1049||h] "));
    CHECK_EQ(run("\x1b[38:2::10:20:30m"), std::string("csi[38:2:-1:10:20:30||m] "));     // sub-parameters (with a left out one)
    CHECK_EQ(run("\x1b[4:3m"), std::string("csi[4:3||m] "));
    CHECK_EQ(run("\x1b[1 q"), std::string("csi[1| |q] "));                         // an intermediate byte
    CHECK_EQ(run("\x1b[!p"), std::string("csi[|!|p] "));
    CHECK_EQ(run("\x1b[99999999m"), std::string("csi[65535||m] "));                // a silly number is clamped
    {   // too many parameters: the first 32 are kept
        std::string s = "\x1b[";
        for (int i = 0; i < 50; i++) s += (i ? ";1" : "1");
        s += "m";
        const std::string out = run(s);
        CHECK(out.rfind("csi[1", 0) == 0);
        size_t semis = 0;
        for (char c : out) if (c == ';') semis++;
        CHECK_EQ(semis, (size_t)31);
    }

    // ---- controls inside a sequence act at once, and the sequence goes on
    CHECK_EQ(run("\x1b[1\n;2H"), std::string("x10 csi[1;2||H] "));
    CHECK_EQ(run("\x1b[\x08" "5C"), std::string("x8 csi[5||C] "));
    CHECK_EQ(run("\x1b]0;ti\ntle\x07"), std::string("osc[0;ti\ntle] "));          // (but in a string a newline is just text)

    // ---- cancelling and restarting
    CHECK_EQ(run("\x1b[12\x18" "A"), std::string("p65 "));                         // CAN: the sequence is dropped, A is text
    CHECK_EQ(run("\x1b[12\x1a" "A"), std::string("p65 "));                         // SUB too
    CHECK_EQ(run("\x1b[12\x1b[3A"), std::string("csi[3||A] "));                    // ESC starts again
    CHECK_EQ(run("\x1b\x1b[A"), std::string("csi[||A] "));
    CHECK_EQ(run("\x1b[1?2H" "x"), std::string("p120 "));                          // a marker in the middle: the whole sequence is skipped
    CHECK_EQ(run("\x1b[1 3m" "x"), std::string("p120 "));                          // a digit after an intermediate: skipped
    CHECK_EQ(run("\x1b[1;2" "\x01" "H"), std::string("x1 csi[1;2||H] "));

    // ---- escape sequences
    CHECK_EQ(run("\x1b" "7"), std::string("esc[|7] "));
    CHECK_EQ(run("\x1b" "M"), std::string("esc[|M] "));
    CHECK_EQ(run("\x1b(B"), std::string("esc[(|B] "));
    CHECK_EQ(run("\x1b)0"), std::string("esc[)|0] "));
    CHECK_EQ(run("\x1b#8"), std::string("esc[#|8] "));
    CHECK_EQ(run("\x1b" "c"), std::string("esc[|c] "));
    CHECK_EQ(run("\x1b=\x1b>"), std::string("esc[|=] esc[|>] "));

    // ---- strings
    CHECK_EQ(run("\x1b]0;hello\x07"), std::string("osc[0;hello] "));
    CHECK_EQ(run("\x1b]2;a title\x1b\\"), std::string("osc[2;a title] "));
    CHECK_EQ(run("\x1b]0;x\x07y"), std::string("osc[0;x] p121 "));
    CHECK_EQ(run("\x1b]0;x\x1b" "[Ay"), std::string("csi[||A] p121 "));            // an ESC that is not ST abandons the string
    CHECK_EQ(run("\x1b]0;x\x18" "y"), std::string("p121 "));                       // CAN too
    CHECK_EQ(run("\x1bP1;2|data\x1b\\" "z"), std::string("p122 "));                // a DCS string is skipped to its end
    CHECK_EQ(run("\x1b_apc\x1b\\" "z"), std::string("p122 "));
    CHECK_EQ(run("\x1b^pm\x07" "z"), std::string(""));                            // BEL does not end these: z is inside the string
    CHECK_EQ(run("\x1b^pm\x07" "z\x1b\\" "k"), std::string("p107 "));                 // ST does
    {   // a string longer than the limit is dropped, and what follows is fine
        const std::string big(VtParser::MAX_STRING + 100, 'x');
        CHECK_EQ(run("\x1b]0;" + big + "\x07" "k"), std::string("p107 "));
        const std::string just(VtParser::MAX_STRING - 3, 'x');
        CHECK(run("\x1b]0;" + just + "\x07").rfind("osc[0;x", 0) == 0);
    }

    // ---- split anywhere: the same events whole, a byte at a time, or in random pieces
    const std::string samples[] = {
        "hello \x1b[1;31mred\x1b[0m \xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80 \x1b]0;title\x07 \x1b(0lqk\x1b(B\x1b[?1049h\x1b[2J\x1b[H\x08\t\r\n",
        "\x1b[38:2::1:2:3m\x1b[48;5;200m\x1bP+q\x1b\\\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\",
        "\xE2\x94\x80\xE2\x94\x80\x1b[12\x18" "ok\x1b[1?2H" "text\x1b\x1b\x1b[A",
    };
    std::mt19937 rng(42);
    for (const std::string& s : samples) {
        const std::string whole = run(s);
        CHECK_EQ(run_bytewise(s), whole);
        for (int trial = 0; trial < 200; trial++) CHECK_EQ(run_in_pieces(s, rng, 5), whole);
    }

    // ---- anything at all: random bytes (with plenty of escapes) never crash, and are split-proof
    for (int trial = 0; trial < 400; trial++) {
        std::string s;
        const int n = 1 + (int)(rng() % 300);
        for (int i = 0; i < n; i++) {
            const unsigned r = rng() % 10;
            if (r == 0) s += '\x1b';
            else if (r == 1) s += "[";
            else if (r == 2) s += (char)('0' + rng() % 10);
            else if (r == 3) s += ';';
            else if (r == 4) s += (char)(0x40 + rng() % 0x3F);
            else if (r == 5) s += (char)(rng() % 32);
            else if (r == 6) s += (char)(0x80 + rng() % 0x80);
            else s += (char)(rng() % 256);
        }
        const std::string whole = run(s);
        CHECK_EQ(run_bytewise(s), whole);
        CHECK_EQ(run_in_pieces(s, rng, 7), whole);
    }

    if (failures == 0) std::printf("all parser tests passed\n");
    return failures == 0 ? 0 : 1;
}
