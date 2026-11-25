// The blend of two screens: what is shown part of the way from one console to another.
#pragma once

#include <string>

#include "vtscreen/vtscreen.h"

namespace vtmux {

enum class TransitionStyle {
    Crossfade,        // vtscreen::fade, as vtshow does it: colours mix, the letters change over at the half way point
    Dissolve,         // cell by cell, in a scattered order, each cell changes from one screen's to the other's
    Dip,              // the first screen fades to dark, then the second fades up from dark
};

const char* style_name(TransitionStyle style);                     // "crossfade", "dissolve", "dip"
bool parse_style(const std::string& name, TransitionStyle& style); // false if it is none of them

// out becomes the picture `pct` (0 to 1) of the way from a to b. a and b are the same size and every cell may be text. out is assigned
// (size and cells) and is not retained. pct is eased by the caller if it wants it. The last row (the bar's) is taken from b.
void blend(const vtscreen::Picture& a, const vtscreen::Picture& b, double pct, TransitionStyle style, vtscreen::Picture& out);

}  // namespace vtmux
