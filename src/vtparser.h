// VtParser: turns the bytes a program writes to a terminal into events: a character to print, a control
// character to carry out, an escape sequence, a control sequence (CSI), a string (OSC). It knows nothing about
// screens; the emulator (emulator.h) gives the events their meaning.
//
// It is the state machine of the DEC ANSI parser (as in Paul Williams' diagram, which xterm, tmux and every other
// terminal emulator follow), so that:
//   - a sequence may be split across any two reads; the parser just waits for the rest,
//   - a control character inside a sequence is carried out at once (the sequence goes on),
//   - CAN and SUB cancel a sequence, and ESC starts a new one,
//   - what is not understood is ignored up to its end instead of being printed,
//   - UTF-8 is decoded here, a bad or cut-off sequence giving U+FFFD (not an exception, not a crash).
// 8-bit C1 controls are not used (in UTF-8 those bytes are parts of characters).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vtmux {

// The numbers of a control sequence: "1;31;4:3" is {1, 31, 4, 3} with the 3 marked as a sub-parameter (it followed a
// colon). A number that was left out ("1;;3") is -1, so the one that handles it can choose its own default.
struct Params {
    std::vector<int> value;
    std::vector<uint8_t> sub;           // 1: this one followed a colon, not a semicolon
    size_t size() const { return value.size(); }
    bool empty() const { return value.empty(); }
    // the i'th number, or fallback if there is none or it was left out
    int get(size_t i, int fallback) const { return i < value.size() && value[i] >= 0 ? value[i] : fallback; }
    // as get(), but 0 also means the fallback (the usual rule: "ESC [ 0 A" moves one, like "ESC [ A")
    int get_nonzero(size_t i, int fallback) const { const int v = get(i, fallback); return v == 0 ? fallback : v; }
};

class VtParserEvents {
public:
    virtual ~VtParserEvents() = default;
    virtual void print(char32_t cp) = 0;                         // a character
    virtual void execute(uint8_t control) = 0;                   // a C0 control: BS, HT, LF, CR, SO, SI, BEL ...
    // ESC [ ... : marker is '?', '>', '<' or '=' when the parameters began with one, else 0
    virtual void csi(const Params& params, char marker, std::string_view intermediates, char final) = 0;
    virtual void esc(std::string_view intermediates, char final) = 0;     // ESC (B, ESC 7, ESC M, ESC # 8 ...
    virtual void osc(std::string_view text) = 0;                 // ESC ] text BEL (or ESC \), for example "0;a title"
};

class VtParser {
public:
    // Feed bytes; events are called as they complete.
    void feed(const char* data, size_t size, VtParserEvents& events);
    void feed(std::string_view s, VtParserEvents& events) { feed(s.data(), s.size(), events); }

    // VT52 mode: there are no control sequences, only ESC and one character (ESC [ is an ESC followed by a [ that is dropped).
    void set_vt52(bool on) { vt52_ = on; }
    bool vt52() const { return vt52_; }

    // How long a string (OSC) may be before the rest is dropped.
    static constexpr size_t MAX_STRING = 4096;
    static constexpr size_t MAX_PARAMS = 32;

private:
    enum class State {
        Ground, Escape, EscapeIntermediate, CsiEntry, CsiParam, CsiIntermediate, CsiIgnore,
        OscString, StringIgnore,         // OSC, and DCS / SOS / PM / APC strings which are skipped to their end
    };

    void byte(uint8_t b, VtParserEvents& ev);
    void ground(uint8_t b, VtParserEvents& ev);
    void clear_sequence();
    void param_byte(uint8_t b);
    void dispatch_csi(char final, VtParserEvents& ev);
    void finish_string(VtParserEvents& ev);

    State state_ = State::Ground;
    bool vt52_ = false;
    // UTF-8 in progress
    char32_t cp_ = 0;
    int utf8_left_ = 0;
    char32_t utf8_min_ = 0;
    // the sequence in progress
    Params params_;
    bool have_digit_ = false;           // the current parameter has a digit (or is a given 0)
    int current_ = -1;
    bool next_is_sub_ = false;
    char marker_ = 0;
    std::string intermediates_;
    std::string string_;
    bool string_too_long_ = false;
    bool esc_in_string_ = false;        // an ESC inside a string: it ends it if a \ follows
};

}  // namespace vtmux
