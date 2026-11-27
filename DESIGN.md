# vtmux: design

A terminal multiplexer with sliding and fading transitions. You run `vtmux` in a console and see what looks like the same console, with one
extra row at the bottom: the control bar. Everything works as it did. Press `Ctrl-B` and the bar changes colour: you are in
*command mode*. There you make and close consoles by typing commands after a colon, or flip between consoles with the left
and right arrow keys, and the screens cross-fade into each other (as in vtshow), real terminal screens fading.

This is "terminal version 2". The old terminal in rasfled2 (`terminal.h`, `terminal.cpp`) is not touched; it is the feature
checklist (its VT100/VT102/VT52 quirks were hard won). The screen layer is vtscreen (a submodule), used with retained pictures.

## What it looks like

    normal mode        the active console fills the screen above a one row bar:
                       [1:bash]  2:vim  3:htop                              vtmux  ^B for commands
    command mode       the bar changes colour and says what keys do:
                       COMMAND  <- -> switch   : command   1-9 go to   Esc back          (or the line being typed: ":new")

Command mode keys:

| key | does |
|---|---|
| `Left`, `Right` | the previous, the next console, with the transition (wraps round: from the last, Right slides all the way back to the first) |
| `1` ... `9` | that console |
| `:` | type a command; Enter runs it, Esc gives up, Backspace edits |
| `Esc`, `Enter` | back to normal mode |
| `Ctrl-B` | send a real Ctrl-B to the program and go back to normal mode |

Commands (after `:`):

| command | does |
|---|---|
| `new [name] [-- command ...]` | a new console running the shell (or the command), and go to it |
| `close`, `kill` | close this console (its program gets SIGHUP); the last one closing ends vtmux |
| `next`, `prev`, `goto N` | switch, with the transition |
| `rename NAME` | name this console |
| `list` | show the consoles on the bar for a moment |
| `set transition slide\|dip\|dissolve\|crossfade`, `set transition-ms N`, `set void-color RRGGBB\|off`, `set fps on\|off`, `set clipboard on\|off` | how, and how long, the switch looks; the frame rate on the bar |
| `quit` | close everything and leave |
| `help` | a card over the middle of the screen (about 2/3, at most 90 wide) listing the keys and commands; the console runs on underneath; arrows scroll, Enter/Space/Esc/q close (mode `Help`) |

A console that ends by itself (the shell exits) closes. Closing the last one leaves vtmux.

## Parts

    keyboard --> Display (vtscreen, raw keys) --> App (modes, commands, windows) --> Session --> pty --> program
                                                    |                                  |
                                                    |                           reader thread --> Emulator (the screen in memory)
                                                    |                                  |
                                          bar, Transition <-- retained Picture <-- PanePainter (dirty rows only)
                                                    |
                                               Display::show --> the real terminal

| module | file | does | status |
|---|---|---|---|
| parser | `src/vtparser.*` | bytes to events (state machine, split-proof, UTF-8); the data of a Sixel string (`DCS q`) is collected, up to 32 MiB; other device control strings are skipped | done, tested |
| emulator | `src/emulator.*`, `src/width.*` | the terminal in memory: cells, cursor, scrollback, alt screen, modes, replies; decodes Sixel (vtscreen's `sixel_decode`) and places it | done, being completed (VT52, VT102 extras) |
| pictures | `src/pictures.h` | the picture layer: a tile (picture, piece) for each cell of both screens and each line of the scrollback, and the store of pictures with its budget | done, tested |
| keys | `src/keys.*` | raw input bytes to key events (arrows, F keys, Escape, paste) for command mode | done, tested |
| painter | `src/render.*` | emulator dirty rows to a retained vtscreen Picture (colours, bold, reverse, cursor); the tiles to one Raster placed on the Picture | done, tested |
| transition | `src/transition.*` | the blend of two pictures: crossfade, dissolve, dip to dark; and `compose`, which places screens on a surface for the slide (a row of consoles; opening: the old one slides off left, then the new one drops in from the top; closing: the old one drops off the bottom, then the neighbour slides in; the void between is a fill colour). Slide is the default; (dip: both screens stay readable; crossfade cuts at the half way point because text cannot mix; dissolve is noisy). The pictures made of pixels go along in every one | done, tested |
| session | `src/session.*` | forkpty, the reader thread, the lock, resize, exit (closes every inherited descriptor in the child) | done, tested |
| app | `src/app.*` | the modes, the command line, the window list, the bar, the transition in progress | done, tested |
| main | `src/main.cpp` | options, the loop, the terminal modes (resets the frame schedule every frame, so a burst of keys cannot push later frames away) | done, e2e tested |
| conformance | `tests/conformance/` | the emulator against tmux and pyte on the same bytes; vttest style checks | helper agent |

## Threads

- The UI thread runs the frame loop (60 fps at most, the clock drives the animation): keys, then for the shown console(s) `Session::publish` (copy what changed into
  the console's Picture), the transition, the bar, `Display::show`.
- Each Session has one reader thread: `read(pty)`, then under the session lock `Emulator::feed`, then the replies are written back
  to the pty (outside the lock).
- The lock is held only for a feed (microseconds) and for a publish (the changed rows). A console nobody is looking at is still
  fed (the program must not block) but not painted: it costs one feed per read, and nothing when idle.
- Resizing: the UI thread resizes every Session (rows - 1 by cols) with the lock held, then `TIOCSWINSZ` so the child gets SIGWINCH.

## Input

Normal mode passes the bytes straight to the active console's pty, untouched, so every program sees exactly the keys you typed.
The only byte watched is the prefix, Ctrl-B (0x02). Command mode parses the bytes into keys (`KeyParser`).

Because the bytes pass straight through, the real terminal must send keys the way the program inside expects (application cursor
keys, bracketed paste, mouse). So whenever the active console, or its modes, change, vtmux mirrors that console's input modes onto
the real terminal (DECCKM `?1`, keypad `=`/`>`, bracketed paste `?2004`, focus `?1004`, mouse `?1000/1002/1003/1006`) and undoes
them on exit. Raw mode: no ISIG (Ctrl-C, Ctrl-Z reach the program), no IXON, no ICRNL, no ECHO or line editing.

Children run with `TERM=xterm-256color`, `COLORTERM=truecolor`, `VTMUX=1`; the window size is the screen minus the bar.

## Drawing

Each Session owns a retained `vtscreen::Picture` (rows + 1 by cols; the last row is the bar's). `PanePainter::paint` writes only the
emulator's dirty rows into it with `set()`, and the cursor as an inverse cell. So an idle console costs nothing to draw, and the
Display's retained mode skips everything that did not change. The bar is written into the shown picture each frame (text cells).
A transition mixes two sessions' pictures into a third with `blend`, then the bar is written over the result.

Cell mapping (vtscreen text cells): the character; foreground r,g,b (the 256 palette resolved here, the default foreground a theme
colour); the background as the alt colour (none for the default background, so the terminal's own background shows); reverse is the
cell's `inv`; bold brightens palette colours 0 to 7 and is also the bold attribute; dim scales the colour; hidden is a space (with no
attributes); italic, underline, strikethrough and blink are cell attributes that vtscreen sends to the terminal. A wide character is a
`CELL_WIDE` cell and a `CELL_TAIL` cell (vtscreen draws the pair once and turns a torn pair into a blank, so slides and dissolves are safe).

## Pictures (Sixel)

vtmux asks the real terminal at start (vtpen: Sixel? the size of a cell?) and tells vtscreen (`set_image_support`) and every console (`Emulator::set_graphics`),
so that what programs ask is answered truly (DA1 names Sixel only if the terminal has it, `CSI 14/16/18 t`, XTSMGRAPHICS, XTVERSION, and the
pixel size in the pty's window size). Without Sixel outside, a picture is discarded and takes no room.

The decisions (2026-11-27): pictures stay with their lines in the scrollback, within a budget of memory (64 MiB for a console, 4 bytes a pixel); the cursor
ends below the picture, in the column it began (as xterm and the DEC terminals do); the decoder lives in vtscreen next to the encoder, and vtpen does not depend on vtscreen.

- **Parser.** `ESC P params q data ST` reaches the emulator as `VtParserEvents::dcs` (any other DCS is skipped as before). Data over 32 MiB drops the picture.
- **Decoder.** `vtscreen::sixel_decode(data, p2, Raster&, limits)`: 256 colour registers (the VT340's first 16), RGB and HLS, repeats, raster attributes
  (size and aspect), P2 (transparent or background), limits of 4096 by 4096. Nothing drawn: false.
- **Layer.** A picture is cut into tiles, one per cell (`Tile{image, col, row}`), in a second grid beside the cells (lazy: no memory until a picture is placed), one
  line of tiles for each line of scrollback (empty when it has none), in step with `scrollback_` through a single `push_history`. Every site in the emulator that writes or
  moves cells does the same to the tiles: a letter written over a cell clears its tile (both cells of a wide letter, and the other half of a broken one), erase clears,
  insert and delete characters shift them with the cells, scrolls move them (and into the history), the alternate screen has its own, resize cuts them, a full reset clears both screens,
  `ED 3` the history. A change of the cell size drops every picture (they were cut for the old cells).
- **Placing.** Row by row, going down as a line feed does (inside the scroll region, scrolling into the history), cut off at the right edge; then the cursor goes one more row down,
  in the column where the picture began. Mode 8452: no last line feed, the cursor stays on the last row, right of the picture. Mode 80 (DECSDM): from the top left, nothing scrolls,
  cut off at the bottom, the cursor stays. (Mode 80 is built from the DEC documentation; xterm's source was not checked.) A picture too big for the budget is not kept but its room is taken.
- **Budget.** The pictures are kept once however many tiles point at them. After one is added, when the total is over a quarter of the budget and has doubled since the last
  clean-up (or is over the budget), the pictures no tile points to are forgotten; then, if still over, the oldest, and their tiles are cleared.
- **Painter.** One `Raster` just big enough for the block of cells that have a tile, placed with `Picture::place_image`; a row of it is made again only when its tiles differ from
  what it shows (a picture number is never used again, so equal tiles mean equal pixels). A raster that a copy of the Picture also holds (kept for a transition) is copied before it
  is changed. The cursor is not drawn over pictures.
- **Transitions.** `compose` moves a layer's placements with it (whole cells; the Display cuts what is outside the body); `blend`: crossfade uses vtscreen's `fade` (the pixels dissolve), dissolve
  makes each cell's pixels follow its letter (needs the size of a cell), dip darkens them; sizes that differ lose them. `images_without` takes the box of the help card out of the placements.
- **Tests.** `tests/pictures_tests.cpp` runs the same random stream into an emulator with pictures and one with letters in their place and compares tile for letter (history included);
  `render_pictures_tests.cpp` compares the painter's pixels with the emulator's tiles, with fresh painters, copies and scrolled-back views; the transition and app tests use real shells; the
  end-to-end test sends a Sixel picture to a pretend terminal that can and one that cannot.

Not done: a stand-in made of letters for a terminal without Sixel, ReGIS, an answer to DECRQM for modes 80 and 8452, noticing a change of the font size while running.

## Not in the first version

Detach and attach (a server and clients), splits (panes within a screen; the design allows it: a Session is just a picture source),
copy mode (scrollback browsing is done: Up/Down/PageUp/PageDown/Home/End in command mode), mouse, a config file.

## Testing

- parser and emulator unit tests, under the sanitizers (`tests/run_tests.sh`); every test file names its sources on a
  `// sources:` line.
- conformance: the same byte streams into the emulator, `pyte` and `tmux`, and the screens compared (`tests/conformance/`).
- the session and app against real programs (`sh`, `cat`, `vi` if present) in a pseudo-terminal, reading the output with a
  terminal model (as vtscreen's tests do).
- previews of the fade: rendered to images from a pseudo-terminal, to judge how it looks.
