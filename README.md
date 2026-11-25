# vtmux

A terminal multiplexer with fades. Run `vtmux` in a console and you get what looks like the same console, with one extra row at the
bottom: the control bar. Everything works as it did. Press `Ctrl-B` and the bar changes colour: you are in command mode, where you can
make and close consoles by typing a command after a colon, or flip between consoles with the left and right arrow keys, and the
screens fade into each other.

![vtmux](docs/demo_sheet.png)

*Top left: a console and the blue bar. Top right: after `Ctrl-B` the bar turns amber. Bottom left: halfway through a fade between two
consoles. Bottom right: the second console.*

It is built on [vtscreen](https://github.com/briefnotion/vtscreen), the same drawing library as vtshow and vtweather, with its
retained pictures: an idle console costs next to nothing to draw. The terminal emulator inside (`src/emulator.*`) is new, a state
machine parser and a screen in memory that is tested against `tmux` and `pyte`.

## Build and run

Linux, `g++` with C++17, `cmake` 3.16 or later, `git`:

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux
    cmake -B build && cmake --build build -j4
    ./build/vtmux

If `vtscreen/` is empty, run `git submodule update --init`. `-DVTMUX_TESTS=OFF` skips building the tests.

    vtmux [--fade crossfade|dissolve|dip] [--fade-ms N] [--no-color] [--fps N] [-- command args ...]

With no command the first console runs your shell (`$SHELL`). Programs inside run with `TERM=xterm-256color`.

## Keys

In normal mode every key goes to the program in the console, untouched (Ctrl-C and Ctrl-Z too). The one key watched is `Ctrl-B`.

| in command mode (after `Ctrl-B`) | does |
|---|---|
| `Left`, `Right` | the previous, the next console, with a fade; it stays in command mode so you can keep flipping |
| `1` to `9` | that console, and back to normal mode |
| `c`, `n`, `p` | a new console, the next, the previous (as in tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | look back through the console's history (2000 lines); the bar shows how far back. `End` returns to the live screen |
| `:` | type a command (Enter runs it, Esc gives up, Backspace edits, Ctrl-U clears) |
| `Esc`, `Enter` | back to normal mode (and to the live screen). Typing into the console does the same |
| `Ctrl-B` | send a real Ctrl-B to the program, and back to normal mode |

## Commands (after the colon)

| command | does |
|---|---|
| `new [name] [-- command ...]` | a new console running your shell (or the command), and go to it. `new "my name"` keeps spaces |
| `close`, `kill` | close this console (its program gets a hangup); closing the last one leaves vtmux |
| `next`, `prev`, `goto N` | switch, with the fade |
| `rename NAME` | name this console on the bar |
| `list` | show the consoles on the bar for a moment |
| `set fade crossfade\|dissolve\|dip` | how the switch fades: `dip` (the default) fades the old screen to dark and the new one up; `dissolve` changes cell by cell; `crossfade` mixes the colours as vtshow does |
| `set fade-ms N` | how long it takes, 0 to 5000 (default 250; 0 switches at once) |
| `quit` | close everything and leave |
| `help` | the commands on the bar |

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
- `tools/demo.py` makes the screenshots above; `tools/preview_transition.py` renders the three fades.

## Licence

Not decided yet.
