Language: [English](https://github.com/briefnotion/vtmux/blob/main/README.md), [Español](https://github.com/briefnotion/vtmux/blob/main/README.es.md), [Français](https://github.com/briefnotion/vtmux/blob/main/README.fr.md), [Deutsch](https://github.com/briefnotion/vtmux/blob/main/README.de.md), [日本語](https://github.com/briefnotion/vtmux/blob/main/README.ja.md), [中文](https://github.com/briefnotion/vtmux/blob/main/README.zh.md), [Corporate](https://github.com/briefnotion/vtmux/blob/main/README.corporate.md), [Cajun](https://github.com/briefnotion/vtmux/blob/main/README.cajun.md), [CyberPunk](https://github.com/briefnotion/vtmux/blob/main/README.cyberpunk.md)

# vtmux

Awright, cher, gather 'round. Dis here's a terminal multiplexer wit' sliding and fading, mais yeah. Run `vtmux` in a console and what you get looks like da same ol' console, wit' just one extra row down at da
bottom: da control bar. Everyt'ing works like it did before, I garontee. Press `Ctrl-B` and da bar changes colour: now you in command mode, where you can
make and close consoles by typin' a command after a colon, or flip between consoles wit' da left and right arrow keys, and da
screens slide (or fade) into each other, smooth like a good roux.

![vtmux](docs/demo_sheet.png)

*vtmux runnin' real programs, wit' its bar along da bottom. Up top: da radar map from [vtweather](https://github.com/briefnotion/vtweather) in da
second console, wit' `Ctrl-B` pressed (da bar's amber, dat's command mode). Bottom left: da thunderstorm scene from vtshow, in da fourth console.
Bottom right: a chat program showin' Chinese text (生日快乐, "happy birt'day"), drawn two columns wide, cher.*

She's built on [vtscreen](https://github.com/briefnotion/vtscreen), da same drawin' library as vtshow and vtweather, wit' its
retained pictures: a console sittin' still costs next to nothin' to draw. Da terminal emulator inside (`src/emulator.*`) is brand new, a state
machine parser and a screen in memory dat's been tested against `tmux` and `pyte`, mais yeah.

## Build and run

Linux, `g++` wit' C++17, `cmake` 3.16 or later, `git`:

    git clone --recursive --shallow-submodules --depth 1 https://github.com/briefnotion/vtmux.git
    cd vtmux/build
    ./m
    ./vtmux

`./m` (in da `build` directory) builds vtmux, or rebuilds it after a change. It also fetches da `vtscreen` submodule if you made da clone
wit'out it, and tells you if `cmake` or a compiler's missin'. `./m test` builds da tests too and runs 'em, and `./m clean` removes what
was built.

`./install` (also in `build`) runs `./m` and den copies `vtmux` to `~/.local/bin`, so you can type `vtmux` at any prompt (or to `/usr/local/bin`
if you run it as root). `./install --system` puts it in `/usr/local/bin` for ever'body (it asks for sudo), `./install DIR` puts it in `DIR`, and
`./install --remove` takes it away again. If da folder ain't in your `PATH` it says so and shows you da line to add.

Da same t'ing as `./m` by hand: `cmake -B build && cmake --build build`. `-DVTMUX_TESTS=OFF` skips buildin' da tests.

    vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]

Wit' no command da first console runs your shell (`$SHELL`). Programs inside run wit' `TERM=xterm-256color`.

## Keys

In normal mode ever' key goes to da program in da console, untouched (Ctrl-C and Ctrl-Z too). Da one key bein' watched is `Ctrl-B`.

| in command mode (after `Ctrl-B`) | does |
|---|---|
| `Left`, `Right` | da previous, da next console, wit' da transition (a slide along da row of consoles; from da last, `Right` slides all da way back to da first); it stays in command mode so you can keep flippin' |
| `1` to `9` | dat console, and back to normal mode |
| `c`, `n`, `p` | a new console, da next, da previous (like in tmux) |
| `Up`, `Down`, `PageUp`, `PageDown`, `Home` | look back t'rough da console's history (2000 lines); da bar shows how far back. `End` returns to da live screen |
| `:` | type a command (Enter runs it, Esc gives up, Backspace edits, Ctrl-U clears) |
| `Esc`, `Enter` | back to normal mode (and to da live screen). Typin' into da console does da same |
| `Ctrl-B` | send a real Ctrl-B to da program, and back to normal mode |

## Sliding

Da consoles sit side by side in a row, in da order on da bar. Switchin' slides da view along da row, so both screens show part of da way
and a jump over several consoles shows da ones between goin' past. A key pressed while a slide's movin' starts da next one from where da
view is now. A new console (`new`) goes in next to da one shown, on its right, in two steps: first da old one slides off to da left, leavin' empty space (da void, a dark blue by default), den da new one drops in from da top. Closin' is two steps too: da closed console drops off da bottom, den da one to its left (or da next, if it was da first) slides in. Both together take one and a half times what one step does. Da bar stays still, and da
cursor's drawn once a console has landed. Ever'tin' is timed by da clock, so a slow terminal skips frames and da slide takes as long.

## Copy and paste

- In an ordinary console (a listin', a shell) select wit' da mouse like you always do: your terminal program does dat, and vtmux ain't in da way.
- A program dat uses da mouse itself (micro, btop, vim wit' `mouse=a`) gets da mouse events instead, so draggin' selects inside da program.
  To select wit' your terminal program's own selection, hold `Shift` while draggin' (Windows Terminal, most Linux terminals).
- A program dat copies by itself (micro wit' `"clipboard": "terminal"` in its settings, vim and tmux-style plugins, `printf` from a script)
  sends da text to da terminal in an escape sequence (OSC 52), and your terminal program puts it in da clipboard. vtmux passes dese on from
  da console dat's shown, so Ctrl-C in micro works like it does wit'out vtmux. A console in da background don't get to set your
  clipboard. Only writes are passed: a program can't read your clipboard t'rough vtmux. `:set clipboard off` stops all of it (on by default).
  Your terminal program might have its own switch for dis too.
- Paste goes t'rough as a paste (bracketed, if da program asked for dat).

## Commands (after da colon)

| command | does |
|---|---|
| `new [name] [-- command ...]` | a new console runnin' your shell (or da command), and go to it. `new "my name"` keeps spaces |
| `close`, `kill` | close dis console (its program gets a hangup); closin' da last one leaves vtmux |
| `next`, `prev`, `goto N` | switch, wit' da transition |
| `rename NAME` | name dis console on da bar |
| `list` | show da consoles on da bar for a moment |
| `set transition slide\|dip\|dissolve\|crossfade` | how da switch looks: `slide` (da default) slides da screens along side by side, passin' da ones between; `dip` fades da old screen to dark and da new one up; `dissolve` changes cell by cell; `crossfade` mixes da colours like vtshow does. (`set fade` still works) |
| `set transition-ms N` | how long one step takes, 0 to 5000 (default 1000; 0 switches at once). A slide over several consoles takes a little longer, up to twice as long |
| `set void-color RRGGBB\|off` | da colour of da empty space while a console's openin' or closin' (default `101220`; `off` leaves it da terminal's own background) |
| `set clipboard on\|off` | whether programs may set your clipboard (OSC 52, see Copy and paste; default on) |
| `set fps on\|off` | show da measured frame rate on da bar (off by default). `--fps N` sets da most frames a second (default 60) |
| `quit` | close ever'tin' and leave |
| `help` | a card over da middle of da screen (about two t'irds of it) wit' all da keys and commands. Da console keeps runnin' round it. `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End` scroll it when da screen's too small to show it all; `Enter`, `Space`, `Esc` or `q` closes it |

A console whose program ends by itself (you type `exit`) closes. When da last one closes, vtmux ends and your terminal's put back like it
was.

## How it works

See [DESIGN.md](DESIGN.md). In short: each console is a program on a pseudo-terminal (`Session`) wit' a reader t'read feedin' an
`Emulator`; da UI t'read paints only da rows dat changed into a retained vtscreen `Picture` (`PanePainter`), blends two pictures
for a fade (`blend`), draws da bar, and shows da result. Normal-mode input bytes go straight to da program. Because da real terminal
must den send keys da way da program expects, vtmux puts it in da shown console's input modes (application cursor keys, bracketed
paste, mouse, focus) and takes it out of dem again on da way out.

## What ain't done yet

- Bold, italic, underline, striket'rough and blink are sent to your terminal as da usual codes, and wide characters (Chinese, emoji) take two
  columns. How dey look, or whether dey show at all (blink and italic often don't), is up to da terminal program. Combinin' marks are
  dropped, and a letter da terminal draws at a different width from da emulator's table can look odd.
- No detach and attach (dere's no server), no splits, no copy mode (da history can be looked at, not selected from), no mouse commands, no config file.
- Not supported by da emulator: sixel, double width and double height lines (`ESC # 3` to `6`; dey're ignored), combinin' marks (dey
  are dropped). Da 80/132 column switch (DECCOLM) works inside da pane: see below.

## Tests

- `tests/run_tests.sh` builds ever' unit test (`tests/*_tests.cpp`, each names its sources on a `// sources:` line) wit' da strict warnin'
  flags and da address and undefined behaviour sanitizers, and runs 'em: da parser, da emulator, da key parser, da painter, da
  fades, da sessions (real programs on real pseudo-terminals), and da app (da modes, commands and fades, driven by bytes and a made-up
  clock). `ctest --test-dir build` runs da same tests from da CMake build.
- `tests/e2e/run.sh` runs da real `vtmux` in a pseudo-terminal wit' real programs inside (`less`, `vi`, `top`, `htop`, Ctrl-C and
  Ctrl-Z, resizin', da mode mirrorin', rapid switchin', heavy output, idle CPU) and reads da screen back wit' a terminal emulator.
- `tests/conformance/run.sh` feeds t'ousands of generated escape-sequence streams, hand-written screens and real program captures to da
  emulator (`tools/vtdump.cpp`), tmux and pyte, and compares da screens. `tests/vt100_tests.cpp`, `vt102_tests.cpp` and `vt52_tests.cpp` pin
  da DEC terminals' behaviour. What it found is in `tests/conformance/FINDINGS.md`.
- To try da real `vttest` program (`sudo apt install vttest`) run it inside vtmux. vttest switches da screen to 80 (or 132) columns wit'
  DECCOLM after allowin' it wit' mode 40, like xterm needs, and vtmux does dat inside da pane: da screen becomes 80 columns wide at da left
  (132 if da pane's dat wide; a narrower pane keeps all of its width), da rest of da pane stays blank, da program is told da new size
  (SIGWINCH), and da screen clears. A full reset (`reset`, `ESC c`), mode 40 turned off, or resizin' da window brings back da whole pane.
  Wit'out mode 40 da switch only clears da screen, so da `reset` command don't narrow not'in'. vttest's double width lines ain't
  drawn double (see above).
- `tools/demo.py` makes a set of screenshots of vtmux from a pseudo-terminal; `tools/preview_transition.py` renders da three fades.

## Licence

GNU General Public License, version 3 (see `LICENSE`). Da `vtscreen` library it builds on is MIT licensed. Now go on, cher, make us proud.
