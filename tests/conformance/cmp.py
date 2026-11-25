#!/usr/bin/env python3
"""cmp.py ROWS COLS 'ESCAPED BYTES' [--attrs]: show what vtmux, tmux and pyte make of one stream.

The bytes are written as in C or Python: \\x1b[2J  \\r  \\n  \\t  \\\\ ; everything else is taken as UTF-8.
Example:  cmp.py 3 5 '\\x1b[3;1H<\\x1b[4habcdefgh'
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vtconf


def unescape(text):
    out = bytearray()
    i = 0
    simple = {"n": 10, "r": 13, "t": 9, "\\": 92, "a": 7, "b": 8, "f": 12, "v": 11, '"': 34, "'": 39, "0": 0}
    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text):
            n = text[i + 1]
            if n == "x" and re.match(r"[0-9a-fA-F]{2}", text[i + 2:i + 4]):
                out.append(int(text[i + 2:i + 4], 16)); i += 4; continue
            if n in simple:
                out.append(simple[n]); i += 2; continue
        out += ch.encode("utf8")
        i += 1
    return bytes(out)


def show(rows, cols, data, attrs=False, tmux=True, pyte=True):
    case = vtconf.Case("cmp", rows, cols, [data])
    vtconf.build_vtdump(verbose=False)
    res = [("vtmux", vtconf.run_vt(case, attrs=attrs))]
    if tmux: res.append(("tmux", vtconf.tmux().run_batch([case], attrs=attrs)[0]))
    if pyte: res.append(("pyte", vtconf.run_pyte(case, attrs=attrs)))
    for name, s in res:
        extra = " (wrap pending)" if s.pending_wrap else ""
        hist = "" if s.scrollback is None else " scrollback=%d" % s.scrollback
        print("%-5s cursor row %d col %d%s visible=%s%s%s" % (name, s.cy, s.cx, extra, s.visible, hist, "  ERROR " + s.error if s.error else ""))
        for r, line in enumerate(s.rows):
            print("   %2d |%s" % (r, line))
        if s.reply: print("   reply", s.reply)
        if attrs and s.cells is not None:
            for k in sorted(s.cells):
                print("   cell", k, s.cells[k])
    return res


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 3:
        print(__doc__)
        sys.exit(2)
    show(int(args[0]), int(args[1]), unescape(args[2]), attrs="--attrs" in sys.argv,
         tmux="--no-tmux" not in sys.argv, pyte="--no-pyte" not in sys.argv)
