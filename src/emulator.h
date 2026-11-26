// Emulator: a terminal in memory. It takes the bytes a program writes to its terminal (feed), keeps the screen
// those bytes make, and says what to send back to the program (answers to its questions). It knows nothing about
// ptys, threads or drawing: the screen is plain data a pane can be drawn from, and the whole thing can be tested by
// feeding it text and reading the cells.
//
// What it does, as xterm and tmux do it: a cursor with the deferred wrap at the right edge; scroll regions;
// scrolling with the lines that go off the top kept as scrollback; erasing with the current background colour;
// insert and delete of characters and lines; tab stops; the alternate screen (vim, less); the cursor save and
// restore; DEC special graphics (line drawing); wide characters (two cells); every SGR attribute with the 16, 256
// and 24-bit colours; the modes a host needs to know (application cursor keys, bracketed paste, the mouse and focus
// reporting, cursor visibility); the title; and the replies (device status, cursor position, device attributes).
//
// Not done: a combining mark is dropped (a cell holds one character), the VT52 mode, sixel, double-width lines.
// A resize keeps the top left of the screen and does not reflow lines.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "vtparser.h"

namespace vtmux {

// A colour: the terminal's default, one of the 256 palette entries, or 24-bit. Palette entries are only resolved
// when something is drawn (see palette_rgb), so a theme can change without touching the screen.
struct Color {
    enum Kind : uint8_t { Default = 0, Indexed = 1, Rgb = 2 };
    uint8_t kind = Default;
    uint8_t a = 0, b = 0, c = 0;               // Indexed: a is the index. Rgb: a, b, c are red, green, blue.

    static Color index(int i) { Color k; k.kind = Indexed; k.a = (uint8_t)i; return k; }
    static Color rgb(int r, int g, int bl) { Color k; k.kind = Rgb; k.a = (uint8_t)r; k.b = (uint8_t)g; k.c = (uint8_t)bl; return k; }
    bool operator==(const Color& o) const { return kind == o.kind && a == o.a && b == o.b && c == o.c; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

// The colour of a palette entry (0 to 255): the 16 standard colours, the 6 x 6 x 6 cube, the 24 greys.
void palette_rgb(int index, int& r, int& g, int& b);

enum Attr : uint16_t {
    ATTR_BOLD = 1, ATTR_DIM = 2, ATTR_ITALIC = 4, ATTR_UNDERLINE = 8, ATTR_BLINK = 16, ATTR_REVERSE = 32,
    ATTR_HIDDEN = 64, ATTR_STRIKE = 128,
    ATTR_WIDE = 256,                           // the first cell of a character that takes two
    ATTR_WIDE_TAIL = 512,                      // the second: it holds no character of its own
};

struct Cell {
    char32_t ch = U' ';
    Color fg, bg;
    uint16_t attrs = 0;

    bool operator==(const Cell& o) const { return ch == o.ch && fg == o.fg && bg == o.bg && attrs == o.attrs; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};
static_assert(sizeof(Cell) == 16, "a cell is 16 bytes");

struct Cursor {
    int row = 0, col = 0;
    bool visible = true;
    int shape = 0;                             // DECSCUSR: 0 default, 1 and 2 block, 3 and 4 underline, 5 and 6 bar (odd = blinking)
    bool operator==(const Cursor& o) const { return row == o.row && col == o.col && visible == o.visible && shape == o.shape; }
    bool operator!=(const Cursor& o) const { return !(*this == o); }
};

class Emulator : private VtParserEvents {
public:
    Emulator(int rows, int cols, size_t scrollback_lines = 2000);

    // The program's output.
    void feed(const char* data, size_t size);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    // The terminal changed size. The top left stays; the rest is cut or padded; a cursor that would be off the
    // bottom scrolls the screen up (into the scrollback) instead.
    void resize(int rows, int cols);

    int rows() const { return rows_; }
    int cols() const { return cols_; }

    // The screen being shown (the alternate one while a program such as vim has switched to it).
    const Cell& cell(int row, int col) const { return screen()[(size_t)row * cols_ + col]; }
    Cursor cursor() const { return cursor_; }
    bool alt_screen() const { return use_alt_; }

    // Rows changed since clear_dirty(): what a pane needs to redraw.
    bool row_dirty(int row) const { return dirty_[(size_t)row] != 0; }
    bool any_dirty() const { return any_dirty_; }
    void clear_dirty();
    void mark_all_dirty();

    // Lines that scrolled off the top of the main screen: 0 is the oldest. (A line keeps the width it had.)
    size_t scrollback_size() const { return scrollback_.size(); }
    const std::vector<Cell>& scrollback_line(size_t i) const { return scrollback_[i]; }
    size_t scrollback_limit() const { return scrollback_limit_; }

    // What the program asked of the terminal, for the host to write back to it. Taking it empties it.
    std::string take_reply() { std::string s; s.swap(reply_); return s; }
    // Clipboard writes the program asked for (OSC 52), each as the whole sequence to send to the real terminal (ESC ] 52 ; selection ; base64 BEL).
    // Only valid writes are kept: a query (?), an empty or broken one is dropped, and only the last few wait to be taken.
    bool has_clipboard() const { return !clipboard_.empty(); }
    std::vector<std::string> take_clipboard() { std::vector<std::string> v; v.swap(clipboard_); return v; }

    // Modes the host needs: how to send keys and mouse and focus, and whether to wrap a paste.
    bool application_cursor_keys() const { return app_cursor_keys_; }
    bool application_keypad() const { return app_keypad_; }
    bool bracketed_paste() const { return bracketed_paste_; }
    bool focus_reporting() const { return focus_reporting_; }
    int mouse_tracking() const { return mouse_tracking_; }          // 0 none, 9 / 1000 clicks, 1002 and drags, 1003 all motion
    bool mouse_sgr() const { return mouse_sgr_; }                   // 1006: the extended encoding
    bool reverse_video() const { return reverse_video_; }           // DECSCNM: the whole screen reversed
    const std::string& title() const { return title_; }
    int bell_count() const { return bell_count_; }

private:
    // ---- the events (VtParserEvents)
    void print(char32_t cp) override;
    void execute(uint8_t control) override;
    void csi(const Params& params, char marker, std::string_view intermediates, char final) override;
    void esc(std::string_view intermediates, char final) override;
    void osc(std::string_view text) override;

    // ---- the screen
    std::vector<Cell>& screen() { return use_alt_ ? alt_ : main_; }
    const std::vector<Cell>& screen() const { return use_alt_ ? alt_ : main_; }
    Cell& at(int row, int col) { return screen()[(size_t)row * cols_ + col]; }
    Cell blank() const;                        // an empty cell with the current background (erasing keeps the colour)
    void mark_dirty(int row) { dirty_[(size_t)row] = 1; any_dirty_ = true; }
    void erase_cells(int row, int from, int to);        // from <= col < to
    void erase_rows(int from, int to);                  // from <= row < to
    void scroll_up(int top, int bottom, int n, bool keep_history = true);   // keep_history false: the lines are deleted (DL), not scrolled off
    void scroll_down(int top, int bottom, int n);
    void fix_wide_at(int row, int col);                 // blank the other half of a wide character that is about to be broken
    void open_gap(int row, int col, int n);             // shift the rest of the row right by n (ICH, and a character in insert mode)
    void close_gap(int row, int col, int n);            // shift the rest of the row left over n cells (DCH)

    // ---- the cursor
    void put_char(char32_t cp);
    void linefeed();
    void reverse_index();
    void carriage_return() { cursor_.col = 0; wrap_pending_ = false; }
    void move_to(int row, int col);                     // as CUP: the origin mode and the margins applied
    void set_row(int row) { cursor_.row = row; wrap_pending_ = false; }
    void set_col(int col) { cursor_.col = col; wrap_pending_ = false; }
    int top_limit() const { return origin_mode_ ? scroll_top_ : 0; }
    int bottom_limit() const { return origin_mode_ ? scroll_bottom_ : rows_ - 1; }
    void horizontal_tab(int count);
    void back_tab(int count);
    void save_cursor();
    void restore_cursor();

    // ---- sequences
    void sgr(const Params& p);
    void set_mode(const Params& p, bool private_mode, bool on);
    void set_private_mode(int mode, bool on);
    void use_alt(bool on, bool clear);
    void device_status(const Params& p, char marker);
    void clear_screen_home();                  // the screen is cleared and the cursor goes home
    void resize_to(int rows, int cols);        // the grid is this size (what resize does, without changing the pane's width)
    void set_columns_mode(bool wide);          // DECCOLM: 132 (wide) or 80 columns, within the pane; the screen clears
    void soft_reset();
    void full_reset();
    void reset_tab_stops();
    char32_t translate(char32_t cp) const;

    int rows_ = 0, cols_ = 0;
    int pane_cols_ = 0;                        // the width of the pane (cols_ is less than that while DECCOLM has the screen at 80 or 132)
    bool allow_deccolm_ = false;               // mode 40: the program allows the 80/132 column switch (as xterm needs); `reset` does not set it
    std::vector<Cell> main_, alt_;
    bool use_alt_ = false;
    std::vector<uint8_t> dirty_;
    bool any_dirty_ = true;

    std::deque<std::vector<Cell>> scrollback_;
    size_t scrollback_limit_ = 0;

    Cursor cursor_;
    bool wrap_pending_ = false;
    int scroll_top_ = 0, scroll_bottom_ = 0;
    std::vector<uint8_t> tab_stops_;

    // the pen: what new characters are drawn with
    Color pen_fg_, pen_bg_;
    uint16_t pen_attrs_ = 0;

    // character sets: G0 and G1, and which is in use (SO and SI)
    enum class Charset : uint8_t { Ascii, Graphics, Uk };
    Charset g0_ = Charset::Ascii, g1_ = Charset::Ascii;
    bool shifted_out_ = false;

    // VT52 mode (CSI ? 2 l to enter, ESC < to leave): its own short escape sequences and no auto wrap
    bool vt52_ = false;
    int vt52_y_pending_ = 0;                   // after ESC Y: the row and then the column bytes are still to come
    int vt52_row_ = 0;
    void vt52_escape(char final);

    // what DECSC saves
    struct Saved {
        int row = 0, col = 0;
        bool wrap_pending = false, origin_mode = false;
        Color fg, bg;
        uint16_t attrs = 0;
        Charset g0 = Charset::Ascii, g1 = Charset::Ascii;
        bool shifted_out = false;
    };
    Saved saved_, saved_alt_;                  // each screen keeps its own
    Saved saved_1049_;                         // and the one the alternate screen restores on leaving

    // modes
    bool auto_wrap_ = true, origin_mode_ = false, insert_mode_ = false, newline_mode_ = false;
    bool app_cursor_keys_ = false, app_keypad_ = false, bracketed_paste_ = false, focus_reporting_ = false;
    bool reverse_video_ = false, reverse_wrap_ = false;
    int mouse_tracking_ = 0;
    bool mouse_sgr_ = false;

    std::string title_;
    std::string reply_;
    std::vector<std::string> clipboard_;
    int bell_count_ = 0;
    char32_t last_printed_ = 0;                // for REP

    VtParser parser_;
};

}  // namespace vtmux
