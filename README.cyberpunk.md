Language: [English](https://github.com/briefnotion/vtmux/blob/main/README.md), [Español](https://github.com/briefnotion/vtmux/blob/main/README.es.md), [Français](https://github.com/briefnotion/vtmux/blob/main/README.fr.md), [Deutsch](https://github.com/briefnotion/vtmux/blob/main/README.de.md), [日本語](https://github.com/briefnotion/vtmux/blob/main/README.ja.md), [中文](https://github.com/briefnotion/vtmux/blob/main/README.zh.md), [Corporate](https://github.com/briefnotion/vtmux/blob/main/README.corporate.md), [Cajun](https://github.com/briefnotion/vtmux/blob/main/README.cajun.md), [CyberPunk](https://github.com/briefnotion/vtmux/blob/main/README.cyberpunk.md)

# vtmux

A terminal multiplexer jacked straight into the neon grid, running sliding and fading transitions at the speed of thought. Spin up `vtmux` in a console and you get what looks like the same old console, with one extra row bolted on at the
bottom: the control bar, your cockpit. Everything still runs like it did. Slam `Ctrl-B` and the bar changes colour: you are in command mode, where you can
spawn and flatline consoles by typing a command after a colon, or ghost between consoles with the left and right arrow keys, while the
screens slide (or fade) into each other like rain on chrome.

![vtmux](docs/demo_sheet.png)

*vtmux running real programs, with its bar burning along the bottom. Top: the radar map from [vtweather](https://github.com/briefnotion/vtweather) in the
second console, with `Ctrl-B` pressed (the bar goes amber: command mode). Bottom left: the thunderstorm scene from vtshow, in the fourth console.
Bottom right: a chat program showing Chinese text (生日快乐, "happy birthday"), drawn two columns wide, straight from the megacity.*

It is built on [vtscreen](https://github.com/briefnotion/vtscreen), the same drawing library as vtshow and vtweather, with its
retained pictures: an idle console costs next to nothing to draw, no wasted watts. The terminal emulator inside (`src/emulator.*`) is brand new, custom-forged: a state
machine parser and a screen in memory, stress-tested against `tmux` and `pyte` in the back alleys of the net.

## Build and run (jack in)

Linux, `g++` with C++17, `cmake` 3.16 or later, `git`:

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux/build
    ./m
    ./vtmux

`./m` (in the `build` directory) forges vtmux, or reforges it after a change. It also pulls the `vtscreen` submodule if the clone was made
without it, and screams at you if `cmake` or a compiler is missing from your rig. `./m test` builds the tests too and runs them, and `./m clean` burns what
was built.

`./install` (also in `build`) runs `./m` and then plants `vtmux` in `~/.local/bin`, so you can type `vtmux` at any prompt (or in `/usr/local/bin`
if you run it as root). `./install --system` drops it into `/usr/local/bin` for every runner on the machine (it asks for sudo), `./install DIR` drops it in `DIR`, and
`./install --remove` wipes the trace. If the folder is not in your `PATH` it says so and shows the line to add.

The same thing as `./m` by hand: `cmake -B build && cmake --build build`. `-DVTMUX_TESTS=OFF` skips building the tests.

    vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]

With no command the first console runs your shell (`$SHELL`). Programs inside run with `TERM=xterm-256color`.

## Keys (the cyberdeck)

In normal mode every key goes to the program in the console, untouched (Ctrl-C and Ctrl-Z too). The one key the system watches is `Ctrl-B`.

| in command mode (after `Ctrl-B`) | does |
|---|---|
| `Left`, `Right` | the previous, the next console, with the transition (a slide along the row of consoles; from the last, `Right` slides all the way back to the first); it stays in command mode so you can keep flipping through the grid |
| `1` to `9` | that console, and back to normal mode |
| `c`, `n`, `p` | a new console, the next, the previous (as in tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | dive back through the console's history (2000 lines); the bar shows how deep you went. `End` returns to the live screen |
| `:` | type a command (Enter runs it, Esc gives up, Backspace edits, Ctrl-U clears) |
| `Esc`, `Enter` | back to normal mode (and to the live screen). Typing into the console does the same |
| `Ctrl-B` | send a real Ctrl-B to the program, and back to normal mode |

## Sliding (data streams)

The consoles sit side by side in a row, in the order on the bar. Switching slides the view along the row, so both screens show part of the way
and a jump over several consoles shows the ones between streaking past. A key pressed while a slide is moving starts the next one from where the
view is now. A new console (`new`) goes in next to the one shown, on its right, in two steps: first the old one slides off to the left, leaving empty space (the void, a dark blue by default, the dead zone between nodes), then the new one drops in from the top. Closing is two steps too: the closed console drops off the bottom, then the one to its left (or the next, if it was the first) slides in. Both together take one and a half times what one step does. The bar stays still, and the
cursor is drawn once a console has landed. Everything is timed by the clock, so a slow terminal skips frames and the slide takes as long.

## Copy and paste (data smuggling)

- In an ordinary console (a listing, a shell) select with the mouse as you always do: your terminal program does that, and vtmux is not in the way.
- A program that uses the mouse itself (micro, btop, vim with `mouse=a`) gets the mouse events instead, so dragging selects inside the program.
  To select with your terminal program's own selection, hold `Shift` while dragging (Windows Terminal, most Linux terminals).
- A program that copies by itself (micro with `"clipboard": "terminal"` in its settings, vim and tmux-style plugins, `printf` from a script)
  sends the text to the terminal in an escape sequence (OSC 52), and your terminal program puts it in the clipboard. vtmux passes these on from
  the console that is shown, so Ctrl-C in micro works as it does without vtmux. A console in the background does not get to set your
  clipboard, no rogue process hijacks your buffer. Only writes are passed: a program cannot read your clipboard through vtmux. `:set clipboard off` stops all of it (on by default).
  Your terminal program may have its own switch for this too.
- Paste goes through as a paste (bracketed, if the program asked for that).

## Commands (after the colon) (the black ICE)

| command | does |
|---|---|
| `new [name] [-- command ...]` | spawn a new console running your shell (or the command), and jump to it. `new "my name"` keeps spaces |
| `close`, `kill` | flatline this console (its program gets a hangup); closing the last one leaves vtmux |
| `next`, `prev`, `goto N` | switch, with the transition |
| `rename NAME` | tag this console on the bar |
| `list` | flash the consoles on the bar for a moment |
| `set transition slide\|dip\|dissolve\|crossfade` | how the switch looks: `slide` (the default) slides the screens along side by side, passing the ones between; `dip` fades the old screen to dark and the new one up; `dissolve` changes cell by cell; `crossfade` mixes the colours as vtshow does. (`set fade` still works) |
| `set transition-ms N` | how long one step takes, 0 to 5000 (default 1000; 0 switches at once, zero latency). A slide over several consoles takes a little longer, up to twice as long |
| `set void-color RRGGBB\|off` | the colour of the empty space while a console is opening or closing (default `101220`; `off` leaves it the terminal's own background) |
| `set clipboard on\|off` | whether programs may set your clipboard (OSC 52, see Copy and paste; default on) |
| `set fps on\|off` | show the measured frame rate on the bar (off by default). `--fps N` sets the most frames a second (default 60) |
| `quit` | close everything and disconnect |
| `help` | a card over the middle of the screen (about two thirds of it) with all the keys and commands. The console keeps running round it. `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End` scroll it when the screen is too small to show it all; `Enter`, `Space`, `Esc` or `q` closes it |

A console whose program ends by itself (you type `exit`) closes. When the last one closes, vtmux ends and your terminal is put back as it
was: clean exit, no trace left on the grid.

## How it works (under the chrome)

See [DESIGN.md](DESIGN.md). In short: each console is a program on a pseudo-terminal (`Session`) with a reader thread feeding an
`Emulator`; the UI thread paints only the rows that changed into a retained vtscreen `Picture` (`PanePainter`), blends two pictures
for a fade (`blend`), draws the bar, and shows the result. Normal-mode input bytes go straight to the program. Because the real terminal
must then send keys the way the program expects, vtmux puts it in the shown console's input modes (application cursor keys, bracketed
paste, mouse, focus) and takes it out of them again on the way out.

## What is not done yet (unfinished mods)

- Bold, italic, underline, strikethrough and blink are sent to your terminal as the usual codes, and wide characters (Chinese, emoji) take two
  columns. How they look, or whether they show at all (blink and italic often do not), is up to the terminal program. Combining marks are
  dropped, and a letter the terminal draws at a different width from the emulator's table can look odd.
- No detach and attach (there is no server), no splits, no copy mode (the history can be looked at, not selected from), no mouse commands, no config file.
- Not supported by the emulator: sixel, double width and double height lines (`ESC # 3` to `6`; they are ignored), combining marks (they
  are dropped). The 80/132 column switch (DECCOLM) works inside the pane: see below.

## Tests (stress-testing the rig)

- `tests/run_tests.sh` builds every unit test (`tests/*_tests.cpp`, each names its sources on a `// sources:` line) with the strict warning
  flags and the address and undefined behaviour sanitizers, and runs them: the parser, the emulator, the key parser, the painter, the
  fades, the sessions (real programs on real pseudo-terminals), and the app (the modes, commands and fades, driven by bytes and a made-up
  clock). `ctest --test-dir build` runs the same tests from the CMake build.
- `tests/e2e/run.sh` runs the real `vtmux` in a pseudo-terminal with real programs inside (`less`, `vi`, `top`, `htop`, Ctrl-C and
  Ctrl-Z, resizing, the mode mirroring, rapid switching, heavy output, idle CPU) and reads the screen back with a terminal emulator.
- `tests/conformance/run.sh` feeds thousands of generated escape-sequence streams, hand-written screens and real program captures to the
  emulator (`tools/vtdump.cpp`), tmux and pyte, and compares the screens. `tests/vt100_tests.cpp`, `vt102_tests.cpp` and `vt52_tests.cpp` pin
  the DEC terminals' behaviour. What it found is in `tests/conformance/FINDINGS.md`.
- To try the real `vttest` program (`sudo apt install vttest`) run it inside vtmux. vttest switches the screen to 80 (or 132) columns with
  DECCOLM after allowing it with mode 40, as xterm needs, and vtmux does that inside the pane: the screen becomes 80 columns wide at the left
  (132 if the pane is that wide; a narrower pane keeps all of its width), the rest of the pane stays blank, the program is told the new size
  (SIGWINCH), and the screen clears. A full reset (`reset`, `ESC c`), mode 40 turned off, or resizing the window brings back the whole pane.
  Without mode 40 the switch only clears the screen, so the `reset` command does not narrow anything. vttest's double width lines are not
  drawn double (see above).
- `tools/demo.py` makes a set of screenshots of vtmux from a pseudo-terminal; `tools/preview_transition.py` renders the three fades.

## Licence

GNU General Public License, version 3 (see `LICENSE`). The `vtscreen` library it builds on is MIT licensed. Stay sharp, choom.
