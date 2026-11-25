#include "keys.h"

#include <string_view>
#include <utility>
#include <vector>

namespace vtmux {

namespace {

constexpr uint8_t ESC = 0x1B;
constexpr char32_t REPLACEMENT = 0xFFFD;
constexpr size_t MAX_SEQUENCE = 64;            // a sequence this long without an end is garbage: give it up
constexpr size_t MAX_PASTE = 1 << 20;          // the most pasted bytes kept waiting for the closing marker
constexpr std::string_view PASTE_BEGIN = "\x1b[200~";
constexpr std::string_view PASTE_END = "\x1b[201~";

// One key's worth of decoding. The functions below return how many bytes the key used, or 0 when more bytes are needed to
// know (never 0 when `flushing`: then what is there is all there will be, and a half-finished key is given up).

// The UTF-8 character at the start of s. Returns 1 (cp, len: a whole character), 0 (need more bytes) or -1 (invalid: the
// first len bytes are the bad sequence, and the byte after them starts something new). Overlong forms, surrogates and
// anything past U+10FFFF are invalid, which the limits on the second byte catch.
int utf8(std::string_view s, bool flushing, char32_t& cp, size_t& len) {
    uint8_t b = (uint8_t)s[0];
    size_t n;
    if (b >= 0xC2 && b <= 0xDF) n = 2;
    else if (b >= 0xE0 && b <= 0xEF) n = 3;
    else if (b >= 0xF0 && b <= 0xF4) n = 4;
    else { len = 1; return -1; }
    uint8_t lo = 0x80, hi = 0xBF;                              // what the second byte may be
    if (b == 0xE0) lo = 0xA0;
    else if (b == 0xED) hi = 0x9F;
    else if (b == 0xF0) lo = 0x90;
    else if (b == 0xF4) hi = 0x8F;
    char32_t v = b & (0xFF >> (n + 1));
    for (size_t i = 1; i < n; i++) {
        if (i >= s.size()) {
            if (!flushing) return 0;
            len = s.size();
            return -1;
        }
        uint8_t c = (uint8_t)s[i];
        if (c < (i == 1 ? lo : 0x80) || c > (i == 1 ? hi : 0xBF)) { len = i; return -1; }
        v = (v << 6) | (c & 0x3F);
    }
    cp = v;
    len = n;
    return 1;
}

// An ASCII byte as a key: the control bytes are Char with their own code, except the few that have a key of their own.
void ascii_key(uint8_t b, Key& key) {
    if (b == 0x0D || b == 0x0A) key.kind = KeyKind::Enter;
    else if (b == 0x09) key.kind = KeyKind::Tab;
    else if (b == 0x7F || b == 0x08) key.kind = KeyKind::Backspace;
    else { key.kind = KeyKind::Char; key.ch = b; }
}

// The modifier parameter of xterm: the number minus one is a bit set (1 shift, 2 alt, 4 ctrl, 8 meta, which counts as alt).
void apply_modifier(int m, Key& key) {
    if (m < 1) return;
    m -= 1;
    if (m & 1) key.shift = true;
    if (m & 10) key.alt = true;
    if (m & 4) key.ctrl = true;
}

// The F number for the code in ESC [ code ~ (0 when it is not one).
int function_number(int code) {
    if (code >= 11 && code <= 15) return code - 10;           // F1 to F5
    if (code >= 17 && code <= 21) return code - 11;           // F6 to F10
    if (code == 23 || code == 24) return code - 12;           // F11, F12
    if (code == 25 || code == 26) return code - 12;           // F13, F14
    if (code == 28 || code == 29) return code - 13;           // F15, F16
    if (code >= 31 && code <= 34) return code - 14;           // F17 to F20
    return 0;
}

// Digits and semicolons to numbers (an empty one is -1). False if anything else is in there.
bool numbers(std::string_view p, std::vector<int>& v) {
    int cur = -1;
    for (char c : p) {
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 65535) cur = 65535;                      // a silly number is still a number
        } else if (c == ';') {
            v.push_back(cur);
            cur = -1;
            if (v.size() > 8) return false;
        } else {
            return false;
        }
    }
    if (!p.empty()) v.push_back(cur);
    return true;
}

// The key a finished CSI or SS3 sequence stands for. False: it is not one we know (the caller makes it Unknown).
bool translate(char final, const std::vector<int>& v, bool ss3, Key& key) {
    int code = -1, mod = -1;
    if (ss3) {                                                 // ESC O 2 P: the one parameter, if any, is the modifier
        if (v.size() > 1) return false;
        if (!v.empty()) mod = v[0];
    } else {
        if (v.size() > 2) return false;
        if (!v.empty()) code = v[0];
        if (v.size() == 2) mod = v[1];
    }
    if (final != '~' && (ss3 || code == -1 || code == 1)) {    // the letter finals (ESC [ A, ESC [ 1 ; 5 A)
        switch (final) {
            case 'A': key.kind = KeyKind::Up; break;
            case 'B': key.kind = KeyKind::Down; break;
            case 'C': key.kind = KeyKind::Right; break;
            case 'D': key.kind = KeyKind::Left; break;
            case 'H': key.kind = KeyKind::Home; break;
            case 'F': key.kind = KeyKind::End; break;
            case 'P': case 'Q': case 'R': case 'S':
                key.kind = KeyKind::Function;
                key.number = final - 'P' + 1;
                break;
            case 'Z':
                if (ss3) return false;
                key.kind = KeyKind::Tab;                       // back tab
                key.shift = true;
                break;
            default:
                if (!ss3) return false;
                // the keypad when the terminal is in application keypad mode (vtmux turns that on to mirror the program)
                if (final == 'M') key.kind = KeyKind::Enter;
                else if (final == 'X') { key.kind = KeyKind::Char; key.ch = '='; }
                else if (final >= 'j' && final <= 'y') { key.kind = KeyKind::Char; key.ch = "*+,-./0123456789"[final - 'j']; }
                else return false;
        }
        apply_modifier(mod, key);
        return true;
    }
    if (final != '~' || ss3) return false;
    switch (code) {
        case 1: case 7: key.kind = KeyKind::Home; break;
        case 2: key.kind = KeyKind::Insert; break;
        case 3: key.kind = KeyKind::Delete; break;
        case 4: case 8: key.kind = KeyKind::End; break;
        case 5: key.kind = KeyKind::PageUp; break;
        case 6: key.kind = KeyKind::PageDown; break;
        default:
            key.number = function_number(code);
            if (key.number == 0) return false;
            key.kind = KeyKind::Function;
    }
    apply_modifier(mod, key);
    return true;
}

// Mouse, SGR form: the parameters are '<' then button;x;y.
bool sgr_mouse(std::string_view body, char final) {
    if (final != 'M' && final != 'm') return false;
    std::vector<int> v;
    if (!numbers(body.substr(1), v) || v.size() != 3) return false;
    return v[0] >= 0 && v[1] >= 0 && v[2] >= 0;
}

// The sequence whose start (ESC [, ESC O, with a second ESC in front for Alt) is in s[0, at). Ends at a final byte 0x40 to
// 0x7E. Anything not allowed inside (a control, ESC, a byte of 0x7F or more) breaks it: what was gathered is Unknown, and the
// byte starts something new. Any sequence we do not know is Unknown too.
size_t sequence(std::string_view s, size_t at, bool ss3, bool alt, bool flushing, Key& key, bool& begins_paste) {
    key.kind = KeyKind::Unknown;
    if (!ss3 && at < s.size() && s[at] == 'M') {                // the old mouse report: exactly three bytes follow, whatever
        size_t total = at + 4;
        if (s.size() < total) return flushing ? s.size() : 0;
        key.kind = KeyKind::Mouse;
        return total;
    }
    for (size_t j = at; j < s.size(); j++) {
        uint8_t b = (uint8_t)s[j];
        if (b < 0x20 || b > 0x7E) return j;                      // broken
        if (b >= 0x40) {                                         // the final byte: complete
            std::string_view body = s.substr(at, j - at);
            if (!ss3 && !alt && b == '~' && body == "200") { begins_paste = true; return j + 1; }
            if (!ss3 && !body.empty() && body[0] == '<') {
                if (sgr_mouse(body, (char)b)) key.kind = KeyKind::Mouse;
                return j + 1;
            }
            std::vector<int> v;
            if (numbers(body, v) && translate((char)b, v, ss3, key)) {
                if (alt) key.alt = true;
            } else {
                key = Key();                                     // translate may have half-filled it
            }
            return j + 1;
        }
        if (j + 1 - at >= MAX_SEQUENCE) return j + 1;           // parameters and no end in sight: garbage
    }
    return flushing ? s.size() : 0;
}

// A key that begins with ESC.
size_t escape(std::string_view s, bool flushing, Key& key, bool& begins_paste) {
    if (s.size() == 1) {                                         // alone: Escape, once we know nothing follows
        if (!flushing) return 0;
        key.kind = KeyKind::Escape;
        return 1;
    }
    uint8_t c = (uint8_t)s[1];
    if (c == '[') return sequence(s, 2, false, false, flushing, key, begins_paste);
    if (c == 'O') return sequence(s, 2, true, false, flushing, key, begins_paste);
    if (c == ESC) {
        // xterm sends ESC ESC for Alt-Escape, and some terminals ESC ESC [ A for Alt-Up: so look one byte further.
        if (s.size() == 2) {
            if (!flushing) return 0;
            key.kind = KeyKind::Escape;
            key.alt = true;
            return 2;
        }
        if (s[2] == '[' || s[2] == 'O') return sequence(s, 3, s[2] == 'O', true, flushing, key, begins_paste);
        key.kind = KeyKind::Escape;
        key.alt = true;
        return 2;
    }
    key.alt = true;                                              // ESC and a character: Alt and that character
    if (c < 0x80) {
        ascii_key(c, key);
        return 2;
    }
    char32_t cp = 0;
    size_t len = 0;
    int r = utf8(s.substr(1), flushing, cp, len);
    if (r == 0) return 0;
    if (r < 0) {                                                 // ESC and garbage: the ESC was a key of its own
        key.alt = false;
        key.kind = KeyKind::Escape;
        return 1;
    }
    key.kind = KeyKind::Char;
    key.ch = cp;
    return 1 + len;
}

size_t decode(std::string_view s, bool flushing, Key& key, bool& begins_paste) {
    uint8_t b = (uint8_t)s[0];
    if (b == ESC) return escape(s, flushing, key, begins_paste);
    if (b < 0x80) {
        ascii_key(b, key);
        return 1;
    }
    char32_t cp = 0;
    size_t len = 0;
    int r = utf8(s, flushing, cp, len);
    if (r == 0) return 0;
    key.kind = KeyKind::Char;
    key.ch = r > 0 ? cp : REPLACEMENT;                           // the bytes stay in raw either way
    return len;
}

}  // namespace

void KeyParser::feed(const char* data, size_t size) {
    if (size == 0) return;
    if (in_paste_) paste_.append(data, size);
    else buffer_.append(data, size);
    parse(false);
}

bool KeyParser::next(Key& key) {
    if (keys_.empty()) return false;
    key = std::move(keys_.front());
    keys_.pop_front();
    return true;
}

void KeyParser::flush() {
    if (!in_paste_ && !buffer_.empty()) parse(true);             // a paste is ended by its marker alone
}

bool KeyParser::pending() const {
    return in_paste_ || !buffer_.empty();
}

// The paste has the opening marker behind it; paste_ is what came after. Ended by the closing marker, or given up when it
// has grown past the cap without one. Both depend only on the bytes, not on how they were fed, so chunking cannot matter.
bool KeyParser::paste_step() {
    size_t end = paste_.find(PASTE_END, scan_);
    if (end != std::string::npos && end <= MAX_PASTE) {
        Key key;
        key.kind = KeyKind::Paste;
        key.text = paste_.substr(0, end);
        key.raw.reserve(end + PASTE_BEGIN.size() + PASTE_END.size());
        key.raw.append(PASTE_BEGIN).append(key.text).append(PASTE_END);
        keys_.push_back(std::move(key));
        buffer_ = paste_.substr(end + PASTE_END.size());
    } else if (end != std::string::npos || paste_.size() >= MAX_PASTE + PASTE_END.size()) {
        // A closing marker could still have started at the cap itself, so it takes that much more to be sure it is not coming.
        Key key;                                                 // Unknown
        key.raw.append(PASTE_BEGIN).append(paste_, 0, MAX_PASTE);
        keys_.push_back(std::move(key));
        buffer_ = paste_.substr(MAX_PASTE);                      // the rest is read as ordinary input
    } else {
        scan_ = paste_.size() >= PASTE_END.size() - 1 ? paste_.size() - (PASTE_END.size() - 1) : 0;   // a marker may be split
        return false;
    }
    in_paste_ = false;
    paste_.clear();
    scan_ = 0;
    return true;
}

void KeyParser::parse(bool flushing) {
    size_t start = 0;
    for (;;) {
        if (in_paste_) {
            if (!paste_step()) break;
            start = 0;                                           // the rest of the input is in buffer_ again
            continue;
        }
        if (start >= buffer_.size()) break;
        std::string_view rest(buffer_.data() + start, buffer_.size() - start);
        Key key;
        bool begins_paste = false;
        size_t used = decode(rest, flushing, key, begins_paste);
        if (used == 0) break;                                    // the start of a key: wait for the rest
        if (begins_paste) {
            paste_.assign(rest.substr(used));
            buffer_.clear();
            start = 0;
            scan_ = 0;
            in_paste_ = true;
            continue;
        }
        key.raw.assign(rest.substr(0, used));
        keys_.push_back(std::move(key));
        start += used;
    }
    if (!in_paste_) buffer_.erase(0, start);
}

}  // namespace vtmux
