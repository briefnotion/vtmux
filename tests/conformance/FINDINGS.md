# Conformance findings

Written from the report of the conformance run (vtmux against tmux 3.4 and pyte 0.8.2; "xterm" is from memory of its source).
Run it again with `tests/conformance/run.sh` (about a minute; `VTCONF_COUNT`, `VTCONF_SEED`, `VTCONF_FUZZ`, `VTCONF_CAPTURE=full`).

Coverage: tens of thousands of random streams, 42 hand-written screens, 84 real-program captures (identical to tmux, history too),
split input (1 and 5 byte pieces) identical, 8000 fuzz streams with no crash or sanitizer report.

## Fixed after the run
- DL at the top row saved the deleted lines into the scrollback (it is a delete, not a scroll off).
- A scroll region whose top is the first row now keeps scrolled-off lines (tmux, xterm), bottom margin or not.
- `CSI ? 6 n` (DECXCPR) is answered: `CSI ? row ; col R`.
- SGR 38/48 with a number out of range is ignored (was clamped).
- VPR (`CSI n e`) stops at the bottom margin like CUD.

## Left as is (a choice, or the references disagree)
- Invalid UTF-8 gives fewer U+FFFD than WHATWG says; harmless.
- Scrolling moves the whole grid per line: 15 MB of coloured lines at 50x200 takes about 1.1 s at -O1 (could be a ring buffer later).
- Deferred wrap: the cursor stays on the last column with a flag (xterm), tmux and pyte keep it one past the edge. BS, CUB, CUF, CR, CUP
  and CPR are pinned in `vt100_tests.cpp`. LF/IND/RI/VPA/CUU/CUD clear the flag; EL/ED/ECH erase the last cell; REP repeats.
- IL/DL move to column 0 (pyte too; tmux leaves the column).
- DECSC/DECRC: one saved slot per screen (xterm). `?47h` keeps the alternate screen's old contents; `?1047h` clears on entry;
  `?1049l` without a `?1049h` does nothing.
- Wide characters: one that does not fit blanks the old last cell and wraps; overwriting half of one blanks the other half. Combining
  marks, ZWJ and VS16 are dropped by design.
- Scrolled-in lines are filled with the current background colour. RIS keeps the scrollback (tmux pushes the screen into history).

## tmux and pyte quirks met (no need to look again)
tmux: wrap model one past the edge; BS at column 0 backs over a wrapped line; ICH with a count above half the line clears fewer cells;
IL/DL act outside the region; DECSTBM in origin mode homes to (0,0); no CHT, HPR, VPR, LNM; REP capped at the line end; DECSTR keeps the
pen; `capture-pane -e` drops conceal and writes OSC 8 into the text; CAN does not end an OSC; insert mode plus wrap replaces; wide
characters in a 1 column screen; 1 row or 1 column screens behave oddly.
pyte: NEL does not return the carriage; DECRC pops a stack; the wrap survives CUU/CUD; erases and ICH/DCH/IL/DL do not fill with the
background; no REP, SU, SD, CHT, CBT, HPA, `CSI s/u`, alternate screen, history or DEC graphics; `CSI ! p` prints a p.

## What the suite cannot do
Replies (only the unit tests check them), titles, bell, mouse modes, resize (tmux reflows, vtmux crops; only fuzzed for invariants),
VT52 (neither reference has it). tmux needs the socket `-L vtmux_conf` free (`VTCONF_SOCKET` picks another).
