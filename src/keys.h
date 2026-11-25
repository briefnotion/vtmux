// KeyParser: the bytes the keyboard sends, as keys. Used in command mode (arrows to switch consoles, the command line);
// in normal mode the bytes go to the program untouched and are never parsed.
//
// A key keeps the exact bytes it came from (`raw`), so it can be passed on as it was.
//
// An ESC with nothing after it is the Escape key, but it also begins every escape sequence (ESC [ A is the up arrow), and the two
// cannot be told apart until the next byte arrives or does not. So: bytes are fed as they come, and flush() is called once a
// frame; an ESC that is still alone at a flush is Escape, and a sequence that never finished is given up as Unknown.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace vtmux {

enum class KeyKind {
    Char,                 // a character: ch (control keys are their code: Ctrl-B is 2; Alt + a key has alt set)
    Up, Down, Left, Right,
    Home, End, PageUp, PageDown, Insert, Delete,
    Function,             // F1 to F24: number
    Escape, Enter, Tab, Backspace,
    Paste,                // text pasted inside a bracketed paste (ESC [ 200 ~ ... ESC [ 201 ~): text; raw is all of it
    Mouse,                // a mouse report (ESC [ < ... M or m, or ESC [ M and three bytes): raw only
    Unknown,              // anything else that was a complete sequence, or one that was given up
};

struct Key {
    KeyKind kind = KeyKind::Unknown;
    char32_t ch = 0;
    int number = 0;
    bool ctrl = false, alt = false, shift = false;     // for the keys that carry them: ESC [ 1 ; 5 C is Ctrl-Right
    std::string text;
    std::string raw;
};

class KeyParser {
public:
    void feed(const char* data, size_t size);
    void feed(const std::string& s) { feed(s.data(), s.size()); }
    bool next(Key& key);               // the next complete key, if there is one
    void flush();                      // see above
    bool pending() const;              // bytes of an unfinished key are waiting

private:
    // (the implementation's own state)
    std::string buffer_;               // bytes that are not a key yet (never anything while a paste is being read)
    std::deque<Key> keys_;
    bool in_paste_ = false;
    std::string paste_;                // while in_paste_: everything after the opening marker
    size_t scan_ = 0;                  // how much of paste_ has been searched for the closing marker
    void parse(bool flushing);         // make keys of buffer_; when flushing, nothing is left waiting (except a paste)
    bool paste_step();                 // look for the end of the paste; true when it ended (or was given up)
};

}  // namespace vtmux
