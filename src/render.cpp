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
// Bold, italic, underline, strikethrough and blink are passed to vtscreen as cell attributes (the terminal program decides how, and whether, to
// draw them); bold is also drawn brighter (see Theme). A wide character is its first cell (CELL_WIDE, with the letter) and its second (CELL_TAIL).
vtscreen::PicCell to_pic(const Cell& k, const Theme& theme, bool flip) {
    vtscreen::PicCell p;
    p.text = true;

    char32_t ch = k.ch;
    if (k.attrs & ATTR_WIDE_TAIL) ch = U' ';
    else if (ch == 0 || ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF)) ch = U' ';
    if (k.attrs & ATTR_HIDDEN) ch = U' ';
    p.ch = ch;

    // what the terminal is told about the cell (concealed text shows nothing, not even a line under it)
    if (!(k.attrs & ATTR_HIDDEN)) {
        if (k.attrs & ATTR_BOLD) p.attrs |= vtscreen::CELL_BOLD;
        if (k.attrs & ATTR_ITALIC) p.attrs |= vtscreen::CELL_ITALIC;
        if (k.attrs & ATTR_UNDERLINE) p.attrs |= vtscreen::CELL_UNDERLINE;
        if (k.attrs & ATTR_STRIKE) p.attrs |= vtscreen::CELL_STRIKE;
        if (k.attrs & ATTR_BLINK) p.attrs |= vtscreen::CELL_BLINK;
    }
    if (k.attrs & ATTR_WIDE_TAIL) p.attrs |= vtscreen::CELL_TAIL;
    else if (k.attrs & ATTR_WIDE) p.attrs |= vtscreen::CELL_WIDE;

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
               bool cursor_here, int cursor_col, const Theme& theme, int emu_row = -1) {
    if (emu_row < 0) emu_row = row;                  // the screen row drawn on picture row `row`
    for (int c = 0; c < cols; c++) {
        const bool flip = reverse_screen != (cursor_here && c == cursor_col);
        pic.set(row, c, to_pic(emu.cell(emu_row, c), theme, flip));
    }
    for (int c = cols; c < pic.cols; c++) pic.set(row, c, vtscreen::PicCell());      // a screen narrower than the pane (DECCOLM): the rest is blank
}

// A row of the view scrolled back: line `line` of the history followed by the screen (a history line may be a different width: it is
// cut or padded with blanks).
void paint_history_row(const Emulator& emu, vtscreen::Picture& pic, int row, int cols, size_t line, bool reverse_screen, const Theme& theme) {
    const size_t history = emu.scrollback_size();
    if (line >= history) {
        paint_row(emu, pic, row, cols, reverse_screen, false, 0, theme, (int)(line - history));
        return;
    }
    const std::vector<Cell>& cells = emu.scrollback_line(line);
    const Cell blank;
    for (int c = 0; c < cols; c++) {
        const Cell& k = (size_t)c < cells.size() ? cells[(size_t)c] : blank;
        pic.set(row, c, to_pic(k, theme, reverse_screen));
    }
    for (int c = cols; c < pic.cols; c++) pic.set(row, c, vtscreen::PicCell());
}

// The piece (tile.col, tile.row) of `src` as the pixels of a cell: dst at (x, y) gets cw x ch pixels. What the picture does not reach
// (the edge of a picture that is not a whole number of cells) is left as it is: transparent.
void copy_piece(vtscreen::Raster& dst, int x, int y, const vtscreen::Raster& src, const Tile& t, int cw, int ch) {
    const int sx = (int)t.col * cw, sy = (int)t.row * ch;
    const int n = std::min(cw, src.w - sx);
    if (n <= 0) return;
    for (int j = 0; j < ch && sy + j < src.h; j++) {
        const vtscreen::Pixel* from = &src.at(sx, sy + j);
        std::copy(from, from + n, &dst.at(x, y + j));
    }
}

}  // namespace

// Put the pictures of the emulator on `pic`: a Raster for the block of cells that have a piece of a picture in them (the cells shown: the
// live screen, or the view scrolled back), kept from one paint to the next so that only the cells whose piece changed are drawn again.
// A picture's pieces never change (a picture has one number for good), so a cell is the same when its tile is.
void PanePainter::paint_pictures(const Emulator& emu, vtscreen::Picture& pic, int rows, int cols, size_t scroll_back) {
    const int cw = emu.cell_pixel_width(), ch = emu.cell_pixel_height();
    const size_t history = emu.scrollback_size();
    const size_t top = history - scroll_back;                // (scroll_back is held to the history)
    auto tile_at = [&](int r, int c) -> Tile {
        if (scroll_back == 0) return emu.picture_tile(r, c);
        const size_t line = top + (size_t)r;
        if (line >= history) return emu.picture_tile((int)(line - history), c);
        const std::vector<Tile>& v = emu.scrollback_tiles(line);
        return (size_t)c < v.size() ? v[(size_t)c] : Tile();
    };

    // the block of cells that have a piece
    int r0 = rows, r1 = -1, c0 = cols, c1 = -1;
    if (cw > 0 && ch > 0 && (scroll_back > 0 || emu.has_pictures())) {
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                if (!tile_at(r, c).empty()) { r0 = std::min(r0, r); r1 = std::max(r1, r); c0 = std::min(c0, c); c1 = std::max(c1, c); }
    }
    if (r1 < 0) {                                            // none
        if (raster_ || !pic.images.empty()) pic.clear_images();
        raster_.reset();
        pic_rows_ = pic_cols_ = 0;
        pic_tiles_.clear();
        return;
    }

    const int brows = r1 - r0 + 1, bcols = c1 - c0 + 1;
    const bool same_block = raster_ && brows == pic_rows_ && bcols == pic_cols_ && r0 == pic_row0_ && c0 == pic_col0_ && cw == pic_cw_ && ch == pic_ch_;
    if (!same_block) {
        raster_ = std::make_shared<vtscreen::Raster>();
        raster_->init(bcols * cw, brows * ch);
        pic_row0_ = r0; pic_col0_ = c0; pic_rows_ = brows; pic_cols_ = bcols; pic_cw_ = cw; pic_ch_ = ch;
        pic_tiles_.assign((size_t)brows * bcols, Tile());
    }

    std::vector<int> changed;                                // the rows whose pieces are not what the raster shows
    for (int r = 0; r < brows; r++) {
        bool differs = false;
        for (int c = 0; c < bcols && !differs; c++) differs = tile_at(r0 + r, c0 + c) != pic_tiles_[(size_t)r * bcols + c];
        if (differs) changed.push_back(r);
    }
    if (!changed.empty() && raster_.use_count() > 2) {
        // somebody else holds this raster (a copy of the picture kept for a transition, or one being drawn): what they hold must not change under them
        raster_ = std::make_shared<vtscreen::Raster>(*raster_);
    }

    for (const int r : changed) {
        for (int j = 0; j < ch; j++) std::fill(&raster_->at(0, r * ch + j), &raster_->at(0, r * ch + j) + (size_t)bcols * cw, vtscreen::Pixel());
        std::shared_ptr<const vtscreen::Raster> src;
        uint32_t src_id = 0;
        for (int c = 0; c < bcols; c++) {
            const Tile t = tile_at(r0 + r, c0 + c);
            pic_tiles_[(size_t)r * bcols + c] = t;
            if (t.empty()) continue;
            if (t.image != src_id) { src = emu.picture(t.image); src_id = t.image; }
            if (src) copy_piece(*raster_, c * cw, r * ch, *src, t, cw, ch);
        }
    }

    // it is what the picture carries, and the only thing
    if (pic.images.size() != 1 || pic.images[0].raster.get() != raster_.get()) {   // (the block is the same whenever the raster is)
        pic.clear_images();
        pic.place_image(raster_, r0, c0);
    }
}

int PanePainter::paint(Emulator& emu, vtscreen::Picture& pic, bool show_cursor, const Theme& theme, size_t scroll_back) {
    // The emulator's rows are the picture's rows but the last (the bar's). If the picture is not that shape, paint the part
    // that fits rather than write out of range.
    const int rows = std::max(0, std::min(emu.rows(), pic.rows - 1));
    const int cols = std::max(0, std::min(emu.cols(), pic.cols));
    const bool reverse_screen = emu.reverse_video();
    scroll_back = std::min(scroll_back, emu.scrollback_size());

    if (scroll_back > 0) {                                   // browsing the history: every row, no cursor (a Picture notes only real changes)
        const size_t top = emu.scrollback_size() - scroll_back;
        for (int r = 0; r < rows; r++) paint_history_row(emu, pic, r, cols, top + (size_t)r, reverse_screen, theme);
        paint_pictures(emu, pic, rows, cols, scroll_back);
        emu.clear_dirty();
        all_ = true;                                         // coming back to the live screen is a full paint
        last_scroll_back_ = scroll_back;
        return rows;
    }

    const bool full = all_ || last_scroll_back_ != 0 || emu.rows() != last_rows_ || emu.cols() != last_cols_ ||
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

    paint_pictures(emu, pic, rows, cols, 0);
    emu.clear_dirty();
    all_ = false;
    last_scroll_back_ = 0;
    last_rows_ = emu.rows();
    last_cols_ = emu.cols();
    last_serial_ = pic.serial;
    last_reverse_video_ = reverse_screen;
    last_cursor_ = cur;
    last_cursor_shown_ = cursor_shown;
    return written;
}

}  // namespace vtmux
