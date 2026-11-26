# vtmux

A terminal multiplexer with sliding and fading transitions. Run `vtmux` in a console and you get what looks like the same console, with one extra row at the
bottom: the control bar. Everything works as it did. Press `Ctrl-B` and the bar changes colour: you are in command mode, where you can
make and close consoles by typing a command after a colon, or flip between consoles with the left and right arrow keys, and the
screens slide (or fade) into each other.

![vtmux](docs/demo_sheet.png)

*Top left: a console and the blue bar. Top right: after `Ctrl-B` the bar turns amber. Bottom left: halfway through a fade between two
consoles. Bottom right: the second console.*

It is built on [vtscreen](https://github.com/briefnotion/vtscreen), the same drawing library as vtshow and vtweather, with its
retained pictures: an idle console costs next to nothing to draw. The terminal emulator inside (`src/emulator.*`) is new, a state
machine parser and a screen in memory that is tested against `tmux` and `pyte`.

## Build and run

Linux, `g++` with C++17, `cmake` 3.16 or later, `git`:

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux/build
    ./m
    ./vtmux

`./m` (in the `build` directory) builds vtmux, or rebuilds it after a change. It also fetches the `vtscreen` submodule if the clone was made
without it, and tells you if `cmake` or a compiler is missing. `./m test` builds the tests too and runs them, and `./m clean` removes what
was built. The same thing by hand: `cmake -B build && cmake --build build`. `-DVTMUX_TESTS=OFF` skips building the tests.

    vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]

With no command the first console runs your shell (`$SHELL`). Programs inside run with `TERM=xterm-256color`.

## Keys

In normal mode every key goes to the program in the console, untouched (Ctrl-C and Ctrl-Z too). The one key watched is `Ctrl-B`.

| in command mode (after `Ctrl-B`) | does |
|---|---|
| `Left`, `Right` | the previous, the next console, with the transition (a slide along the row of consoles; from the last, `Right` slides all the way back to the first); it stays in command mode so you can keep flipping |
| `1` to `9` | that console, and back to normal mode |
| `c`, `n`, `p` | a new console, the next, the previous (as in tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | look back through the console's history (2000 lines); the bar shows how far back. `End` returns to the live screen |
| `:` | type a command (Enter runs it, Esc gives up, Backspace edits, Ctrl-U clears) |
| `Esc`, `Enter` | back to normal mode (and to the live screen). Typing into the console does the same |
| `Ctrl-B` | send a real Ctrl-B to the program, and back to normal mode |

## Sliding

The consoles sit side by side in a row, in the order on the bar. Switching slides the view along the row, so both screens show part of the way
and a jump over several consoles shows the ones between going past. A key pressed while a slide is moving starts the next one from where the
view is now. A new console (`new`) goes in next to the one shown, on its right, in two steps: first the old one slides off to the left, leaving empty space (the void, a dark blue by default), then the new one drops in from the top. Closing is two steps too: the closed console drops off the bottom, then the one to its left (or the next, if it was the first) slides in. Both together take one and a half times what one step does. The bar stays still, and the
cursor is drawn once a console has landed. Everything is timed by the clock, so a slow terminal skips frames and the slide takes as long.

## Commands (after the colon)

| command | does |
|---|---|
| `new [name] [-- command ...]` | a new console running your shell (or the command), and go to it. `new "my name"` keeps spaces |
| `close`, `kill` | close this console (its program gets a hangup); closing the last one leaves vtmux |
| `next`, `prev`, `goto N` | switch, with the transition |
| `rename NAME` | name this console on the bar |
| `list` | show the consoles on the bar for a moment |
| `set transition slide\|dip\|dissolve\|crossfade` | how the switch looks: `slide` (the default) slides the screens along side by side, passing the ones between; `dip` fades the old screen to dark and the new one up; `dissolve` changes cell by cell; `crossfade` mixes the colours as vtshow does. (`set fade` still works) |
| `set transition-ms N` | how long one step takes, 0 to 5000 (default 1000; 0 switches at once). A slide over several consoles takes a little longer, up to twice as long |
| `set void-color RRGGBB\|off` | the colour of the empty space while a console is opening or closing (default `101220`; `off` leaves it the terminal's own background) |
| `set fps on\|off` | show the measured frame rate on the bar (off by default). `--fps N` sets the most frames a second (default 60) |
| `quit` | close everything and leave |
| `help` | a card over the middle of the screen (about two thirds of it) with all the keys and commands. The console keeps running round it. `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End` scroll it when the screen is too small to show it all; `Enter`, `Space`, `Esc` or `q` closes it |

A console whose program ends by itself (you type `exit`) closes. When the last one closes, vtmux ends and your terminal is put back as it
was.

## How it works

See [DESIGN.md](DESIGN.md). In short: each console is a program on a pseudo-terminal (`Session`) with a reader thread feeding an
`Emulator`; the UI thread paints only the rows that changed into a retained vtscreen `Picture` (`PanePainter`), blends two pictures
for a fade (`blend`), draws the bar, and shows the result. Normal-mode input bytes go straight to the program. Because the real terminal
must then send keys the way the program expects, vtmux puts it in the shown console's input modes (application cursor keys, bracketed
paste, mouse, focus) and takes it out of them again on the way out.

## What is not done yet

- Underline, italic, strikethrough and blink are not drawn, and wide characters (Chinese, emoji) show as `?`: vtscreen's cells do not
  have those yet (a later step is to give them to vtscreen).
- No detach and attach (there is no server), no splits, no copy mode (the history can be looked at, not selected from), no mouse commands, no config file.
- Not supported by the emulator: sixel, double height lines, the 132 column mode (the screen clears but does not resize), combining
  marks (they are dropped).

## Tests

- `tests/run_tests.sh` builds every unit test (`tests/*_tests.cpp`, each names its sources on a `// sources:` line) with the strict warning
  flags and the address and undefined behaviour sanitizers, and runs them: the parser, the emulator, the key parser, the painter, the
  fades, the sessions (real programs on real pseudo-terminals), and the app (the modes, commands and fades, driven by bytes and a made-up
  clock). `ctest --test-dir build` runs the same tests from the CMake build.
- `tests/e2e/run.sh` runs the real `vtmux` in a pseudo-terminal with real programs inside (`less`, `vi`, `top`, `htop`, Ctrl-C and
  Ctrl-Z, resizing, the mode mirroring, rapid switching, heavy output, idle CPU) and reads the screen back with a terminal emulator.
- `tests/conformance/run.sh` feeds thousands of generated escape-sequence streams, hand-written screens and real program captures to the
  emulator (`tools/vtdump.cpp`), tmux and pyte, and compares the screens. `tests/vt100_tests.cpp`, `vt102_tests.cpp` and `vt52_tests.cpp` pin
  the DEC terminals' behaviour. What it found is in `tests/conformance/FINDINGS.md`.
- `tools/demo.py` makes the screenshots above; `tools/preview_transition.py` renders the three fades.

## Licence

GNU General Public License, version 3 (see `LICENSE`). The `vtscreen` library it builds on is MIT licensed.
