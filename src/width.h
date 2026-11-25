// How many cells a character takes on a terminal: 0 (a combining mark, a joiner), 1, or 2 (wide: Chinese, Japanese,
// Korean, fullwidth forms, emoji). The tables are generated (tools/gen_width.py), not written by hand.
#pragma once

namespace vtmux {

int char_width(char32_t cp);

}  // namespace vtmux
