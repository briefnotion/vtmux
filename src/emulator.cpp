#include "emulator.h"

#include <algorithm>

#include "width.h"

namespace vtmux {

// ------------------------------------------------------------------------------------------------ colours

void palette_rgb(int index, int& r, int& g, int& b) {
    static const int STANDARD[16][3] = {
        {0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0}, {0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
        {127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    index = std::min(std::max(index, 0), 255);
    if (index < 16) { r = STANDARD[index][0]; g = STANDARD[index][1]; b = STANDARD[index][2]; return; }
    if (index < 232) {
        static const int LEVEL[6] = {0, 95, 135, 175, 215, 255};
        const int i = index - 16;
        r = LEVEL[i / 36]; g = LEVEL[(i / 6) % 6]; b = LEVEL[i % 6];
        return;
    }
    r = g = b = 8 + 10 * (index - 232);
}

namespace {

// DEC special graphics, ESC ( 0: what the characters 0x5F to 0x7E draw
char32_t dec_graphics(char32_t cp) {
    static const char32_t TABLE[32] = {
        U' ',  U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°',
        U'±', U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼',
        U'⎺', U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴',
        U'┬', U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·'};
    return cp >= 0x5F && cp <= 0x7E ? TABLE[cp - 0x5F] : cp;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ set up

Emulator::Emulator(int rows, int cols, size_t scrollback_lines) : scrollback_limit_(scrollback_lines) {
    rows_ = std::max(rows, 1);
    cols_ = std::max(cols, 1);
    pane_cols_ = cols_;
    main_.assign((size_t)rows_ * cols_, Cell());
    alt_.assign((size_t)rows_ * cols_, Cell());
    dirty_.assign((size_t)rows_, 1);
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    reset_tab_stops();
}

void Emulator::reset_tab_stops() {
    tab_stops_.assign((size_t)cols_, 0);
    for (int c = 8; c < cols_; c += 8) tab_stops_[(size_t)c] = 1;
}

void Emulator::clear_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), (uint8_t)0);
    any_dirty_ = false;
}

void Emulator::mark_all_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), (uint8_t)1);
    any_dirty_ = true;
}

void Emulator::feed(const char* data, size_t size) {
    parser_.feed(data, size, *this);
}

Cell Emulator::blank() const {
    Cell c;
    c.bg = pen_bg_;
    return c;
}

// ------------------------------------------------------------------------------------------------ the screen

// If a wide character's other half is in the way of what is about to be written or erased, that half is blanked.
void Emulator::fix_wide_at(int row, int col) {
    if (col < 0 || col >= cols_) return;
    const uint16_t a = at(row, col).attrs;
    if ((a & ATTR_WIDE) && col + 1 < cols_) at(row, col + 1) = blank();
    if ((a & ATTR_WIDE_TAIL) && col > 0) at(row, col - 1) = blank();
}

// Make room for n cells at col by shifting the rest of the row right: a wide character that would be cut by the gap is
// blanked (both halves), and one pushed to the last column with its second half off the edge is blanked too.
void Emulator::open_gap(int row, int col, int n) {
    if (n <= 0 || col >= cols_) return;
    if (col > 0 && (at(row, col).attrs & ATTR_WIDE_TAIL)) { at(row, col - 1) = blank(); at(row, col) = blank(); }
    for (int c = cols_ - 1; c >= col + n; c--) at(row, c) = at(row, c - n);
    for (int c = col; c < std::min(col + n, cols_); c++) at(row, c) = blank();
    if (at(row, cols_ - 1).attrs & ATTR_WIDE) at(row, cols_ - 1) = blank();
    mark_dirty(row);
}

// Take n cells away at col by shifting the rest of the row left, without leaving half a wide character.
void Emulator::close_gap(int row, int col, int n) {
    n = std::min(n, cols_ - col);
    if (n <= 0) return;
    if (col > 0 && (at(row, col).attrs & ATTR_WIDE_TAIL)) at(row, col - 1) = blank();           // its first half is outside, and stays
    if (col + n < cols_ && (at(row, col + n).attrs & ATTR_WIDE_TAIL)) at(row, col + n) = blank();   // its first half is deleted
    for (int c = col; c + n < cols_; c++) at(row, c) = at(row, c + n);
    const Cell b = blank();
    for (int c = cols_ - n; c < cols_; c++) at(row, c) = b;
    mark_dirty(row);
}

void Emulator::erase_cells(int row, int from, int to) {
    from = std::max(from, 0);
    to = std::min(to, cols_);
    if (from >= to) return;
    if (from > 0 && (at(row, from).attrs & ATTR_WIDE_TAIL)) at(row, from - 1) = blank();
    if (to < cols_ && (at(row, to - 1).attrs & ATTR_WIDE)) at(row, to) = blank();
    const Cell b = blank();
    for (int c = from; c < to; c++) at(row, c) = b;
    mark_dirty(row);
}

void Emulator::erase_rows(int from, int to) {
    for (int r = std::max(from, 0); r < std::min(to, rows_); r++) erase_cells(r, 0, cols_);
}

void Emulator::scroll_up(int top, int bottom, int n, bool keep_history) {
    n = std::min(n, bottom - top + 1);
    if (n <= 0) return;
    if (keep_history && !use_alt_ && top == 0 && scrollback_limit_ > 0) {     // lines leave through the top of the main screen: keep them (as tmux and xterm do, with or without a bottom margin)
        for (int r = 0; r < n; r++) {
            scrollback_.emplace_back(main_.begin() + (size_t)r * cols_, main_.begin() + (size_t)(r + 1) * cols_);
            if (scrollback_.size() > scrollback_limit_) scrollback_.pop_front();
        }
    }
    std::vector<Cell>& g = screen();
    std::move(g.begin() + (size_t)(top + n) * cols_, g.begin() + (size_t)(bottom + 1) * cols_, g.begin() + (size_t)top * cols_);
    const Cell b = blank();
    std::fill(g.begin() + (size_t)(bottom - n + 1) * cols_, g.begin() + (size_t)(bottom + 1) * cols_, b);
    for (int r = top; r <= bottom; r++) mark_dirty(r);
}

void Emulator::scroll_down(int top, int bottom, int n) {
    n = std::min(n, bottom - top + 1);
    if (n <= 0) return;
    std::vector<Cell>& g = screen();
    std::move_backward(g.begin() + (size_t)top * cols_, g.begin() + (size_t)(bottom + 1 - n) * cols_, g.begin() + (size_t)(bottom + 1) * cols_);
    const Cell b = blank();
    std::fill(g.begin() + (size_t)top * cols_, g.begin() + (size_t)(top + n) * cols_, b);
    for (int r = top; r <= bottom; r++) mark_dirty(r);
}

// ------------------------------------------------------------------------------------------------ the cursor

void Emulator::linefeed() {
    wrap_pending_ = false;
    if (cursor_.row == scroll_bottom_) scroll_up(scroll_top_, scroll_bottom_, 1);
    else if (cursor_.row < rows_ - 1) cursor_.row++;
}

void Emulator::reverse_index() {
    wrap_pending_ = false;
    if (cursor_.row == scroll_top_) scroll_down(scroll_top_, scroll_bottom_, 1);
    else if (cursor_.row > 0) cursor_.row--;
}

void Emulator::move_to(int row, int col) {
    if (origin_mode_) row += scroll_top_;
    cursor_.row = std::min(std::max(row, top_limit()), bottom_limit());
    cursor_.col = std::min(std::max(col, 0), cols_ - 1);
    wrap_pending_ = false;
}

void Emulator::horizontal_tab(int count) {
    wrap_pending_ = false;
    for (int i = 0; i < count; i++) {
        int c = cursor_.col + 1;
        while (c < cols_ - 1 && !tab_stops_[(size_t)c]) c++;
        cursor_.col = std::min(c, cols_ - 1);
    }
}

void Emulator::back_tab(int count) {
    wrap_pending_ = false;
    for (int i = 0; i < count; i++) {
        int c = cursor_.col - 1;
        while (c > 0 && !tab_stops_[(size_t)c]) c--;
        cursor_.col = std::max(c, 0);
    }
}

void Emulator::save_cursor() {
    Saved& s = use_alt_ ? saved_alt_ : saved_;
    s.row = cursor_.row; s.col = cursor_.col; s.wrap_pending = wrap_pending_; s.origin_mode = origin_mode_;
    s.fg = pen_fg_; s.bg = pen_bg_; s.attrs = pen_attrs_;
    s.g0 = g0_; s.g1 = g1_; s.shifted_out = shifted_out_;
}

void Emulator::restore_cursor() {
    const Saved& s = use_alt_ ? saved_alt_ : saved_;
    origin_mode_ = s.origin_mode;
    cursor_.row = std::min(std::max(s.row, 0), rows_ - 1);
    cursor_.col = std::min(std::max(s.col, 0), cols_ - 1);
    wrap_pending_ = s.wrap_pending && cursor_.col == cols_ - 1;
    pen_fg_ = s.fg; pen_bg_ = s.bg; pen_attrs_ = s.attrs;
    g0_ = s.g0; g1_ = s.g1; shifted_out_ = s.shifted_out;
}

char32_t Emulator::translate(char32_t cp) const {
    const Charset set = shifted_out_ ? g1_ : g0_;
    if (set == Charset::Graphics) return dec_graphics(cp);
    if (set == Charset::Uk && cp == U'#') return U'\u00A3';                     // the pound sign
    return cp;
}

// ------------------------------------------------------------------------------------------------ printing

void Emulator::put_char(char32_t cp) {
    const int w = char_width(cp);
    if (w == 0) return;                                          // a combining mark or a joiner: there is no room for it in a cell
    if (w == 2 && cols_ < 2) return;
    const bool wraps = auto_wrap_ && !vt52_;
    if (wrap_pending_ && wraps) {
        cursor_.col = 0;
        linefeed();
    }
    wrap_pending_ = false;
    if (w == 2 && cursor_.col == cols_ - 1) {                    // it does not fit in what is left of the line
        if (!wraps) return;
        erase_cells(cursor_.row, cursor_.col, cols_);
        cursor_.col = 0;
        linefeed();
    }
    const int row = cursor_.row, col = cursor_.col;
    if (insert_mode_) open_gap(row, col, w);                     // the rest of the line moves right
    fix_wide_at(row, col);
    if (w == 2) fix_wide_at(row, col + 1);
    Cell c;
    c.ch = cp; c.fg = pen_fg_; c.bg = pen_bg_; c.attrs = (uint16_t)(pen_attrs_ | (w == 2 ? ATTR_WIDE : 0));
    at(row, col) = c;
    if (w == 2) {
        Cell tail;
        tail.fg = pen_fg_; tail.bg = pen_bg_; tail.attrs = (uint16_t)(pen_attrs_ | ATTR_WIDE_TAIL);
        at(row, col + 1) = tail;
    }
    mark_dirty(row);
    cursor_.col += w;
    if (cursor_.col >= cols_) {
        cursor_.col = cols_ - 1;
        if (wraps) wrap_pending_ = true;
    }
}

void Emulator::print(char32_t cp) {
    if (vt52_y_pending_ > 0) {                                   // VT52 direct cursor addressing: ESC Y row column, each + 32
        const int v = (int)cp - 32;
        if (vt52_y_pending_ == 2) { vt52_row_ = v; vt52_y_pending_ = 1; return; }
        vt52_y_pending_ = 0;
        cursor_.row = std::min(std::max(vt52_row_, 0), rows_ - 1);
        cursor_.col = std::min(std::max(v, 0), cols_ - 1);
        wrap_pending_ = false;
        return;
    }
    cp = translate(cp);
    last_printed_ = cp;
    put_char(cp);
}

void Emulator::execute(uint8_t control) {
    switch (control) {
        case 0x07: bell_count_++; break;
        case 0x08:                                               // BS
            wrap_pending_ = false;
            if (cursor_.col > 0) cursor_.col--;
            else if (reverse_wrap_ && cursor_.row > 0) { cursor_.row--; cursor_.col = cols_ - 1; }
            break;
        case 0x09: horizontal_tab(1); break;
        case 0x0A: case 0x0B: case 0x0C:                         // LF, VT, FF
            linefeed();
            if (newline_mode_) carriage_return();
            break;
        case 0x0D: carriage_return(); break;
        case 0x0E: shifted_out_ = true; break;                   // SO: use G1
        case 0x0F: shifted_out_ = false; break;                  // SI: use G0
        default: break;
    }
}

// ------------------------------------------------------------------------------------------------ sequences

void Emulator::osc(std::string_view text) {
    const size_t semi = text.find(';');
    if (semi == std::string_view::npos) return;
    const std::string_view num = text.substr(0, semi);
    if (num == "0" || num == "2") title_ = std::string(text.substr(semi + 1));
}

void Emulator::vt52_escape(char final) {
    wrap_pending_ = false;
    switch (final) {
        case 'A': if (cursor_.row > 0) cursor_.row--; break;
        case 'B': if (cursor_.row < rows_ - 1) cursor_.row++; break;
        case 'C': if (cursor_.col < cols_ - 1) cursor_.col++; break;
        case 'D': if (cursor_.col > 0) cursor_.col--; break;
        case 'H': cursor_.row = 0; cursor_.col = 0; break;
        case 'I': reverse_index(); break;
        case 'J': erase_cells(cursor_.row, cursor_.col, cols_); erase_rows(cursor_.row + 1, rows_); break;
        case 'K': erase_cells(cursor_.row, cursor_.col, cols_); break;
        case 'Y': vt52_y_pending_ = 2; break;
        case 'Z': reply_ += "\x1b/Z"; break;
        case 'F': g0_ = Charset::Graphics; shifted_out_ = false; break;
        case 'G': g0_ = Charset::Ascii; shifted_out_ = false; break;
        case '=': app_keypad_ = true; break;
        case '>': app_keypad_ = false; break;
        case '<': vt52_ = false; vt52_y_pending_ = 0; parser_.set_vt52(false); break;       // back to ANSI mode
        default: break;                                                                     // anything else is dropped
    }
}

void Emulator::esc(std::string_view inter, char final) {
    if (vt52_) { vt52_escape(final); return; }
    if (inter.empty()) {
        switch (final) {
            case '7': save_cursor(); break;
            case '8': restore_cursor(); break;
            case 'D': linefeed(); break;                         // IND
            case 'E': carriage_return(); linefeed(); break;      // NEL
            case 'M': reverse_index(); break;                    // RI
            case 'H': tab_stops_[(size_t)cursor_.col] = 1; break;                 // HTS
            case 'Z': reply_ += "\x1b[?1;2c"; break;             // DECID: the same answer as device attributes
            case 'c': full_reset(); break;                       // RIS
            case '=': app_keypad_ = true; break;
            case '>': app_keypad_ = false; break;
            default: break;
        }
        return;
    }
    if (inter == "(" || inter == ")") {
        Charset set;
        if (final == '0' || final == '2') set = Charset::Graphics;      // 2: the alternate character ROM with graphics
        else if (final == 'B' || final == '1') set = Charset::Ascii;
        else if (final == 'A') set = Charset::Uk;
        else return;                                                    // the other sets are not drawn differently
        if (inter == "(") g0_ = set; else g1_ = set;
        return;
    }
    if (inter == "#" && final == '8') {                          // DECALN: fill the screen with E
        scroll_top_ = 0; scroll_bottom_ = rows_ - 1;
        for (int r = 0; r < rows_; r++) {
            for (int c = 0; c < cols_; c++) { Cell e; e.ch = U'E'; at(r, c) = e; }
            mark_dirty(r);
        }
        move_to(0, 0);
    }
}

void Emulator::sgr(const Params& p) {
    if (p.empty()) { pen_fg_ = Color(); pen_bg_ = Color(); pen_attrs_ = 0; return; }
    auto set = [&](uint16_t a, bool on) { if (on) pen_attrs_ = (uint16_t)(pen_attrs_ | a); else pen_attrs_ = (uint16_t)(pen_attrs_ & ~a); };
    for (size_t i = 0; i < p.size(); i++) {
        const int code = p.value[i] < 0 ? 0 : p.value[i];
        if (p.sub[i]) continue;                                  // a sub-parameter belongs to the one before it; it was taken there
        switch (code) {
            case 0: pen_fg_ = Color(); pen_bg_ = Color(); pen_attrs_ = 0; break;
            case 1: set(ATTR_BOLD, true); break;
            case 2: set(ATTR_DIM, true); break;
            case 3: set(ATTR_ITALIC, true); break;
            case 4: {                                            // 4, or 4:0 (none) to 4:5 (the styles, all drawn as one)
                const bool off = i + 1 < p.size() && p.sub[i + 1] && p.value[i + 1] == 0;
                set(ATTR_UNDERLINE, !off);
                break;
            }
            case 5: case 6: set(ATTR_BLINK, true); break;
            case 7: set(ATTR_REVERSE, true); break;
            case 8: set(ATTR_HIDDEN, true); break;
            case 9: set(ATTR_STRIKE, true); break;
            case 21: set(ATTR_UNDERLINE, true); break;           // double underline
            case 22: set(ATTR_BOLD, false); set(ATTR_DIM, false); break;
            case 23: set(ATTR_ITALIC, false); break;
            case 24: set(ATTR_UNDERLINE, false); break;
            case 25: set(ATTR_BLINK, false); break;
            case 27: set(ATTR_REVERSE, false); break;
            case 28: set(ATTR_HIDDEN, false); break;
            case 29: set(ATTR_STRIKE, false); break;
            case 39: pen_fg_ = Color(); break;
            case 49: pen_bg_ = Color(); break;
            case 38: case 48: {
                // 38;5;n  38;2;r;g;b   or the colon forms 38:5:n  38:2:r:g:b  38:2:colour-space:r:g:b
                std::vector<int> v;
                size_t used = i;
                if (i + 1 < p.size() && p.sub[i + 1]) {          // colon form: take the sub-parameters
                    while (used + 1 < p.size() && p.sub[used + 1]) { used++; v.push_back(p.value[used]); }
                } else {                                         // semicolon form: the kind, then the numbers
                    if (i + 1 < p.size()) {
                        const int kind = p.value[i + 1];
                        const size_t want = kind == 5 ? 2 : (kind == 2 ? 4 : 0);
                        if (want > 0 && i + want < p.size()) { for (size_t k = 1; k <= want; k++) v.push_back(p.value[i + k]); used = i + want; }
                        else used = p.size() - 1;                // too short: ignore the rest
                    }
                }
                Color c;
                bool ok = false;
                auto in_range = [](int x) { return x >= 0 && x <= 255; };           // out of range: the whole setting is ignored (as xterm and tmux do)
                if (!v.empty() && v[0] == 5 && v.size() >= 2) { if (in_range(v[1])) { c = Color::index(v[1]); ok = true; } }
                else if (!v.empty() && v[0] == 2) {
                    if (v.size() >= 5) { if (in_range(v[2]) && in_range(v[3]) && in_range(v[4])) { c = Color::rgb(v[2], v[3], v[4]); ok = true; } }   // with a colour space id
                    else if (v.size() >= 4) { if (in_range(v[1]) && in_range(v[2]) && in_range(v[3])) { c = Color::rgb(v[1], v[2], v[3]); ok = true; } }
                }
                if (ok) { if (code == 38) pen_fg_ = c; else pen_bg_ = c; }
                i = used;
                break;
            }
            default:
                if (code >= 30 && code <= 37) pen_fg_ = Color::index(code - 30);
                else if (code >= 40 && code <= 47) pen_bg_ = Color::index(code - 40);
                else if (code >= 90 && code <= 97) pen_fg_ = Color::index(code - 90 + 8);
                else if (code >= 100 && code <= 107) pen_bg_ = Color::index(code - 100 + 8);
                break;
        }
    }
}

void Emulator::use_alt(bool on, bool clear) {
    if (on == use_alt_) return;
    use_alt_ = on;
    if (on && clear) {
        const Cell b = Cell();
        std::fill(alt_.begin(), alt_.end(), b);
    }
    mark_all_dirty();
}

void Emulator::set_private_mode(int mode, bool on) {
    switch (mode) {
        case 1: app_cursor_keys_ = on; break;
        case 2:                                                  // DECANM: reset enters VT52 mode (leaving it is ESC <)
            if (!on) { vt52_ = true; vt52_y_pending_ = 0; parser_.set_vt52(true); }
            break;
        case 3: set_columns_mode(on); break;                     // DECCOLM: 132 (set) or 80 (reset) columns
        case 40: allow_deccolm_ = on; if (!on && cols_ != pane_cols_) resize_to(rows_, pane_cols_); break;
        case 5: reverse_video_ = on; mark_all_dirty(); break;
        case 6: origin_mode_ = on; move_to(0, 0); break;
        case 7: auto_wrap_ = on; if (!on) wrap_pending_ = false; break;
        case 25: cursor_.visible = on; break;
        case 45: reverse_wrap_ = on; break;
        case 47: use_alt(on, false); break;
        case 1047: use_alt(on, true); break;
        case 1048: if (on) save_cursor(); else restore_cursor(); break;
        case 1049:
            if (on) {
                if (!use_alt_) {
                    save_cursor();
                    saved_1049_ = saved_;
                    use_alt(true, true);
                }
            } else if (use_alt_) {
                use_alt(false, false);
                saved_ = saved_1049_;
                restore_cursor();
            }
            break;
        case 9: case 1000: case 1002: case 1003:
            if (on) mouse_tracking_ = mode; else if (mouse_tracking_ == mode) mouse_tracking_ = 0;
            break;
        case 1004: focus_reporting_ = on; break;
        case 1006: mouse_sgr_ = on; break;
        case 2004: bracketed_paste_ = on; break;
        case 66: app_keypad_ = on; break;
        default: break;                                          // the rest (blink, 132 columns, sync ...) change nothing here
    }
}

void Emulator::set_mode(const Params& p, bool private_mode, bool on) {
    for (size_t i = 0; i < p.size(); i++) {
        const int m = p.value[i];
        if (private_mode) set_private_mode(m, on);
        else if (m == 4) insert_mode_ = on;
        else if (m == 20) newline_mode_ = on;
    }
}

// DECCOLM. The real terminal cannot be told to change its width, so the screen is 80 or 132 columns of the pane (the rest of the pane stays
// blank; a pane narrower than that gets all of it). It only happens if the program allowed it with mode 40, as xterm needs: `reset`, which
// sends the mode 3 reset, does not narrow anything. Either way the screen clears and the cursor goes home.
void Emulator::set_columns_mode(bool wide) {
    if (allow_deccolm_) resize_to(rows_, std::min(wide ? 132 : 80, pane_cols_));
    clear_screen_home();
}

void Emulator::clear_screen_home() {
    scroll_top_ = 0; scroll_bottom_ = rows_ - 1;
    erase_rows(0, rows_);
    move_to(0, 0);
}

void Emulator::device_status(const Params& p, char marker) {
    const int what = p.get(0, 0);
    if (marker == '?') {                                         // the DEC ones
        if (what == 6) {                                         // DECXCPR: the cursor position, as the DEC private form
            const int row = cursor_.row - (origin_mode_ ? scroll_top_ : 0);
            reply_ += "\x1b[?" + std::to_string(row + 1) + ";" + std::to_string(cursor_.col + 1) + "R";
        }
        else if (what == 15) reply_ += "\x1b[?13n";                   // no printer
        else if (what == 25) reply_ += "\x1b[?21n";              // user defined keys are locked
        else if (what == 26) reply_ += "\x1b[?27;1n";            // keyboard: North American
        return;
    }
    if (marker == 0 && what == 5) reply_ += "\x1b[0n";
    else if (marker == 0 && what == 6) {
        const int row = cursor_.row - (origin_mode_ ? scroll_top_ : 0);
        reply_ += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(cursor_.col + 1) + "R";
    }
}

void Emulator::soft_reset() {
    pen_fg_ = Color(); pen_bg_ = Color(); pen_attrs_ = 0;
    origin_mode_ = false; insert_mode_ = false; app_cursor_keys_ = false; app_keypad_ = false;
    cursor_.visible = true; cursor_.shape = 0;
    g0_ = g1_ = Charset::Ascii;
    shifted_out_ = false;
    vt52_ = false; vt52_y_pending_ = 0; parser_.set_vt52(false);
    scroll_top_ = 0; scroll_bottom_ = rows_ - 1;
    saved_ = Saved(); saved_alt_ = Saved();
    wrap_pending_ = false;
}

void Emulator::full_reset() {
    soft_reset();
    use_alt_ = false;
    std::fill(main_.begin(), main_.end(), Cell());
    std::fill(alt_.begin(), alt_.end(), Cell());
    auto_wrap_ = true; newline_mode_ = false; bracketed_paste_ = false; focus_reporting_ = false; reverse_video_ = false;
    reverse_wrap_ = false; mouse_tracking_ = 0; mouse_sgr_ = false;
    allow_deccolm_ = false;
    if (cols_ != pane_cols_) resize_to(rows_, pane_cols_);        // the full pane again
    cursor_ = Cursor();
    reset_tab_stops();
    mark_all_dirty();
}

void Emulator::csi(const Params& p, char marker, std::string_view inter, char final) {
    // the few that have an intermediate byte
    if (!inter.empty()) {
        if (inter == " " && final == 'q' && marker == 0) cursor_.shape = std::min(std::max(p.get(0, 0), 0), 6);        // DECSCUSR
        else if (inter == "!" && final == 'p' && marker == 0) soft_reset();                                          // DECSTR
        return;
    }
    if (marker == '>') {
        if (final == 'c') reply_ += "\x1b[>0;0;0c";                                                                   // secondary device attributes
        return;
    }
    if (marker == '<' || marker == '=') return;
    if (marker == '?') {
        if (final == 'h' || final == 'l') set_mode(p, true, final == 'h');
        else if (final == 'n') device_status(p, '?');
        else if (final == 'J') csi(p, 0, inter, 'J');                                                                // DECSED: as ED
        else if (final == 'K') csi(p, 0, inter, 'K');
        return;
    }

    const int n1 = p.get_nonzero(0, 1);
    switch (final) {
        case '@':                                                                                                     // ICH
            open_gap(cursor_.row, cursor_.col, std::min(n1, cols_ - cursor_.col));
            wrap_pending_ = false;
            break;
        case 'A': cursor_.row = std::max(cursor_.row - n1, cursor_.row >= scroll_top_ ? scroll_top_ : 0); wrap_pending_ = false; break;
        case 'B': cursor_.row = std::min(cursor_.row + n1, cursor_.row <= scroll_bottom_ ? scroll_bottom_ : rows_ - 1); wrap_pending_ = false; break;
        case 'C': case 'a': cursor_.col = std::min(cursor_.col + n1, cols_ - 1); wrap_pending_ = false; break;
        case 'D': cursor_.col = std::max(cursor_.col - n1, 0); wrap_pending_ = false; break;
        case 'E': cursor_.row = std::min(cursor_.row + n1, cursor_.row <= scroll_bottom_ ? scroll_bottom_ : rows_ - 1); set_col(0); break;
        case 'F': cursor_.row = std::max(cursor_.row - n1, cursor_.row >= scroll_top_ ? scroll_top_ : 0); set_col(0); break;
        case 'G': case '`': set_col(std::min(n1, cols_) - 1); break;
        case 'H': case 'f': move_to(p.get_nonzero(0, 1) - 1, p.get_nonzero(1, 1) - 1); break;
        case 'I': horizontal_tab(n1); break;
        case 'Z': back_tab(n1); break;
        case 'J': {                                                                                                   // ED
            const int what = p.get(0, 0);
            if (what == 0) { erase_cells(cursor_.row, cursor_.col, cols_); erase_rows(cursor_.row + 1, rows_); }
            else if (what == 1) { erase_rows(0, cursor_.row); erase_cells(cursor_.row, 0, cursor_.col + 1); }
            else if (what == 2) erase_rows(0, rows_);
            else if (what == 3) scrollback_.clear();
            break;
        }
        case 'K': {                                                                                                   // EL
            const int what = p.get(0, 0);
            if (what == 0) erase_cells(cursor_.row, cursor_.col, cols_);
            else if (what == 1) erase_cells(cursor_.row, 0, cursor_.col + 1);
            else if (what == 2) erase_cells(cursor_.row, 0, cols_);
            break;
        }
        case 'L':                                                                                                     // IL
            if (cursor_.row >= scroll_top_ && cursor_.row <= scroll_bottom_) { scroll_down(cursor_.row, scroll_bottom_, n1); carriage_return(); }
            break;
        case 'M':                                                                                                     // DL
            if (cursor_.row >= scroll_top_ && cursor_.row <= scroll_bottom_) { scroll_up(cursor_.row, scroll_bottom_, n1, false); carriage_return(); }
            break;
        case 'P':                                                                                                     // DCH
            close_gap(cursor_.row, cursor_.col, n1);
            wrap_pending_ = false;
            break;
        case 'S': scroll_up(scroll_top_, scroll_bottom_, n1); break;
        case 'T': scroll_down(scroll_top_, scroll_bottom_, n1); break;
        case 'X': erase_cells(cursor_.row, cursor_.col, cursor_.col + n1); break;                                     // ECH
        case 'b':                                                                                                     // REP
            if (last_printed_) for (int i = 0; i < std::min(n1, 65535); i++) put_char(last_printed_);
            break;
        case 'c': if (p.get(0, 0) == 0) reply_ += "\x1b[?1;2c"; break;                                                // device attributes
        case 'x':                                                                                                     // DECREQTPARM: the terminal parameters
            if (p.get(0, 0) == 0) reply_ += "\x1b[2;1;1;112;112;1;0x";
            else if (p.get(0, 0) == 1) reply_ += "\x1b[3;1;1;112;112;1;0x";
            break;
        case 'd': move_to(n1 - 1, cursor_.col); break;
        case 'e': cursor_.row = std::min(cursor_.row + n1, cursor_.row <= scroll_bottom_ ? scroll_bottom_ : rows_ - 1); wrap_pending_ = false; break;      // VPR: like CUD
        case 'g':                                                                                                     // TBC
            if (p.get(0, 0) == 0) tab_stops_[(size_t)cursor_.col] = 0;
            else if (p.get(0, 0) == 3) std::fill(tab_stops_.begin(), tab_stops_.end(), (uint8_t)0);
            break;
        case 'h': set_mode(p, false, true); break;
        case 'l': set_mode(p, false, false); break;
        case 'm': sgr(p); break;
        case 'n': device_status(p, marker); break;
        case 'r': {                                                                                                   // DECSTBM
            const int top = p.get_nonzero(0, 1) - 1;
            const int bottom = std::min(p.get_nonzero(1, rows_), rows_) - 1;
            if (top < bottom) { scroll_top_ = top; scroll_bottom_ = bottom; move_to(0, 0); }
            break;
        }
        case 's': save_cursor(); break;
        case 'u': restore_cursor(); break;
        default: break;                                                                                               // window operations and the rest: not ours
    }
}

// ------------------------------------------------------------------------------------------------ resize

void Emulator::resize(int rows, int cols) {
    pane_cols_ = std::max(cols, 1);                              // the pane's own size: whatever DECCOLM did is over
    resize_to(rows, cols);
}

void Emulator::resize_to(int rows, int cols) {
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);
    if (rows == rows_ && cols == cols_) return;

    // If the cursor would be below the new bottom, the lines above it scroll up and off (into the scrollback of the
    // main screen) so that it stays where it is on the screen.
    auto shift_up = [&](std::vector<Cell>& grid, int shift, bool keep_history) {
        if (shift <= 0) return;
        shift = std::min(shift, rows_);
        if (keep_history && scrollback_limit_ > 0) {
            for (int r = 0; r < shift; r++) {
                scrollback_.emplace_back(grid.begin() + (size_t)r * cols_, grid.begin() + (size_t)(r + 1) * cols_);
                if (scrollback_.size() > scrollback_limit_) scrollback_.pop_front();
            }
        }
        std::move(grid.begin() + (size_t)shift * cols_, grid.end(), grid.begin());
        std::fill(grid.end() - (ptrdiff_t)((size_t)shift * cols_), grid.end(), Cell());
    };
    const int main_cursor_row = use_alt_ ? saved_1049_.row : cursor_.row;
    const int main_shift = main_cursor_row - (rows - 1);
    shift_up(main_, main_shift, true);
    if (use_alt_) {
        shift_up(alt_, cursor_.row - (rows - 1), false);
        cursor_.row -= std::max(0, cursor_.row - (rows - 1));
        saved_1049_.row -= std::max(0, main_shift);
    } else {
        cursor_.row -= std::max(0, main_shift);
    }

    auto reshape = [&](std::vector<Cell>& grid) {
        std::vector<Cell> next((size_t)rows * cols, Cell());
        for (int r = 0; r < std::min(rows, rows_); r++)
            for (int c = 0; c < std::min(cols, cols_); c++) next[(size_t)r * cols + c] = grid[(size_t)r * cols_ + c];
        // a wide character cut by the new edge leaves half of itself: blank it
        for (int r = 0; r < rows; r++) {
            Cell& last = next[(size_t)r * cols + cols - 1];
            if (last.attrs & ATTR_WIDE) last = Cell();
        }
        grid.swap(next);
    };
    reshape(main_);
    reshape(alt_);

    rows_ = rows; cols_ = cols;
    dirty_.assign((size_t)rows_, 1);
    any_dirty_ = true;
    scroll_top_ = 0; scroll_bottom_ = rows_ - 1;
    cursor_.row = std::min(std::max(cursor_.row, 0), rows_ - 1);
    cursor_.col = std::min(std::max(cursor_.col, 0), cols_ - 1);
    wrap_pending_ = false;
    for (Saved* s : {&saved_, &saved_alt_, &saved_1049_}) {
        s->row = std::min(std::max(s->row, 0), rows_ - 1);
        s->col = std::min(std::max(s->col, 0), cols_ - 1);
    }
    std::vector<uint8_t> old_stops = tab_stops_;
    tab_stops_.assign((size_t)cols_, 0);
    for (int c = 0; c < cols_; c++) tab_stops_[(size_t)c] = (size_t)c < old_stops.size() ? old_stops[(size_t)c] : (c % 8 == 0 && c > 0);
}

}  // namespace vtmux
