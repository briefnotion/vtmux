// Tests for the key parser: what keys a stream of bytes makes, fed whole or in any pieces.
// sources: ../src/keys.cpp
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "../src/keys.h"

using namespace vtmux;

namespace {

const std::string E = "\x1b";

std::vector<Key> drain(KeyParser& p) {
    std::vector<Key> out;
    Key k;
    while (p.next(k)) out.push_back(k);
    return out;
}

// Whole input at once, then a flush.
std::vector<Key> keys_of(const std::string& bytes) {
    KeyParser p;
    p.feed(bytes);
    p.flush();
    return drain(p);
}

// Every field of a key as text, so keys and whole streams compare with ==.
std::string show(const Key& k) {
    std::ostringstream o;
    o << (int)k.kind << ",ch" << (unsigned)k.ch << ",n" << k.number << (k.ctrl ? ",C" : "") << (k.alt ? ",A" : "")
      << (k.shift ? ",S" : "") << ",t[" << k.text << "],r[";
    for (unsigned char c : k.raw) o << (unsigned)c << ' ';
    o << ']';
    return o.str();
}

std::string show(const std::vector<Key>& keys) {
    std::string s;
    for (const Key& k : keys) s += show(k) + "\n";
    return s;
}

std::string raws(const std::vector<Key>& keys) {
    std::string s;
    for (const Key& k : keys) s += k.raw;
    return s;
}

// The one key a stream makes (fails the test, and returns an Unknown, if it makes any other number).
Key one(const std::string& bytes) {
    std::vector<Key> keys = keys_of(bytes);
    CHECK_EQ(keys.size(), (size_t)1);
    if (keys.size() != 1) return Key();
    CHECK_EQ(keys[0].raw, bytes);
    return keys[0];
}

void is_char(const std::string& bytes, char32_t ch, bool alt = false) {
    Key k = one(bytes);
    CHECK(k.kind == KeyKind::Char);
    CHECK_EQ((unsigned)k.ch, (unsigned)ch);
    CHECK_EQ(k.alt, alt);
    CHECK(!k.ctrl && !k.shift);
}

void is_kind(const std::string& bytes, KeyKind kind, bool ctrl = false, bool alt = false, bool shift = false) {
    Key k = one(bytes);
    CHECK(k.kind == kind);
    if (k.kind != kind) { for (unsigned char c : bytes) std::printf("%02x ", c); std::printf("<- bytes\n"); }
    CHECK_EQ(k.ctrl, ctrl);
    CHECK_EQ(k.alt, alt);
    CHECK_EQ(k.shift, shift);
}

void is_function(const std::string& bytes, int number, bool ctrl = false, bool alt = false, bool shift = false) {
    Key k = one(bytes);
    CHECK(k.kind == KeyKind::Function);
    CHECK_EQ(k.number, number);
    CHECK_EQ(k.ctrl, ctrl);
    CHECK_EQ(k.alt, alt);
    CHECK_EQ(k.shift, shift);
}

void is_unknown(const std::string& bytes) {
    Key k = one(bytes);
    CHECK(k.kind == KeyKind::Unknown);
}

// Bytes in random pieces, no flush until the end.
std::vector<Key> in_pieces(const std::string& bytes, std::mt19937& rng, size_t max_piece, bool* pending = nullptr) {
    KeyParser p;
    std::vector<Key> out;
    size_t i = 0;
    while (i < bytes.size()) {
        size_t n = 1 + rng() % max_piece;
        if (n > bytes.size() - i) n = bytes.size() - i;
        p.feed(bytes.data() + i, n);
        i += n;
        // drain now and then, not always: both must give the same keys
        if (rng() % 2) for (const Key& k : drain(p)) out.push_back(k);
    }
    for (const Key& k : drain(p)) out.push_back(k);
    if (pending) *pending = p.pending();
    return out;
}

std::string random_stream(std::mt19937& rng, size_t length, int style) {
    static const char* const pieces[] = {
        "\x1b", "\x1b", "\x1b[", "\x1bO", "[", "O", "1", "2", ";", "5", "~", "A", "C", "M", "m", "<", "Z", "\x1b[200~", "\x1b[201~",
        "\x1b[<0;10;20M", "\x1b[M abc", "\x1b[1;5C", "\x1b[15~", "\x1bOP", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xc3", "\xe2\x82",
        "\x80", "\xff", "\r", "\t", "\x7f", "\x02", "hello", " ", "\x1b\x1b", "\x1b\x1b[A", "\x1b[?1;2c", "\x1b[", "\x01",
    };
    std::string s;
    while (s.size() < length) {
        if (style == 0) s += (char)(rng() & 0xFF);                                     // any byte
        else if (style == 1) s += "\x1b[;0123456789~ABCDMOZmP<"[rng() % 24];           // ESC-heavy characters
        else s += pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];                // likely sequences, glued at random
    }
    return s;
}

// ---- plain bytes

void test_plain() {
    is_char("a", 'a');
    is_char("Z", 'Z');
    is_char(" ", ' ');
    is_char("~", '~');
    is_char("\x02", 2);                                            // Ctrl-B
    is_char("\x01", 1);
    is_char("\x1a", 26);
    is_kind("\r", KeyKind::Enter);
    is_kind("\n", KeyKind::Enter);
    is_kind("\t", KeyKind::Tab);
    is_kind("\x7f", KeyKind::Backspace);
    is_kind("\x08", KeyKind::Backspace);
    // a NUL byte is a character like the others (Ctrl-Space)
    std::string nul(1, '\0');
    is_char(nul, 0);
    // a run of plain bytes is one key each
    std::vector<Key> keys = keys_of("ab\rc");
    CHECK_EQ(keys.size(), (size_t)4);
    CHECK(keys[0].kind == KeyKind::Char && keys[0].ch == 'a');
    CHECK(keys[2].kind == KeyKind::Enter);
    CHECK(keys[3].kind == KeyKind::Char && keys[3].ch == 'c');
}

void test_utf8() {
    is_char("\xc3\xa9", 0xE9);                                     // e acute
    is_char("\xe2\x82\xac", 0x20AC);                               // euro
    is_char("\xf0\x9f\x98\x80", 0x1F600);                          // an emoji
    is_char("\xc2\x80", 0x80);                                     // the lowest of two bytes
    is_char("\xdf\xbf", 0x7FF);
    is_char("\xe0\xa0\x80", 0x800);
    is_char("\xed\x9f\xbf", 0xD7FF);                               // just under the surrogates
    is_char("\xee\x80\x80", 0xE000);
    is_char("\xef\xbf\xbd", 0xFFFD);                               // a real U+FFFD is itself
    is_char("\xf0\x90\x80\x80", 0x10000);
    is_char("\xf4\x8f\xbf\xbf", 0x10FFFF);
    // invalid: FFFD, and the raw bytes are the bad ones only
    const char* bad[] = {"\x80", "\xbf", "\xc0\xaf", "\xc1\xbf", "\xf5\x80\x80\x80", "\xff", "\xfe", "\xed\xa0\x80" /*surrogate*/,
                         "\xe0\x80\x80" /*overlong*/, "\xf0\x80\x80\x80" /*overlong*/, "\xf4\x90\x80\x80" /*too big*/};
    for (const char* b : bad) {
        std::vector<Key> keys = keys_of(b);
        CHECK(!keys.empty());
        CHECK_EQ(raws(keys), std::string(b));
        for (const Key& k : keys) {
            CHECK(k.kind == KeyKind::Char);
            CHECK_EQ((unsigned)k.ch, 0xFFFDu);
        }
    }
    // a lead byte cut short by a plain byte: FFFD for the lead, then the plain byte
    std::vector<Key> keys = keys_of("\xe2\x82z");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK_EQ(keys[0].raw, std::string("\xe2\x82"));
    CHECK_EQ((unsigned)keys[0].ch, 0xFFFDu);
    CHECK_EQ(keys[1].raw, std::string("z"));
    // a lead followed by another lead
    keys = keys_of("\xc3\xc3\xa9");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK_EQ((unsigned)keys[0].ch, 0xFFFDu);
    CHECK_EQ(keys[0].raw, std::string("\xc3"));
    CHECK_EQ((unsigned)keys[1].ch, 0xE9u);
}

void test_alt() {
    is_char(E + "a", 'a', true);
    is_char(E + "Z", 'Z', true);
    is_char(E + "1", '1', true);
    is_char(E + "\x02", 2, true);                                  // Alt-Ctrl-B
    is_char(E + "\xc3\xa9", 0xE9, true);                           // Alt with a non-ASCII letter
    is_char(E + "\xe2\x82\xac", 0x20AC, true);
    is_kind(E + "\r", KeyKind::Enter, false, true);                // Alt-Enter, Alt-Tab, Alt-Backspace keep their kind
    is_kind(E + "\t", KeyKind::Tab, false, true);
    is_kind(E + "\x7f", KeyKind::Backspace, false, true);
    // ESC ESC: Escape with alt
    is_kind(E + E, KeyKind::Escape, false, true);
    // the second ESC begins nothing of its own unless a sequence follows: then it is Alt + that key
    std::vector<Key> keys = keys_of(E + E + "x");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Escape && keys[0].alt && keys[0].raw == E + E);
    CHECK(keys[1].kind == KeyKind::Char && keys[1].ch == 'x' && !keys[1].alt);
    is_kind(E + E + "[A", KeyKind::Up, false, true);               // Alt-Up as old terminals send it
    is_kind(E + E + "OD", KeyKind::Left, false, true);
    is_kind(E + E + "[1;5A", KeyKind::Up, true, true);
    // ESC ESC ESC: Escape with alt, then a lone Escape at the flush
    keys = keys_of(E + E + E);
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Escape && keys[0].alt);
    CHECK(keys[1].kind == KeyKind::Escape && !keys[1].alt);
    // ESC and bad UTF-8: the Escape is a key of its own, then the bad byte
    keys = keys_of(E + "\xff");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Escape && !keys[0].alt && keys[0].raw == E);
    CHECK_EQ((unsigned)keys[1].ch, 0xFFFDu);
    keys = keys_of(E + "\xe2\x82");                                // ESC and a cut-off character at the flush
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Escape);
    CHECK_EQ((unsigned)keys[1].ch, 0xFFFDu);
    CHECK_EQ(keys[1].raw, std::string("\xe2\x82"));
}

// ---- sequences

void test_cursor_keys() {
    const char* letters = "ABCD";
    KeyKind kinds[] = {KeyKind::Up, KeyKind::Down, KeyKind::Right, KeyKind::Left};
    for (int i = 0; i < 4; i++) {
        is_kind(E + "[" + letters[i], kinds[i]);
        is_kind(E + "O" + letters[i], kinds[i]);
        is_kind(E + "[1;5" + letters[i], kinds[i], true);
        is_kind(E + "[1;2" + letters[i], kinds[i], false, false, true);
        is_kind(E + "[1;3" + letters[i], kinds[i], false, true);
        is_kind(E + "[1;4" + letters[i], kinds[i], false, true, true);
        is_kind(E + "[1;6" + letters[i], kinds[i], true, false, true);
        is_kind(E + "[1;7" + letters[i], kinds[i], true, true);
        is_kind(E + "[1;8" + letters[i], kinds[i], true, true, true);
        is_kind(E + "[1;9" + letters[i], kinds[i], false, true);   // meta counts as alt
        is_kind(E + "[1;13" + letters[i], kinds[i], true, true);   // meta + ctrl
        is_kind(E + "[1;1" + letters[i], kinds[i]);                // modifier 1 is none
        is_kind(E + "[;5" + letters[i], kinds[i], true);           // an empty first parameter
    }
}

void test_home_end_etc() {
    is_kind(E + "[H", KeyKind::Home);
    is_kind(E + "[F", KeyKind::End);
    is_kind(E + "OH", KeyKind::Home);
    is_kind(E + "OF", KeyKind::End);
    is_kind(E + "[1~", KeyKind::Home);
    is_kind(E + "[4~", KeyKind::End);
    is_kind(E + "[7~", KeyKind::Home);
    is_kind(E + "[8~", KeyKind::End);
    is_kind(E + "[1;5H", KeyKind::Home, true);
    is_kind(E + "[2~", KeyKind::Insert);
    is_kind(E + "[3~", KeyKind::Delete);
    is_kind(E + "[5~", KeyKind::PageUp);
    is_kind(E + "[6~", KeyKind::PageDown);
    is_kind(E + "[5;3~", KeyKind::PageUp, false, true);
    is_kind(E + "[3;5~", KeyKind::Delete, true);
    is_kind(E + "[2;2~", KeyKind::Insert, false, false, true);
    is_kind(E + "[6;8~", KeyKind::PageDown, true, true, true);
    is_kind(E + "[Z", KeyKind::Tab, false, false, true);           // back tab
    is_kind(E + "[1;5Z", KeyKind::Tab, true, false, true);
}

void test_function_keys() {
    is_function(E + "OP", 1);
    is_function(E + "OQ", 2);
    is_function(E + "OR", 3);
    is_function(E + "OS", 4);
    is_function(E + "[11~", 1);
    is_function(E + "[12~", 2);
    is_function(E + "[13~", 3);
    is_function(E + "[14~", 4);
    is_function(E + "[1;5P", 1, true);
    is_function(E + "[1;2Q", 2, false, false, true);
    is_function(E + "[1;3R", 3, false, true);
    is_function(E + "[1;7S", 4, true, true);
    is_function(E + "O2P", 1, false, false, true);                 // older xterm: modifier after SS3
    is_function(E + "O5S", 4, true);
    const int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
    for (int i = 0; i < 8; i++) {
        is_function(E + "[" + std::to_string(codes[i]) + "~", 5 + i);
        is_function(E + "[" + std::to_string(codes[i]) + ";5~", 5 + i, true);
        is_function(E + "[" + std::to_string(codes[i]) + ";4~", 5 + i, false, true, true);
    }
    const int more[] = {25, 26, 28, 29, 31, 32, 33, 34};
    for (int i = 0; i < 8; i++) is_function(E + "[" + std::to_string(more[i]) + "~", 13 + i);
    // codes nobody uses are Unknown, not some function key
    is_unknown(E + "[16~");
    is_unknown(E + "[22~");
    is_unknown(E + "[27~");
    is_unknown(E + "[30~");
    is_unknown(E + "[0~");
    is_unknown(E + "[99~");
}

void test_keypad() {
    is_kind(E + "OM", KeyKind::Enter);
    is_char(E + "Oj", '*');
    is_char(E + "Ok", '+');
    is_char(E + "Om", '-');
    is_char(E + "On", '.');
    is_char(E + "Oo", '/');
    is_char(E + "Op", '0');
    is_char(E + "Oy", '9');
    is_char(E + "OX", '=');
    is_unknown(E + "Oz");
    is_unknown(E + "OI");
}

void test_unknown_sequences() {
    is_unknown(E + "[?1;2c");                                      // a device report
    is_unknown(E + "[>0;276;0c");
    is_unknown(E + "[12;40R");                                     // a cursor position report
    is_unknown(E + "[I");                                          // focus in
    is_unknown(E + "[O");                                          // focus out
    is_unknown(E + "[X");
    is_unknown(E + "[5A");                                         // a count, not a key
    is_unknown(E + "[1;2;3A");                                     // too many parameters
    is_unknown(E + "[1:2A");
    is_unknown(E + "[1 q");                                        // an intermediate byte
    is_unknown(E + "[201~");                                       // a paste end with no paste
    is_unknown(E + "[200;1~");
    is_unknown(E + "[~");
    is_unknown(E + "[<1;2M");                                      // SGR mouse needs three numbers
    is_unknown(E + "[<M");
    is_unknown(E + "[<1;2;3A");
    is_unknown(E + "[<1;2;-3M");
    is_unknown(E + "[<1;2;3;4M");
    is_unknown(E + "O");                                           // (SS3 with nothing: at the flush, given up)
    is_unknown(E + "[");
    is_unknown(E + "[1;");
    // a sequence is complete at its final byte, and what follows is the next key
    std::vector<Key> keys = keys_of(E + "[?1;2cx");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK(keys[1].kind == KeyKind::Char && keys[1].ch == 'x');
}

void test_broken_sequences() {
    // a control byte inside a sequence breaks it: Unknown for what was gathered, then the control as a key of its own
    std::vector<Key> keys = keys_of(E + "[1\rx");
    CHECK_EQ(keys.size(), (size_t)3);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK_EQ(keys[0].raw, E + "[1");
    CHECK(keys[1].kind == KeyKind::Enter);
    CHECK(keys[2].kind == KeyKind::Char);

    keys = keys_of(E + "[" + "\x02");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK_EQ(keys[0].raw, E + "[");
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK(keys[1].kind == KeyKind::Char && keys[1].ch == 2);

    keys = keys_of(E + "O\t");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Unknown && keys[0].raw == E + "O");
    CHECK(keys[1].kind == KeyKind::Tab);

    // an ESC inside a sequence starts the next one
    keys = keys_of(E + "[1;5" + E + "[A");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Unknown && keys[0].raw == E + "[1;5");
    CHECK(keys[1].kind == KeyKind::Up);

    keys = keys_of(E + "[1" + E + "x");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK(keys[1].kind == KeyKind::Char && keys[1].alt && keys[1].ch == 'x');

    // DEL and high bytes break it too
    keys = keys_of(E + "[1\x7f");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Unknown && keys[1].kind == KeyKind::Backspace);
    keys = keys_of(E + "[1\xc3\xa9");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Unknown && keys[0].raw == E + "[1");
    CHECK(keys[1].kind == KeyKind::Char && keys[1].ch == 0xE9);

    // endless parameters: given up after a while, and the rest is ordinary input
    std::string garbage = E + "[" + std::string(500, '1');
    keys = keys_of(garbage);
    CHECK(keys.size() > 2);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK_EQ(raws(keys), garbage);
}

// ---- mouse

void test_mouse() {
    Key k = one(E + "[<0;10;20M");
    CHECK(k.kind == KeyKind::Mouse);
    CHECK(one(E + "[<0;10;20m").kind == KeyKind::Mouse);
    CHECK(one(E + "[<35;120;45M").kind == KeyKind::Mouse);
    CHECK(one(E + "[<64;1;1M").kind == KeyKind::Mouse);
    CHECK(one(E + "[M !!").kind == KeyKind::Mouse);                // legacy: three bytes after ESC [ M
    CHECK(one(E + "[M\x20\x80\xff").kind == KeyKind::Mouse);       // those bytes may be anything
    CHECK(one(E + "[M" + E + "[A").kind == KeyKind::Mouse);        // even an ESC: they are not parsed
    std::vector<Key> keys = keys_of(E + "[M !!x");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK(keys[0].kind == KeyKind::Mouse && keys[0].raw.size() == 6);
    CHECK(keys[1].kind == KeyKind::Char);
    // a legacy report cut short is given up at the flush
    keys = keys_of(E + "[M !");
    CHECK_EQ(keys.size(), (size_t)1);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK_EQ(keys[0].raw, E + "[M !");
    // mouse with ESC O M is the keypad Enter, not a mouse
    is_kind(E + "OM", KeyKind::Enter);
}

// ---- paste

void test_paste() {
    Key k = one(E + "[200~hello" + E + "[201~");
    CHECK(k.kind == KeyKind::Paste);
    CHECK_EQ(k.text, std::string("hello"));
    k = one(E + "[200~" + E + "[201~");                            // an empty paste
    CHECK(k.kind == KeyKind::Paste);
    CHECK_EQ(k.text, std::string());
    // control bytes, ESC and UTF-8 come through verbatim, and are not keys
    std::string body = "a\r\nb\t\x03" + E + "[A" + E + "x \xc3\xa9\xe2\x82\xac \xff" + E + "[200~ nested?";
    k = one(E + "[200~" + body + E + "[201~");
    CHECK(k.kind == KeyKind::Paste);
    CHECK_EQ(k.text, body);
    // a closing marker alone is not the end if its start was different
    k = one(E + "[200~x" + E + "[20~y" + E + "[201~");
    CHECK_EQ(k.text, "x" + E + "[20~y");
    // keys before and after
    std::vector<Key> keys = keys_of("ab" + E + "[200~PASTE" + E + "[201~" + E + "[Ac");
    CHECK_EQ(keys.size(), (size_t)5);
    CHECK(keys[2].kind == KeyKind::Paste && keys[2].text == "PASTE");
    CHECK(keys[3].kind == KeyKind::Up);
    CHECK(keys[4].kind == KeyKind::Char && keys[4].ch == 'c');
    // two pastes in a row
    keys = keys_of(E + "[200~1" + E + "[201~" + E + "[200~2" + E + "[201~");
    CHECK_EQ(keys.size(), (size_t)2);
    CHECK_EQ(keys[0].text, std::string("1"));
    CHECK_EQ(keys[1].text, std::string("2"));
    // not a paste: a modifier on the 200 makes it Unknown, and no text is swallowed
    keys = keys_of(E + E + "[200~x");
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK_EQ(raws(keys), E + E + "[200~x");
}

void test_paste_split() {
    std::string body = "line one\nline " + E + "[A two \xc3\xa9\xe2\x82\xac end" + E;
    std::string input = "x" + E + "[200~" + body + E + "[201~y";
    std::string expect = show(keys_of(input));
    // byte by byte, with a flush after every byte: the flush must not end the paste
    {
        KeyParser p;
        std::vector<Key> keys;
        const size_t opened = 1 + 6;                               // "x" and the opening marker; a flush inside a marker would end it
        p.feed(input.substr(0, opened));
        for (size_t i = opened; i < input.size(); i++) {
            p.feed(&input[i], 1);
            p.flush();
            for (const Key& k : drain(p)) keys.push_back(k);
        }
        for (const Key& k : drain(p)) keys.push_back(k);
        CHECK(!p.pending());
        CHECK_EQ(keys.size(), (size_t)3);
        if (keys.size() == 3) {
            CHECK(keys[1].kind == KeyKind::Paste);
            CHECK_EQ(keys[1].text, body);
            CHECK_EQ(keys[1].raw, E + "[200~" + body + E + "[201~");
        }
    }
    // split inside both markers
    for (size_t cut = 0; cut <= input.size(); cut++) {
        KeyParser p;
        p.feed(input.substr(0, cut));
        p.feed(input.substr(cut));
        p.flush();
        CHECK_EQ(show(drain(p)), expect);
    }
    // a paste that is pending shows as pending, and nothing comes out until the end
    KeyParser p;
    p.feed(E + "[200~abc");
    p.flush();
    Key k;
    CHECK(!p.next(k));
    CHECK(p.pending());
    p.feed(E + "[20");
    p.flush();
    CHECK(!p.next(k));
    p.feed("1~");
    CHECK(p.next(k));
    CHECK(k.kind == KeyKind::Paste && k.text == "abc");
    CHECK(!p.pending());
}

void test_paste_unterminated() {
    // never ended: it keeps waiting, flush or not, and says so
    KeyParser p;
    p.feed(E + "[200~some text");
    p.flush();
    p.flush();
    Key k;
    CHECK(!p.next(k));
    CHECK(p.pending());
    // fewer than the 6 bytes it takes to rule out a closing marker past the cap: still waiting
    const size_t cap = 1 << 20;
    KeyParser q;
    q.feed(E + "[200~" + std::string(cap, 'p') + "tail");
    q.flush();
    CHECK(!q.next(k));
    CHECK(q.pending());
}

void test_paste_cap() {
    const size_t cap = 1 << 20;
    std::string text(cap, 'p');
    // exactly the cap is still a paste
    {
        std::vector<Key> keys = keys_of(E + "[200~" + text + E + "[201~");
        CHECK_EQ(keys.size(), (size_t)1);
        CHECK(keys[0].kind == KeyKind::Paste);
        CHECK_EQ(keys[0].text.size(), cap);
    }
    // one more byte is too much; the same however it is fed
    std::string over = E + "[200~" + text + "q" + E + "[201~" + "z";
    std::string whole = show(keys_of(over));
    std::vector<Key> keys = keys_of(over);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK_EQ(keys[0].raw.size(), cap + 6);
    CHECK_EQ(raws(keys), over);
    CHECK(keys.back().kind == KeyKind::Char && keys.back().ch == 'z');
    for (size_t piece : {(size_t)1, (size_t)7, (size_t)4096, (size_t)100000}) {
        KeyParser p;
        for (size_t i = 0; i < over.size(); i += piece) p.feed(over.substr(i, piece));
        p.flush();
        CHECK_EQ(show(drain(p)), whole);
        CHECK(!p.pending());
    }
    // no end at all, much longer than the cap: given up, no longer pending, nothing lost
    std::string endless = E + "[200~" + std::string(cap + 50000, 'e');
    keys = keys_of(endless);
    CHECK(keys[0].kind == KeyKind::Unknown);
    CHECK_EQ(raws(keys), endless);
}

// ---- flush

void test_flush() {
    {   // a lone ESC waits, and is Escape at the flush
        KeyParser p;
        p.feed(E);
        Key k;
        CHECK(!p.next(k));
        CHECK(p.pending());
        p.flush();
        CHECK(p.next(k));
        CHECK(k.kind == KeyKind::Escape && !k.alt && k.raw == E);
        CHECK(!p.pending());
        CHECK(!p.next(k));
    }
    {   // an ESC, then a flush, then a letter: Escape, then a plain letter (not Alt)
        KeyParser p;
        p.feed(E);
        p.flush();
        p.feed("a");
        p.flush();
        std::vector<Key> keys = drain(p);
        CHECK_EQ(keys.size(), (size_t)2);
        CHECK(keys[0].kind == KeyKind::Escape);
        CHECK(keys[1].kind == KeyKind::Char && !keys[1].alt);
    }
    {   // an ESC, then the next byte before any flush: Alt
        KeyParser p;
        p.feed(E);
        p.feed("a");
        p.flush();
        std::vector<Key> keys = drain(p);
        CHECK_EQ(keys.size(), (size_t)1);
        CHECK(keys[0].kind == KeyKind::Char && keys[0].alt);
    }
    {   // ESC [ then a flush, then the rest: given up as Unknown, then the rest as plain keys
        KeyParser p;
        p.feed(E + "[");
        CHECK(p.pending());
        p.flush();
        p.feed("A");
        std::vector<Key> keys = drain(p);
        CHECK_EQ(keys.size(), (size_t)2);
        CHECK(keys[0].kind == KeyKind::Unknown && keys[0].raw == E + "[");
        CHECK(keys[1].kind == KeyKind::Char && keys[1].ch == 'A');
    }
    {   // an unfinished CSI with parameters
        KeyParser p;
        p.feed(E + "[1;5");
        p.flush();
        std::vector<Key> keys = drain(p);
        CHECK_EQ(keys.size(), (size_t)1);
        CHECK(keys[0].kind == KeyKind::Unknown && keys[0].raw == E + "[1;5");
        CHECK(!p.pending());
    }
    {   // ESC O alone
        KeyParser p;
        p.feed(E + "O");
        p.flush();
        std::vector<Key> keys = drain(p);
        CHECK_EQ(keys.size(), (size_t)1);
        CHECK(keys[0].kind == KeyKind::Unknown && keys[0].raw == E + "O");
    }
    {   // ESC ESC waits for a possible sequence, then is Alt-Escape
        KeyParser p;
        p.feed(E + E);
        CHECK(p.pending());
        p.flush();
        std::vector<Key> keys = drain(p);
        CHECK_EQ(keys.size(), (size_t)1);
        CHECK(keys[0].kind == KeyKind::Escape && keys[0].alt);
    }
    {   // a split UTF-8 character waits; the flush makes it FFFD
        KeyParser p;
        p.feed("\xe2\x82");
        Key k;
        CHECK(!p.next(k));
        CHECK(p.pending());
        p.feed("\xac");
        CHECK(p.next(k));
        CHECK_EQ((unsigned)k.ch, 0x20ACu);
        CHECK(!p.pending());
        p.feed("\xf0\x9f");
        p.flush();
        CHECK(p.next(k));
        CHECK_EQ((unsigned)k.ch, 0xFFFDu);
        CHECK_EQ(k.raw, std::string("\xf0\x9f"));
        CHECK(!p.pending());
        p.feed("\x98");                                            // the tail of it arriving too late: a bad byte of its own
        p.flush();
        CHECK(p.next(k));
        CHECK_EQ((unsigned)k.ch, 0xFFFDu);
    }
    {   // a flush with nothing waiting does nothing
        KeyParser p;
        p.flush();
        Key k;
        CHECK(!p.next(k));
        CHECK(!p.pending());
        p.feed("a");
        p.flush();
        p.flush();
        CHECK_EQ(drain(p).size(), (size_t)1);
    }
    {   // a key that is complete is out before the flush (a complete sequence is never held back)
        KeyParser p;
        p.feed(E + "[A");
        Key k;
        CHECK(p.next(k));
        CHECK(k.kind == KeyKind::Up);
        CHECK(!p.pending());
    }
    {   // next() on nothing leaves the key alone
        KeyParser p;
        Key k;
        k.ch = 77;
        CHECK(!p.next(k));
        CHECK_EQ((unsigned)k.ch, 77u);
    }
}

void test_interleaved() {
    std::string input = "ls -l" + std::string("\r") + E + "[A" + "x" + E + "[1;5C" + "\xc3\xa9" + E + "OP" + E + "z" + "\t" + E + "[Z" + E;
    std::vector<Key> keys = keys_of(input);
    CHECK_EQ(raws(keys), input);
    CHECK_EQ(keys.size(), (size_t)5 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1);
    CHECK(keys[5].kind == KeyKind::Enter);
    CHECK(keys[6].kind == KeyKind::Up);
    CHECK(keys[8].kind == KeyKind::Right && keys[8].ctrl);
    CHECK(keys[9].kind == KeyKind::Char && keys[9].ch == 0xE9);
    CHECK(keys[10].kind == KeyKind::Function && keys[10].number == 1);
    CHECK(keys[11].kind == KeyKind::Char && keys[11].alt && keys[11].ch == 'z');
    CHECK(keys[12].kind == KeyKind::Tab && !keys[12].shift);
    CHECK(keys[13].kind == KeyKind::Tab && keys[13].shift);
    CHECK(keys[14].kind == KeyKind::Escape);
}

// ---- properties on random input

void test_round_trip() {
    std::mt19937 rng(12345);
    for (int round = 0; round < 3000; round++) {
        std::string input = random_stream(rng, 1 + rng() % 60, round % 3);
        KeyParser p;
        p.feed(input);
        p.flush();
        std::vector<Key> keys = drain(p);
        std::string back = raws(keys);
        if (p.pending()) {                                         // only an unfinished paste can still be waiting
            CHECK(input.compare(0, back.size(), back) == 0);
            CHECK(back.size() < input.size());
        } else {
            CHECK_EQ(back, input);
        }
        for (const Key& k : keys) {
            CHECK(!k.raw.empty());
            if (k.kind == KeyKind::Paste) CHECK(k.raw.size() >= 12);
        }
    }
}

void test_chunking() {
    std::mt19937 rng(777);
    for (int round = 0; round < 4000; round++) {
        std::string input = random_stream(rng, 1 + rng() % 80, round % 3);
        // whole, no flush (so the result is comparable with the pieces)
        KeyParser whole;
        whole.feed(input);
        std::string expect = show(drain(whole));
        bool expect_pending = whole.pending();
        // byte by byte
        bool pending = false;
        std::string got = show(in_pieces(input, rng, 1, &pending));
        CHECK_EQ(got, expect);
        CHECK_EQ(pending, expect_pending);
        // random pieces
        for (int t = 0; t < 3; t++) {
            got = show(in_pieces(input, rng, 1 + rng() % 12, &pending));
            CHECK_EQ(got, expect);
            CHECK_EQ(pending, expect_pending);
        }
        if (got != expect) { std::printf("  input: "); for (unsigned char c : input) std::printf("%02x ", c); std::printf("\n"); break; }
    }
}

void test_chunking_with_flush_at_end() {
    // the flush after the last piece settles the same way however the bytes came
    std::mt19937 rng(4242);
    for (int round = 0; round < 1500; round++) {
        std::string input = random_stream(rng, 1 + rng() % 50, round % 3);
        KeyParser a;
        a.feed(input);
        a.flush();
        KeyParser b;
        for (char c : input) b.feed(&c, 1);
        b.flush();
        CHECK_EQ(show(drain(a)), show(drain(b)));
        CHECK_EQ(a.pending(), b.pending());
    }
}

void test_long_garbage() {
    std::mt19937 rng(99);
    for (int style = 0; style < 3; style++) {
        std::string input = random_stream(rng, 300000, style);
        KeyParser p;
        size_t i = 0;
        std::string back;
        while (i < input.size()) {
            size_t n = 1 + rng() % 5000;
            if (n > input.size() - i) n = input.size() - i;
            p.feed(input.data() + i, n);
            i += n;
            for (const Key& k : drain(p)) back += k.raw;
        }
        p.flush();
        for (const Key& k : drain(p)) back += k.raw;
        if (!p.pending()) CHECK_EQ(back, input);
        else CHECK(input.compare(0, back.size(), back) == 0);
    }
    // a huge run of one thing that never completes
    for (const std::string& unit : {std::string("\xe2"), E, E + "[", std::string("1"), E + "[1;"}) {
        std::string input;
        while (input.size() < 200000) input += unit;
        std::vector<Key> keys = keys_of(input);
        CHECK_EQ(raws(keys), input);
    }
    std::string zeros(100000, '\0');
    CHECK_EQ(keys_of(zeros).size(), (size_t)100000);
}

}  // namespace

int main() {
    test_plain();
    test_utf8();
    test_alt();
    test_cursor_keys();
    test_home_end_etc();
    test_function_keys();
    test_keypad();
    test_unknown_sequences();
    test_broken_sequences();
    test_mouse();
    test_paste();
    test_paste_split();
    test_paste_unterminated();
    test_paste_cap();
    test_flush();
    test_interleaved();
    test_round_trip();
    test_chunking();
    test_chunking_with_flush_at_end();
    test_long_garbage();
    if (failures == 0) std::printf("all keys tests passed\n");
    return failures == 0 ? 0 : 1;
}
