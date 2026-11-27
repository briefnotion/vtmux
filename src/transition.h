// The blend of two screens: what is shown part of the way from one console to another.
#pragma once

#include <string>
#include <vector>

#include "vtscreen/vtscreen.h"

namespace vtmux {

enum class TransitionStyle {
    Crossfade,        // vtscreen::fade, as vtshow does it: colours mix, the letters change over at the half way point
    Dissolve,         // cell by cell, in a scattered order, each cell changes from one screen's to the other's
    Dip,              // the first screen fades to dark, then the second fades up from dark
    Slide,            // the screens slide along side by side (see compose); blend() itself treats this as a crossfade
};

const char* style_name(TransitionStyle style);                     // "crossfade", "dissolve", "dip", "slide"
bool parse_style(const std::string& name, TransitionStyle& style); // false if it is none of them

// out becomes the picture `pct` (0 to 1) of the way from a to b. a and b are the same size and every cell may be text. out is assigned
// (size, cells and pictures made of pixels) and is not retained. pct is eased by the caller if it wants it. The last row (the bar's) is taken from b.
//
// The pictures made of pixels (Picture::images) go along: a crossfade dissolves them (vtscreen::fade), a dip darkens them as it does the
// letters, and a dissolve changes each cell's pixels over with its letters, which needs the size of a cell in pixels (cell_w, cell_h; with
// 0 the pictures of the nearer screen are used whole). Pictures are only kept when a and b are the same size.
void blend(const vtscreen::Picture& a, const vtscreen::Picture& b, double pct, TransitionStyle style, vtscreen::Picture& out,
           int cell_w = 0, int cell_h = 0);

// ---- sliding: screens placed on a surface, a part of which is shown

// One screen placed with its top left corner at row dy, column dx of the body (every row but the bar's). It may stick out of the body.
struct Layer {
    const vtscreen::Picture* pic;
    int dx, dy;
};

// The pictures made of pixels of a layer move with it (by whole cells, so they stay on their cells); the screen shows only the part in the body.
// (Layers that overlap would have the pixels of the lower one over the letters of the upper: layers are meant to sit side by side.)
// out becomes the layers drawn in order (a later one over an earlier one), cut to the body of `bar`; what no layer covers is blank. The last
// row (the bar's) is taken from `bar`, which also gives the size. out is assigned (size and cells) and is not retained.
// `fill` is what the body is made of before the layers are drawn (the void between screens that slid apart); a default cell is blank.
void compose(const std::vector<Layer>& layers, const vtscreen::Picture& bar, vtscreen::Picture& out,
             const vtscreen::PicCell& fill = vtscreen::PicCell());

// The pictures made of pixels of `pic` without what is in the cells row0 .. row0 + rows - 1, col0 .. col0 + cols - 1 (those cells' pixels are made
// transparent): what is left to show round something drawn over the screen, such as the help card. A cell is cell_w x cell_h pixels.
// A picture that has no pixels in those cells is used as it is (not copied).
std::vector<vtscreen::ImagePlacement> images_without(const vtscreen::Picture& pic, int row0, int col0, int rows, int cols, int cell_w, int cell_h);

// 0 to 1 in, 0 to 1 out: fast at first and slowing to a stop (cubic). Outside 0..1 is held to the ends.
double ease_out(double t);
// The other way: slow at first and speeding up (cubic), for a screen leaving.
double ease_in(double t);

// How long a slide of `steps` consoles takes, given what one step takes: one step takes base_ms, a longer one a little longer (up to twice
// as long), a shorter one (a slide started again part of the way) shorter, but not under 0.3 of a step.
double slide_ms(double steps, double base_ms);

}  // namespace vtmux
