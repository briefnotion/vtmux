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
    // ESC P params final data ESC \ : a device control string with data. Only Sixel graphics (final 'q', no marker and no intermediate byte)
    // is passed on, with the data after the q; every other device control string is skipped to its end, as it always was. Not called
    // for a string that was cancelled (CAN, SUB, or an ESC that did not start ST) or that was longer than the limit.
    virtual void dcs(const Params& params, char marker, std::string_view intermediates, char final, std::string_view data) {
        (void)params; (void)marker; (void)intermediates; (void)final; (void)data;
    }
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
    static constexpr size_t MAX_CLIPBOARD_STRING = 1 << 20;     // the limit for an OSC 52 (clipboard) string: 1 MiB
    static constexpr size_t MAX_SIXEL_STRING = 32u << 20;       // the limit for the data of a Sixel picture: 32 MiB (the rest is dropped, and so is the picture)
    // The limit for the data of a device control string (Sixel); a different one is for tests.
    void set_max_dcs_string(size_t bytes) { max_dcs_string_ = bytes; }
    static constexpr size_t MAX_PARAMS = 32;

private:
    enum class State {
        Ground, Escape, EscapeIntermediate, CsiEntry, CsiParam, CsiIntermediate, CsiIgnore,
        DcsEntry, DcsParam, DcsIntermediate, DcsString,      // ESC P: its parameters, then (for Sixel) its data
        OscString, StringIgnore,         // OSC, and the DCS / SOS / PM / APC strings which are skipped to their end
    };

    void byte(uint8_t b, VtParserEvents& ev);
    void ground(uint8_t b, VtParserEvents& ev);
    void clear_sequence();
    void param_byte(uint8_t b);
    void dispatch_csi(char final, VtParserEvents& ev);
    void finish_string(VtParserEvents& ev);
    void finish_params();                  // the parameter in progress goes on the list
    void dcs_final(uint8_t b);             // the final byte of ESC P ... : Sixel goes on to collect its data, every other string is skipped

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
    size_t max_dcs_string_ = MAX_SIXEL_STRING;
    bool esc_in_string_ = false;        // an ESC inside a string: it ends it if a \ follows
};

}  // namespace vtmux
