// PanePainter: what the emulator's screen looks like, as cells of a vtscreen Picture. Only the rows that changed are written (with
// Picture::set, so a retained Picture only notes real changes), and the cursor.
#pragma once

#include "emulator.h"
#include "vtscreen/vtscreen.h"

namespace vtmux {

struct Theme {
    int default_fg[3] = {204, 204, 204};      // what the default foreground colour is drawn as
    bool bold_is_bright = true;               // bold text in palette colours 0 to 7 is drawn as 8 to 15; bold in the default colour is bright white
    double dim = 0.6;                         // dim text: its colour scaled by this
    bool color = true;                        // false: no colour at all (the cells carry none)
};

class PanePainter {
public:
    // The picture was made again, or the theme changed: the next paint writes every row.
    void invalidate() { all_ = true; }

    // Write what changed in `emu` into `pic`: `pic` is a retained Picture with emu.rows() + 1 rows and emu.cols() columns (the last
    // row is the control bar's and is never touched). The cursor is drawn as an inverse cell when show_cursor is true and the emulator's
    // cursor is visible. Clears the emulator's dirty flags (so the caller holds the session lock). Returns how many rows were written.
    // A different size of picture from last time is a full paint.
    //
    // scroll_back: how many lines the view is scrolled back into the history. 0 is the live screen. Above 0 the picture shows the
    // history lines and then the screen, `scroll_back` lines up from the live view, without a cursor (a scroll back greater than the
    // history is held to it).
    int paint(Emulator& emu, vtscreen::Picture& pic, bool show_cursor, const Theme& theme, size_t scroll_back = 0);

private:
    bool all_ = true;
    int last_rows_ = 0, last_cols_ = 0;
    Cursor last_cursor_;
    bool last_cursor_shown_ = false;
    bool last_reverse_video_ = false;
    size_t last_scroll_back_ = 0;
    uint64_t last_serial_ = 0;                // the picture painted last (a different one is a full paint)
};

}  // namespace vtmux
