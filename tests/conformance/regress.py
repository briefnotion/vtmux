"""Hand-written streams: vttest style screens and the minimal streams of the bugs found.  cases() returns Case objects.

Each stream is compared with tmux and pyte like the random ones (conform.py), so it must be something both can follow:
the DEC and xterm sequences the emulator is meant to handle, written as a program would write them.
"""
from vtconf import Case

ESC = "\x1b"
CSI = ESC + "["


def cup(r, c): return "%s%d;%dH" % (CSI, r, c)


def cases():
    out = []

    def add(name, rows, cols, data, tags=()):
        if isinstance(data, str): data = data.encode("utf8")
        out.append(Case("reg-" + name, rows, cols, [data], set(tags), "regress", "regress"))

    # ---------------------------------------------------------------- vttest menu 1, cursor movements
    for rows, cols in ((24, 80), (10, 20), (5, 9)):
        s = ESC + "#8"                                                # a screen of E
        s += cup(2, 2) + CSI + "%dX" % (cols - 2)                     # hollow out rows 2 .. rows-1 with ECH
        for r in range(2, rows): s += cup(r, 2) + CSI + "%dX" % (cols - 2)
        s += cup(1, 1) + "+"                                          # the four corners, reached by moves that go too far
        s += CSI + "999C" + "+" + CSI + "999B" + "+" + CSI + "999D" + "+"
        s += CSI + "999A" + CSI + "2B" + CSI + "2C" + "*" + CSI + "3D" + CSI + "1A" + "#"
        add("cursor-frame-%dx%d" % (rows, cols), rows, cols, s)

    for rows, cols in ((24, 80), (6, 12)):                            # the wrap at the right margin, printing and tabbing
        s = cup(1, 1)
        for r in range(rows):
            s += "".join(chr(ord("a") + (r + c) % 26) for c in range(cols + 3)) + "\r\n"
        add("autowrap-%dx%d" % (rows, cols), rows, cols, s)
        s = CSI + "?7l" + cup(2, 1) + "x" * (cols + 5) + CSI + "?7h" + cup(4, 1) + "y" * (cols + 2) + "z"
        add("nowrap-%dx%d" % (rows, cols), rows, cols, s)

    s = cup(1, 1) + CSI + "3g"                                        # clear all tab stops, then set one every 5th column
    for c in range(1, 60, 5): s += cup(1, c) + ESC + "H"
    s += cup(2, 1) + "\t1\t2\t3\t4\t5\t6\t7\t8\t9"
    s += cup(3, 1) + CSI + "0g"                                       # clear the stop under the cursor (none at col 1: no change)
    s += cup(3, 6) + CSI + "g" + cup(3, 1) + "\ta\tb\tc"
    add("tabs-hts-tbc", 6, 70, s)
    add("tabs-default", 4, 60, "\t1\t2\t3\t4\t5\t6\t7\t8\r\n    \t|\r\n" + "x" * 8 + "\ty")

    # ---------------------------------------------------------------- scroll regions
    def numbered(rows): return "".join(cup(r, 1) + "line %02d" % r for r in range(1, rows + 1))
    for rows, cols in ((12, 20), (24, 80)):
        top, bot = 3, rows - 2
        s = numbered(rows) + CSI + "%d;%dr" % (top, bot)
        s += cup(bot, 1) + "\n\n" + "after-lf"                         # LF at the bottom margin scrolls only the region
        s += cup(top, 1) + ESC + "M" + ESC + "M" + "after-ri"         # RI at the top margin scrolls it down
        s += cup(rows, 1) + "\n" + "below"                            # LF below the region at the bottom of the screen: no scroll
        s += cup(1, 1) + ESC + "M" + "above"                          # RI above the region at the top of the screen: stays
        s += cup(bot + 1, 5) + ESC + "D" + "ind-below"
        add("region-scroll-%dx%d" % (rows, cols), rows, cols, s)
        s = numbered(rows) + CSI + "?6h" + CSI + "%d;%dr" % (top, bot) + "origin" + cup(1, 1) + "A" + cup(99, 99) + "Z" + CSI + "99A" + "T" + CSI + "99B" + "B"
        s += CSI + "6n"                                               # (the answer is not compared: only the cursor and screen are)
        s += CSI + "?6l" + cup(1, 1) + "free"
        add("region-origin-%dx%d" % (rows, cols), rows, cols, s)
        s = numbered(rows) + CSI + "%d;%dr" % (top, bot) + cup(top + 1, 1) + CSI + "2L" + "il" + cup(bot, 1) + CSI + "3M" + "dl"
        s += cup(1, 1) + CSI + "M" + cup(rows, 1) + CSI + "L"          # outside the region: nothing
        s += CSI + "2S" + CSI + "1T"                                  # scroll up and down inside it
        add("region-il-dl-su-sd-%dx%d" % (rows, cols), rows, cols, s)

    # ---------------------------------------------------------------- screen features
    s = numbered(8) + cup(4, 5) + CSI + "K" + cup(5, 5) + CSI + "1K" + cup(6, 5) + CSI + "2K"
    add("el-forms", 8, 20, s)
    s = numbered(8) + cup(3, 4) + CSI + "J" + cup(7, 4) + CSI + "1J"
    add("ed-forms", 8, 20, s)
    s = numbered(8) + cup(4, 4) + CSI + "2J"
    add("ed-2", 8, 20, s)
    s = "0123456789" * 3 + cup(1, 5) + CSI + "5@" + cup(1, 25) + CSI + "5P" + cup(2, 1) + "abcdefghij" + cup(2, 3) + CSI + "3X"
    add("ich-dch-ech", 4, 30, s)
    s = CSI + "4h" + "world" + cup(1, 1) + "hello " + CSI + "4l" + "XX"
    add("insert-mode", 3, 30, s)
    s = ESC + "#8" + cup(5, 5) + CSI + "1;7;31m" + "colour" + CSI + "0m" + ESC + "#8" + CSI + "2J" + "after-decaln"
    add("decaln-then-clear", 8, 20, s)

    # ---------------------------------------------------------------- character sets
    s = ESC + "(0" + "".join(chr(c) for c in range(0x5f, 0x7f)) + ESC + "(B\r\n"
    s += ESC + "(0" + "lqqqqqqqqk\r\nx        x\r\nmqqqqqqqqj" + ESC + "(B\r\ndone"
    add("graphics-set", 6, 40, s)
    s = ESC + ")0" + "ab\x0eqx\x0fab" + ESC + "(0" + "\x0eqx\x0f" + "qx" + ESC + "(B" + "qx"
    add("graphics-so-si", 3, 40, s)
    s = ESC + "(0" + "q" + ESC + "7" + ESC + "(B" + "q" + ESC + "8" + "q"
    add("graphics-decsc", 3, 20, s)

    # ---------------------------------------------------------------- colours and attributes
    s = ""
    for i in range(8): s += CSI + "%dm" % (30 + i) + "fg%d " % i + CSI + "0m"
    s += "\r\n"
    for i in range(8): s += CSI + "%dm" % (40 + i) + "bg%d " % i + CSI + "0m"
    s += "\r\n"
    for i in range(8): s += CSI + "%dm" % (90 + i) + "hi%d " % i + CSI + "0m"
    s += "\r\n"
    for a in (1, 2, 3, 4, 5, 7, 8, 9): s += CSI + "%dm" % a + "a%d" % a + CSI + "0m "
    s += "\r\n" + CSI + "38;5;196mred" + CSI + "48;5;21mblue-bg" + CSI + "0m " + CSI + "38;2;10;200;30;48;2;200;10;10mtrue" + CSI + "0m"
    s += "\r\n" + CSI + "1;4;7;31;42mall" + CSI + "22;24;27mpart" + CSI + "39;49mdone" + CSI + "m"
    add("sgr-sample", 8, 70, s)
    s = CSI + "44m" + CSI + "2J" + cup(3, 5) + "on-blue" + CSI + "K" + cup(5, 1) + CSI + "3L" + cup(6, 1) + CSI + "2M" + CSI + "0m" + cup(8, 1) + "tail"
    add("bce-erase", 10, 30, s)
    s = CSI + "42m" + "\r\n" * 12 + "x"                                # scrolling with a background
    add("bce-scroll", 6, 20, s)

    # ---------------------------------------------------------------- wide characters, UTF-8
    add("wide-edge", 4, 10, "abcdefghi中\r\n中中中中中中\r\n" + cup(3, 10) + "中" + cup(4, 1) + "x" + CSI + "4X" + "ab")
    add("wide-overwrite", 3, 10, "中中中" + cup(1, 2) + "x" + cup(1, 5) + "yz")
    add("wide-edit", 3, 12, "a中b中c" + cup(1, 2) + CSI + "2P" + cup(2, 1) + "a中b中c" + cup(2, 3) + CSI + "2@")
    add("utf8-mixed", 3, 20, "é ñ ü ß € → ┌─┐ 日本語 ░▒▓")
    add("box-drawing", 5, 20, "┌──────┐\r\n│ box  │\r\n├──────┤\r\n│      │\r\n└──────┘")

    # ---------------------------------------------------------------- alternate screen, reset
    add("alt-1049", 5, 20, "main text\r\nline2" + CSI + "?1049h" + "alt text" + cup(3, 3) + "zz" + CSI + "?1049l" + "!")
    add("alt-1049-twice", 5, 20, "main" + CSI + "?1049h" + "alt1" + CSI + "?1049l" + CSI + "?1049h" + "alt2" + CSI + "?1049l")
    add("ris", 5, 20, "text" + CSI + "3;4r" + CSI + "?6h" + CSI + "1;31m" + ESC + "c" + "after")
    add("cursor-hide", 3, 10, "ab" + CSI + "?25l" + "c")
    add("save-restore", 6, 20, cup(2, 3) + CSI + "1m" + ESC + "7" + cup(5, 5) + CSI + "0m" + "x" + ESC + "8" + "y" + CSI + "s" + cup(1, 1) + CSI + "u" + "z")

    # ---------------------------------------------------------------- what real programs do
    add("less-like", 6, 30, CSI + "?1049h" + CSI + "H" + CSI + "2J" + "line one\r\nline two\r\nline three\r\n" + cup(6, 1) + CSI + "7m" + ":" + CSI + "m" + CSI + "K")
    add("prompt-colours", 4, 40, CSI + "?2004h" + CSI + "1;32muser@host" + CSI + "0m:" + CSI + "1;34m~/src" + CSI + "0m$ ls\r\n" + CSI + "0m" + CSI + "01;34mdir" + CSI + "0m  file\r\n")
    add("progress-bar", 3, 30, "".join("\r[" + "#" * i + " " * (20 - i) + "] %3d%%" % (i * 5) for i in range(0, 21)))
    add("backspace-erase", 3, 20, "abc\b \bd\x08\x08X")
    add("ctrl-chars", 3, 20, "a\x00b\x07c\x7fd\x0be\x0cf")
    return out
