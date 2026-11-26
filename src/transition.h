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
// (size and cells) and is not retained. pct is eased by the caller if it wants it. The last row (the bar's) is taken from b.
void blend(const vtscreen::Picture& a, const vtscreen::Picture& b, double pct, TransitionStyle style, vtscreen::Picture& out);

// ---- sliding: screens placed on a surface, a part of which is shown

// One screen placed with its top left corner at row dy, column dx of the body (every row but the bar's). It may stick out of the body.
struct Layer {
    const vtscreen::Picture* pic;
    int dx, dy;
};

// out becomes the layers drawn in order (a later one over an earlier one), cut to the body of `bar`; what no layer covers is blank. The last
// row (the bar's) is taken from `bar`, which also gives the size. out is assigned (size and cells) and is not retained.
// `fill` is what the body is made of before the layers are drawn (the void between screens that slid apart); a default cell is blank.
void compose(const std::vector<Layer>& layers, const vtscreen::Picture& bar, vtscreen::Picture& out,
             const vtscreen::PicCell& fill = vtscreen::PicCell());

// 0 to 1 in, 0 to 1 out: fast at first and slowing to a stop (cubic). Outside 0..1 is held to the ends.
double ease_out(double t);
// The other way: slow at first and speeding up (cubic), for a screen leaving.
double ease_in(double t);

// How long a slide of `steps` consoles takes, given what one step takes: one step takes base_ms, a longer one a little longer (up to twice
// as long), a shorter one (a slide started again part of the way) shorter, but not under 0.3 of a step.
double slide_ms(double steps, double base_ms);

}  // namespace vtmux
