#include "vtparser.h"

namespace vtmux {

namespace {
constexpr char32_t REPLACEMENT = 0xFFFD;
}

void VtParser::feed(const char* data, size_t size, VtParserEvents& events) {
    for (size_t i = 0; i < size; i++) byte((uint8_t)data[i], events);
}

void VtParser::clear_sequence() {
    params_.value.clear();
    params_.sub.clear();
    have_digit_ = false;
    current_ = -1;
    next_is_sub_ = false;
    marker_ = 0;
    intermediates_.clear();
}

// the byte that begins or continues a parameter list (0x30 to 0x3F)
void VtParser::param_byte(uint8_t b) {
    if (b >= '0' && b <= '9') {
        have_digit_ = true;
        current_ = (current_ < 0 ? 0 : current_) * 10 + (b - '0');
        if (current_ > 65535) current_ = 65535;                  // a silly number is still a number
    } else if (b == ';' || b == ':') {
        if (params_.value.size() < MAX_PARAMS) {
            params_.value.push_back(have_digit_ ? current_ : -1);
            params_.sub.push_back(next_is_sub_ ? 1 : 0);
        }
        have_digit_ = false;
        current_ = -1;
        next_is_sub_ = (b == ':');
    }
}

void VtParser::dispatch_csi(char final, VtParserEvents& ev) {
    // the last parameter, if there was anything at all (an empty list stays empty)
    if (have_digit_ || !params_.value.empty() || next_is_sub_) {
        if (params_.value.size() < MAX_PARAMS) {
            params_.value.push_back(have_digit_ ? current_ : -1);
            params_.sub.push_back(next_is_sub_ ? 1 : 0);
        }
    }
    ev.csi(params_, marker_, intermediates_, final);
}

void VtParser::finish_string(VtParserEvents& ev) {
    if (state_ == State::OscString && !string_too_long_) ev.osc(string_);
    string_.clear();
    string_too_long_ = false;
    esc_in_string_ = false;
    state_ = State::Ground;
}

void VtParser::ground(uint8_t b, VtParserEvents& ev) {
    if (utf8_left_ > 0) {
        if ((b & 0xC0) == 0x80) {                                // a continuation
            cp_ = (cp_ << 6) | (b & 0x3F);
            if (--utf8_left_ == 0) {
                // too long a form, a surrogate, or beyond Unicode: not a character
                if (cp_ < utf8_min_ || (cp_ >= 0xD800 && cp_ <= 0xDFFF) || cp_ > 0x10FFFF) ev.print(REPLACEMENT);
                else ev.print(cp_);
            }
            return;
        }
        utf8_left_ = 0;                                          // it was cut short: one bad character, then this byte as it is
        ev.print(REPLACEMENT);
    }
    if (b < 0x80) { ev.print(b); return; }
    if ((b & 0xE0) == 0xC0) { cp_ = b & 0x1F; utf8_left_ = 1; utf8_min_ = 0x80; }
    else if ((b & 0xF0) == 0xE0) { cp_ = b & 0x0F; utf8_left_ = 2; utf8_min_ = 0x800; }
    else if ((b & 0xF8) == 0xF0) { cp_ = b & 0x07; utf8_left_ = 3; utf8_min_ = 0x10000; }
    else ev.print(REPLACEMENT);                                  // a stray continuation, or 0xF8 and up
}

void VtParser::byte(uint8_t b, VtParserEvents& ev) {
    // Strings first: their own rules for BEL and ESC.
    if (state_ == State::OscString || state_ == State::StringIgnore) {
        if (esc_in_string_) {
            esc_in_string_ = false;
            if (b == '\\') { finish_string(ev); return; }        // ESC \ : the end
            // an ESC that was not the start of ST: the string is abandoned and this byte starts an escape sequence
            string_.clear(); string_too_long_ = false;
            clear_sequence();
            state_ = State::Escape;
            byte(b, ev);
            return;
        }
        if (b == 0x1B) { esc_in_string_ = true; return; }
        if (b == 0x07 && state_ == State::OscString) { finish_string(ev); return; }     // BEL ends an OSC
        if (b == 0x18 || b == 0x1A) { string_.clear(); string_too_long_ = false; state_ = State::Ground; return; }   // CAN, SUB
        if (state_ == State::OscString) {
            // a clipboard write (OSC 52) may be a whole page of text; any other string is kept short
            const size_t limit = (string_.size() >= 3 && string_.compare(0, 3, "52;") == 0) ? MAX_CLIPBOARD_STRING : MAX_STRING;
            if (string_.size() < limit) string_ += (char)b; else string_too_long_ = true;
        }
        return;
    }

    // C0 controls act at once in every state, and CAN, SUB and ESC also end a sequence.
    if (b == 0x18 || b == 0x1A) {
        state_ = State::Ground;
        utf8_left_ = 0;
        return;
    }
    if (b == 0x1B) {
        if (utf8_left_ > 0) { utf8_left_ = 0; ev.print(REPLACEMENT); }
        clear_sequence();
        state_ = State::Escape;
        return;
    }
    if (b < 0x20) {
        if (state_ == State::Ground && utf8_left_ > 0) { utf8_left_ = 0; ev.print(REPLACEMENT); }
        ev.execute(b);
        return;
    }
    if (b == 0x7F) return;                                       // DEL is ignored everywhere

    switch (state_) {
        case State::Ground:
            ground(b, ev);
            return;

        case State::Escape:
            if (vt52_) {                                          // ESC and one character, whatever it is
                if (b >= 0x20 && b <= 0x7E) ev.esc("", (char)b);
                state_ = State::Ground;
                return;
            }
            if (b == '[') { clear_sequence(); state_ = State::CsiEntry; }
            else if (b == ']') { string_.clear(); string_too_long_ = false; state_ = State::OscString; }
            else if (b == 'P' || b == 'X' || b == '^' || b == '_') { string_.clear(); state_ = State::StringIgnore; }   // DCS, SOS, PM, APC
            else if (b >= 0x20 && b <= 0x2F) { intermediates_ += (char)b; state_ = State::EscapeIntermediate; }
            else if (b >= 0x30 && b <= 0x7E) { ev.esc(intermediates_, (char)b); state_ = State::Ground; }
            return;

        case State::EscapeIntermediate:
            if (b >= 0x20 && b <= 0x2F) { if (intermediates_.size() < 4) intermediates_ += (char)b; }
            else if (b >= 0x30 && b <= 0x7E) { ev.esc(intermediates_, (char)b); state_ = State::Ground; }
            return;

        case State::CsiEntry:
            if (b >= 0x3C && b <= 0x3F) { marker_ = (char)b; state_ = State::CsiParam; }
            else if ((b >= '0' && b <= '9') || b == ';' || b == ':') { param_byte(b); state_ = State::CsiParam; }
            else if (b >= 0x20 && b <= 0x2F) { intermediates_ += (char)b; state_ = State::CsiIntermediate; }
            else if (b >= 0x40 && b <= 0x7E) { dispatch_csi((char)b, ev); state_ = State::Ground; }
            return;

        case State::CsiParam:
            if ((b >= '0' && b <= '9') || b == ';' || b == ':') param_byte(b);
            else if (b >= 0x3C && b <= 0x3F) state_ = State::CsiIgnore;          // a marker in the middle: not a sequence we know
            else if (b >= 0x20 && b <= 0x2F) { intermediates_ += (char)b; state_ = State::CsiIntermediate; }
            else if (b >= 0x40 && b <= 0x7E) { dispatch_csi((char)b, ev); state_ = State::Ground; }
            return;

        case State::CsiIntermediate:
            if (b >= 0x20 && b <= 0x2F) { if (intermediates_.size() < 4) intermediates_ += (char)b; }
            else if (b >= 0x30 && b <= 0x3F) state_ = State::CsiIgnore;
            else if (b >= 0x40 && b <= 0x7E) { dispatch_csi((char)b, ev); state_ = State::Ground; }
            return;

        case State::CsiIgnore:
            if (b >= 0x40 && b <= 0x7E) state_ = State::Ground;                   // skipped to its end
            return;

        case State::OscString:
        case State::StringIgnore:
            return;                                                               // handled above
    }
}

}  // namespace vtmux
