#include "render.h"

#include <algorithm>
#include <cmath>

namespace vtmux {

namespace {

// A colour as red, green, blue (0 to 255) for the foreground. `fallback` is what the default colour is drawn as.
void resolve(const Color& c, const int fallback[3], double& r, double& g, double& b) {
    int ri = 0, gi = 0, bi = 0;
    switch (c.kind) {
        case Color::Indexed: palette_rgb(c.a, ri, gi, bi); break;
        case Color::Rgb: ri = c.a; gi = c.b; bi = c.c; break;
        default: ri = fallback[0]; gi = fallback[1]; bi = fallback[2]; break;
    }
    r = ri; g = gi; b = bi;
}

double scale_channel(double v, double k) { return std::floor(v * k + 0.5); }

// One emulator cell as a vtscreen text cell. `flip` toggles inverse video (the cursor, and the reversed screen).
//
// Not drawn, because a vtscreen text cell has no such attribute yet: underline, italic, strikethrough and blink.
// Wide characters: vtscreen cannot draw a character that takes two columns, so the first cell of one is drawn as a '?'
// (with its colours) and the second as a space. A later step adds wide characters to vtscreen; then this is the
// one place to change.
vtscreen::PicCell to_pic(const Cell& k, const Theme& theme, bool flip) {
    vtscreen::PicCell p;
    p.text = true;

    char32_t ch = k.ch;
    if (k.attrs & ATTR_WIDE_TAIL) ch = U' ';
    else if (k.attrs & ATTR_WIDE) ch = U'?';
    else if (ch == 0 || ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF)) ch = U' ';
    if (k.attrs & ATTR_HIDDEN) ch = U' ';
    p.ch = ch;

    p.inv = (((k.attrs & ATTR_REVERSE) != 0) != flip);

    if (!theme.color) return p;                       // no colour at all: r, g, b and the background stay 0

    // the foreground: the colour, brightened by bold, scaled by dim
    Color fg = k.fg;
    if ((k.attrs & ATTR_BOLD) && theme.bold_is_bright) {
        if (fg.kind == Color::Indexed && fg.a < 8) fg.a = (uint8_t)(fg.a + 8);
        else if (fg.kind == Color::Default) fg = Color::rgb(255, 255, 255);
    }
    resolve(fg, theme.default_fg, p.r, p.g, p.b);
    if (k.attrs & ATTR_DIM) {
        p.r = scale_channel(p.r, theme.dim);
        p.g = scale_channel(p.g, theme.dim);
        p.b = scale_channel(p.b, theme.dim);
    }

    // the background is the alt colour; the default one leaves it off so the terminal's own background shows
    if (k.bg.kind != Color::Default) {
        const int none[3] = {0, 0, 0};
        p.alt_lum = 1;
        resolve(k.bg, none, p.alt_r, p.alt_g, p.alt_b);
    }
    return p;
}

void paint_row(const Emulator& emu, vtscreen::Picture& pic, int row, int cols, bool reverse_screen,
               bool cursor_here, int cursor_col, const Theme& theme) {
    for (int c = 0; c < cols; c++) {
        const bool flip = reverse_screen != (cursor_here && c == cursor_col);
        pic.set(row, c, to_pic(emu.cell(row, c), theme, flip));
    }
}

}  // namespace

int PanePainter::paint(Emulator& emu, vtscreen::Picture& pic, bool show_cursor, const Theme& theme) {
    // The emulator's rows are the picture's rows but the last (the bar's). If the picture is not that shape, paint the part
    // that fits rather than write out of range.
    const int rows = std::max(0, std::min(emu.rows(), pic.rows - 1));
    const int cols = std::max(0, std::min(emu.cols(), pic.cols));
    const bool reverse_screen = emu.reverse_video();

    const bool full = all_ || emu.rows() != last_rows_ || emu.cols() != last_cols_ ||
                      pic.serial != last_serial_ || reverse_screen != last_reverse_video_;

    // where the cursor is drawn now
    const Cursor cur = emu.cursor();
    const bool cursor_shown = show_cursor && cur.visible && cur.row >= 0 && cur.row < rows && cur.col >= 0 && cur.col < cols;
    const bool cursor_changed = cursor_shown != last_cursor_shown_ ||
                                (cursor_shown && (cur.row != last_cursor_.row || cur.col != last_cursor_.col));

    int written = 0;
    for (int r = 0; r < rows; r++) {
        bool need = full || emu.row_dirty(r);
        if (!need && cursor_changed) {
            // the row the cursor left, and the row it arrived on
            need = (last_cursor_shown_ && r == last_cursor_.row) || (cursor_shown && r == cur.row);
        }
        if (!need) continue;
        paint_row(emu, pic, r, cols, reverse_screen, cursor_shown && r == cur.row, cur.col, theme);
        written++;
    }

    emu.clear_dirty();
    all_ = false;
    last_rows_ = emu.rows();
    last_cols_ = emu.cols();
    last_serial_ = pic.serial;
    last_reverse_video_ = reverse_screen;
    last_cursor_ = cur;
    last_cursor_shown_ = cursor_shown;
    return written;
}

}  // namespace vtmux
