"""vtconf: engines and comparison for the vtmux conformance suite.

Three engines turn a byte stream into a screen:
  vt    the program under test, tools/vtdump.cpp (vtmux::Emulator)
  tmux  tmux 3.x in a detached session on its own socket (-L vtmux_conf), the authority for text and cursor
  pyte  the Python pyte module, a second opinion (no alternate screen, no scrollback, fewer sequences)

A Screen is what is compared: rows of text, the cursor, whether it is visible, the alternate-screen flag and
(optionally) the per-cell attributes.  Everything here is 0-based.
"""
import atexit
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
WORK = os.environ.get("VTCONF_WORK", "/tmp/vtconf")
VTDUMP = os.path.join(WORK, "vtdump")
SOCKET = os.environ.get("VTCONF_SOCKET", "vtmux_conf")        # one run at a time per socket
TMUX_CONF = os.path.join(HERE, "tmux.conf")


# ------------------------------------------------------------------------------------------------ the data

class Screen:
    """What an engine says the screen looks like after a stream."""

    def __init__(self, rows, cols):
        self.nrows, self.ncols = rows, cols
        self.rows = []          # list of str (trailing spaces trimmed)
        self.cy = self.cx = 0
        self.visible = True
        self.alt = False
        self.pending_wrap = None   # tmux only: the cursor is past the last column (a deferred wrap)
        self.cells = None       # dict (r, c) -> (ch, fg, bg, attrs) for non-default cells, only when asked
        self.reply = ""
        self.scrollback = None
        self.error = None
        self.history = None     # lines that scrolled off (oldest first), only when asked for
        self.invariant = None   # vtmux only: what vtdump found broken in the screen (it must hold for any input)
        self.lastcol = None
        self.bg = None          # vtmux only (with trace): ranges of offsets with a background colour in the pen
        self.curs = None        # vtmux only (with trace): (row, col) of the cursor after each byte, offsets 0 to N
        self.outside = None     # vtmux only (with trace): ranges of offsets with the cursor outside a scroll region
        self.col0 = None        # vtmux only (with trace): ranges of offsets with the cursor in column 0 below the first row
        self.pending = None     # vtmux only (with trace): ranges of byte offsets at which the wrap was pending

    def cursor(self):
        return (self.cy, self.cx)


class Case:
    """One byte stream with a size.  tokens (when known) lets the minimiser take the stream apart."""

    def __init__(self, name, rows, cols, tokens, tags=(), source="gen", profile=""):
        self.name = name
        self.rows, self.cols = rows, cols
        self.tokens = list(tokens) if not isinstance(tokens, (bytes, bytearray)) else [bytes(tokens)]
        self.tags = set(tags)
        self.source = source
        self.profile = profile
        self.nsuffix = 0        # how many of the last tokens are a probe added by variant(): never cut off
        self.cut_at = None      # set by the runner: the stream was cut here (see conform.py, tainted)

    @property
    def data(self):
        return b"".join(self.tokens)

    def variant(self, suffix):
        c = Case(self.name + "+" + suffix.decode("latin1"), self.rows, self.cols, self.tokens + [suffix], self.tags, self.source, self.profile)
        c.nsuffix = self.nsuffix + 1
        return c

    def cut(self, offset, drop_suffix=False):
        """The same case with the stream cut short at a byte offset (the probe suffix is kept unless drop_suffix)."""
        body = b"".join(self.tokens[:len(self.tokens) - self.nsuffix])
        suffix = [] if drop_suffix else self.tokens[len(self.tokens) - self.nsuffix:]
        c = Case(self.name + "@%d" % offset, self.rows, self.cols, [body[:offset]] + suffix, self.tags, self.source, self.profile)
        c.nsuffix = 0 if drop_suffix else self.nsuffix
        c.cut_at = offset
        return c


def esc_literal(data):
    """A byte string as an escaped literal that can be pasted into a C++ or Python string."""
    out = []
    prev_hex = False
    for b in data:
        if b == 0x1b: out.append("\\x1b"); prev_hex = True
        elif b == 0x0a: out.append("\\n"); prev_hex = False
        elif b == 0x0d: out.append("\\r"); prev_hex = False
        elif b == 0x09: out.append("\\t"); prev_hex = False
        elif b == 0x5c: out.append("\\\\"); prev_hex = False
        elif b == 0x22: out.append('\\"'); prev_hex = False
        elif 0x20 <= b < 0x7f:
            c = chr(b)
            if prev_hex and c in "0123456789abcdefABCDEF":   # C++ would swallow it into the hex escape
                out.append('" "' + c)
            else:
                out.append(c)
            prev_hex = False
        else: out.append("\\x%02x" % b); prev_hex = True
    return "".join(out)


# ------------------------------------------------------------------------------------------------ build

def build_vtdump(verbose=True):
    os.makedirs(WORK, exist_ok=True)
    srcs = [os.path.join(ROOT, "tools", "vtdump.cpp")] + [os.path.join(ROOT, "src", f) for f in ("vtparser.cpp", "emulator.cpp", "width.cpp")]
    newest = max(os.path.getmtime(s) for s in srcs + [os.path.join(ROOT, "src", f) for f in os.listdir(os.path.join(ROOT, "src")) if f.endswith(".h")])
    if os.path.exists(VTDUMP) and os.path.getmtime(VTDUMP) >= newest:
        return True
    cmd = ["g++", "-std=c++17", "-O1", "-I" + ROOT, "-o", VTDUMP] + srcs
    if verbose:
        print("building vtdump ...", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        print(r.stderr)
        return False
    return True


# ------------------------------------------------------------------------------------------------ vtmux

def _parse_color(s):
    if s == "d": return None
    if s.startswith("i:"): return ("i", int(s[2:]))
    return ("r",) + tuple(int(x) for x in s[2:].split("."))


def run_vt(case, chunk=0, attrs=False, trace=False, history=False, timeout=20):
    path = os.path.join(WORK, "vt_%d_%s.bin" % (os.getpid(), threading_id()))
    with open(path, "wb") as f:
        f.write(case.data)
    cmd = [VTDUMP, str(case.rows), str(case.cols), path]
    if chunk: cmd += ["--chunk", str(chunk)]
    if attrs: cmd += ["--attrs"]
    if trace: cmd += ["--trace"]
    if history: cmd += ["--history"]
    s = Screen(case.rows, case.cols)
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        s.error = "timeout"
        return s
    finally:
        try: os.unlink(path)
        except OSError: pass
    if r.returncode != 0:
        s.error = "vtdump exit %d: %s" % (r.returncode, r.stderr.decode("utf8", "replace")[:200])
        return s
    lines = r.stdout.decode("utf8", "replace").split("\n")
    s.rows = lines[:case.rows]
    if attrs: s.cells = {}
    if history: s.history = []
    for ln in lines[case.rows:]:
        p = ln.split(" ")
        if p[0] == "cursor": s.cy, s.cx, s.visible = int(p[1]), int(p[2]), p[3] == "1"
        elif p[0] == "alt": s.alt = p[1] == "1"
        elif p[0] == "scrollback": s.scrollback = int(p[1])
        elif p[0] == "reply": s.reply = ln[6:]
        elif p[0] == "invariant": s.invariant = ln[10:]
        elif p[0] == "hist":
            s.history.append(ln[5:])
        elif p[0] == "curs":
            s.curs = [tuple(int(x) for x in t.split(":")) for t in p[1:] if t]
        elif p[0] in ("pending", "col0", "lastcol", "outside", "bg"):
            rngs = []
            for rg in p[1:]:
                if rg and rg != "-":
                    lo, hi = rg.split("-")
                    rngs.append((int(lo), int(hi)))
            if p[0] == "pending": s.pending = rngs
            elif p[0] == "col0": s.col0 = rngs
            elif p[0] == "outside": s.outside = rngs
            elif p[0] == "bg": s.bg = rngs
            else: s.lastcol = rngs
        elif p[0] == "cell" and attrs:
            r_, c_ = int(p[1]), int(p[2])
            ch = chr(int(p[3][2:], 16))
            fg = _parse_color(p[4][3:]); bg = _parse_color(p[5][3:])
            at = set() if p[6] == "attrs=-" else set(p[6][6:].split(","))
            if "tail" in at: continue
            at.discard("wide")
            s.cells[(r_, c_)] = (ch, fg, bg, frozenset(at))
    return s


def threading_id():
    import threading
    return str(threading.get_ident())


# ------------------------------------------------------------------------------------------------ pyte

def run_pyte(case, attrs=False):
    import pyte
    from pyte import graphics
    s = Screen(case.rows, case.cols)
    try:
        scr = pyte.Screen(case.cols, case.rows)
        st = pyte.ByteStream(scr)
        st.feed(case.data)
    except Exception as e:  # pyte has bugs: a crash is a result, not an error of ours
        s.error = "pyte crashed: %r" % (e,)
        return s
    for y in range(case.rows):
        line = scr.buffer[y]
        t = "".join(line[x].data for x in range(case.cols))
        s.rows.append(t.rstrip(" "))
    s.cy, s.cx = scr.cursor.y, scr.cursor.x
    if s.cx >= case.cols:
        s.pending_wrap = True
        s.cx = case.cols - 1
    s.visible = not scr.cursor.hidden
    if attrs:
        s.cells = {}
        for y in range(case.rows):
            for x in range(case.cols):
                ch = scr.buffer[y][x]
                if ch.data == "":
                    continue
                at = set()
                if ch.bold: at.add("bold")
                if ch.italics: at.add("italic")
                if ch.underscore: at.add("underline")
                if ch.strikethrough: at.add("strike")
                if ch.reverse: at.add("reverse")
                if ch.blink: at.add("blink")
                fg, bg = _pyte_color(ch.fg), _pyte_color(ch.bg)
                if ch.data == " " and fg is None and bg is None and not at:
                    continue
                s.cells[(y, x)] = (ch.data, fg, bg, frozenset(at))
    return s


_PYTE_NAMES = ["black", "red", "green", "brown", "blue", "magenta", "cyan", "white"]


def _pyte_color(c):
    if c == "default": return None
    if c in _PYTE_NAMES: return ("i", _PYTE_NAMES.index(c))
    if c.startswith("bright") and c[6:] in _PYTE_NAMES: return ("i", 8 + _PYTE_NAMES.index(c[6:]))
    if re.fullmatch(r"[0-9a-f]{6}", c): return ("x", c)      # a 256-colour index or truecolour: pyte keeps only the hex
    return ("?", c)


# ------------------------------------------------------------------------------------------------ tmux

class Tmux:
    """Runs many cases at once on one private tmux server and reads the screens back."""

    def __init__(self):
        self.started = False
        self.counter = 0
        atexit.register(self.stop)

    def _tmux(self, *args, timeout=60):
        return subprocess.run(["tmux", "-L", SOCKET, "-f", TMUX_CONF] + list(args), capture_output=True, timeout=timeout)

    def start(self):
        if self.started: return
        for _ in range(10):
            r = self._tmux("start-server", ";", "new-session", "-d", "-s", "keeper", "-x", "10", "-y", "3", "sleep 100000")
            if r.returncode == 0: break
            time.sleep(0.2)
        else:
            raise RuntimeError("cannot start tmux: " + r.stderr.decode())
        self.started = True

    def stop(self):
        if self.started:
            subprocess.run(["tmux", "-L", SOCKET, "kill-server"], capture_output=True)
            self.started = False

    def run_batch(self, cases, attrs=False, history=False):
        """cases: list of Case.  Returns a list of Screen, in order."""
        self.start()
        n = len(cases)
        tag = "p%db%d" % (os.getpid(), self.counter)
        self.counter += 1
        files = []
        cmd = []
        for i, c in enumerate(cases):
            path = os.path.join(WORK, "tm_%s_%d.bin" % (tag, i))
            with open(path, "wb") as f:
                f.write(c.data)
            files.append(path)
            if cmd: cmd.append(";")
            # stty raw: no output processing (the pty would turn LF into CR LF). The CAN + title sequence at the end
            # tells us that tmux has read everything before it, even when the stream ended in the middle of a sequence (BEL and ESC \\
            # end an unfinished OSC / DCS string, CAN a CSI).
            sh = "stty raw -echo; cat %s; printf '\\007\\033\\\\\\030\\033]2;DONE\\007'; exec sleep 300" % path
            cmd += ["new-session", "-d", "-s", "%s_%d" % (tag, i), "-x", str(c.cols), "-y", str(c.rows), sh]
        r = self._tmux(*cmd)
        if r.returncode != 0:
            raise RuntimeError("tmux new-session failed: " + r.stderr.decode())
        # wait for all of them
        deadline = time.time() + 60
        fmt = "#{session_name} #{pane_title}"
        while True:
            out = self._tmux("list-panes", "-a", "-F", fmt).stdout.decode("utf8", "replace")
            done = sum(1 for ln in out.split("\n") if ln.startswith(tag + "_") and ln.endswith(" DONE"))
            if done >= n: break
            if time.time() > deadline:
                raise RuntimeError("tmux did not finish %d/%d" % (done, n))
            time.sleep(0.02)
        # read back
        cmd = []
        for i in range(n):
            t = "%s_%d" % (tag, i)
            if cmd: cmd.append(";")
            cmd += ["display-message", "-p", "-t", t, "@@@ #{cursor_x} #{cursor_y} #{cursor_flag} #{alternate_on} #{pane_width} #{pane_height} #{history_size}",
                    ";", "capture-pane", "-p", "-e", "-N", "-t", t]
            if history:
                cmd += [";", "display-message", "-p", "@@H", ";", "capture-pane", "-p", "-e", "-N", "-S", "-", "-E", "-1", "-t", t]
        r = self._tmux(*cmd)
        text = r.stdout.decode("utf8", "replace")
        self._tmux(*sum([["kill-session", "-t", "%s_%d" % (tag, i), ";"] for i in range(n)], [])[:-1])
        for f in files:
            try: os.unlink(f)
            except OSError: pass
        chunks = text.split("@@@ ")[1:]
        if len(chunks) != n:
            raise RuntimeError("tmux gave %d screens for %d cases" % (len(chunks), n))
        res = []
        for c, ch in zip(cases, chunks):
            res.append(self._parse(c, ch, attrs, history))
        return res

    def _parse(self, case, chunk, attrs, history=False):
        head, _, body = chunk.partition("\n")
        s = Screen(case.rows, case.cols)
        cx, cy, cf, alt, w, h, hist = [int(x) for x in head.split()]
        s.alt = bool(alt); s.visible = bool(cf); s.cy = cy; s.scrollback = hist
        if cx >= case.cols:
            s.pending_wrap = True; cx = case.cols - 1
        else:
            s.pending_wrap = False
        s.cx = cx
        hist_text = None
        if history and "@@H\n" in body:
            body, _, hist_text = body.partition("@@H\n")
        lines = body.split("\n")[:case.rows]
        while len(lines) < case.rows: lines.append("")
        if (w, h) != (case.cols, case.rows):
            s.error = "tmux pane is %dx%d, wanted %dx%d" % (w, h, case.cols, case.rows)
        s.rows, cells = parse_tmux_e(lines, case.cols)
        if attrs: s.cells = cells
        if hist_text is not None:
            hl = hist_text.split("\n")
            if hl and hl[-1] == "": hl.pop()
            n = s.scrollback or 0
            s.history = parse_tmux_e(hl[:n], case.cols)[0] if n else []
        return s


DEC_GRAPHICS = {chr(0x5f + i): c for i, c in enumerate(" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·")}


def parse_tmux_e(lines, cols):
    """The output of capture-pane -p -e as (rows of text, cells): an SGR parser, SO/SI for the line drawing set,
    and a wcwidth-aware column counter."""
    from wcwidth import wcwidth
    cells = {}
    rows = []
    fg = bg = None                  # tmux does not repeat a state at the start of the next line: carry it
    at = set()
    graphics = False
    for r, ln in enumerate(lines):
        col = 0
        i = 0
        text = {}
        while i < len(ln):
            ch = ln[i]
            if ch == "\x1b" and ln[i + 1:i + 2] == "[":
                j = i + 2
                while j < len(ln) and not (0x40 <= ord(ln[j]) <= 0x7e): j += 1
                if ln[j:j + 1] == "m":
                    fg, bg, at = _apply_sgr(ln[i + 2:j], fg, bg, at)
                i = j + 1
                continue
            if ch == "\x1b" and ln[i + 1:i + 2] == "]":              # OSC 8 hyperlinks: tmux writes them back
                j = i + 2
                while j < len(ln) and ln[j] != "\x07" and not (ln[j] == "\x1b" and ln[j + 1:j + 2] == "\\"): j += 1
                i = j + (1 if ln[j:j + 1] == "\x07" else 2)
                continue
            if ch == "\x1b":
                i += 1
                if ln[i:i + 1] in "()": i += 2
                continue
            if ch == "\x0e": graphics = True; i += 1; continue
            if ch == "\x0f": graphics = False; i += 1; continue
            if graphics: ch = DEC_GRAPHICS.get(ch, ch)
            w = wcwidth(ch)
            if w < 0: w = 1
            if w == 0:
                i += 1
                continue
            text[col] = ch
            if not (ch == " " and fg is None and bg is None and not at):
                cells[(r, col)] = (ch, fg, bg, frozenset(at))
            col += w
            i += 1
        last = max(text) if text else -1
        out = []
        for c in range(last + 1):
            if c in text: out.append(text[c])
            elif not (c - 1 in text and wcwidth(text[c - 1]) == 2): out.append(" ")     # the tail of a wide character has no text
        rows.append("".join(out).rstrip(" "))
    return rows, cells


def _apply_sgr(s, fg, bg, at):
    at = set(at)
    ps = s.split(";") if s != "" else ["0"]
    i = 0
    names = {1: "bold", 2: "dim", 3: "italic", 4: "underline", 5: "blink", 7: "reverse", 8: "hidden", 9: "strike"}
    off = {22: ("bold", "dim"), 23: ("italic",), 24: ("underline",), 25: ("blink",), 27: ("reverse",), 28: ("hidden",), 29: ("strike",)}
    while i < len(ps):
        try:
            p = ps[i].split(":")
            n = int(p[0]) if p[0] != "" else 0
        except ValueError:
            i += 1
            continue
        if n == 0: fg = bg = None; at = set()
        elif n in names:
            if n == 4 and len(p) > 1 and p[1] == "0": at.discard("underline")
            else: at.add(names[n])
        elif n in off:
            for a in off[n]: at.discard(a)
        elif 30 <= n <= 37: fg = ("i", n - 30)
        elif 40 <= n <= 47: bg = ("i", n - 40)
        elif 90 <= n <= 97: fg = ("i", n - 90 + 8)
        elif 100 <= n <= 107: bg = ("i", n - 100 + 8)
        elif n == 39: fg = None
        elif n == 49: bg = None
        elif n in (38, 48):
            if len(p) > 1:
                sub = [int(x) for x in p[1:] if x != ""]
                kind = sub[0] if sub else 0
                vals = sub[1:]
            else:
                kind = int(ps[i + 1]) if i + 1 < len(ps) else 0
                if kind == 5: vals = [int(ps[i + 2])]; i += 2
                elif kind == 2: vals = [int(x) for x in ps[i + 2:i + 5]]; i += 4
                else: vals = []; i += 1
            col = None
            if kind == 5 and vals: col = ("i", vals[0])
            elif kind == 2 and len(vals) >= 3: col = ("r",) + tuple(vals[-3:])
            if n == 38: fg = col
            else: bg = col
        i += 1
    return fg, bg, at


# ------------------------------------------------------------------------------------------------ comparison

def compare(a, b, attrs=False, cursor=True, cursor_visible=True, text=True, ignore_attrs=(), skip_blank_cells=False, history=False):
    """Differences between two Screens as a list of short strings (empty when they agree)."""
    d = []
    if text:
        for r, (x, y) in enumerate(zip(a.rows, b.rows)):
            if x != y:
                d.append("row %d: %r != %r" % (r, x, y))
    if cursor and a.cursor() != b.cursor():
        d.append("cursor %s != %s" % (a.cursor(), b.cursor()))
    if cursor_visible and a.visible != b.visible:
        d.append("cursor visible %s != %s" % (a.visible, b.visible))
    if history and a.history is not None and b.history is not None and not a.alt and not b.alt:
        ha, hb = a.history, b.history
        # tmux drops a tenth of its history when it is full: compare what both still have (the end of the longer one)
        if len(ha) != len(hb) and max(len(ha), len(hb)) >= 1800:
            n = min(len(ha), len(hb))
            ha, hb = ha[len(ha) - n:], hb[len(hb) - n:]
    else:
        ha = hb = None
    if ha is not None and ha != hb:
        d.append("history (lines scrolled off the top): %d lines %r != %d lines %r" % (len(ha), ha[:3], len(hb), hb[:3]))
    if attrs and a.cells is not None and b.cells is not None:
        keys = set(a.cells) | set(b.cells)
        for k in sorted(keys):
            ca, cb = a.cells.get(k), b.cells.get(k)
            if skip_blank_cells and ((ca and ca[0] == " ") or (cb and cb[0] == " ")):
                if (ca is None or ca[0] == " ") and (cb is None or cb[0] == " "): continue
            if ca and ca[0] == " " and not (ca[3] & {"underline", "strike", "reverse"}): ca = (ca[0], None, ca[2], ca[3])    # the colour of a blank is not seen
            if cb and cb[0] == " " and not (cb[3] & {"underline", "strike", "reverse"}): cb = (cb[0], None, cb[2], cb[3])
            if ignore_attrs:
                ca = ca and (ca[0], ca[1], ca[2], ca[3] - set(ignore_attrs))
                cb = cb and (cb[0], cb[1], cb[2], cb[3] - set(ignore_attrs))
                if ca and ca[0] == " " and ca[1] is None and ca[2] is None and not ca[3]: ca = None
                if cb and cb[0] == " " and cb[1] is None and cb[2] is None and not cb[3]: cb = None
            if ca != cb:
                d.append("cell %s: %r != %r" % (k, ca, cb))
    return d


def pyte_view(screen):
    """A copy of a Screen with the colours written the way pyte writes them: the 16 standard ones by name (here: index),
    everything else (palette entries from 16 on, 24-bit) as a hex string."""
    from pyte import graphics
    if screen.cells is None: return screen
    v = Screen(screen.nrows, screen.ncols)
    v.__dict__.update(screen.__dict__)

    def conv(c):
        if c is None: return None
        if c[0] == "i" and c[1] >= 16: return ("x", graphics.FG_BG_256[c[1]])
        if c[0] == "r": return ("x", "%02x%02x%02x" % c[1:])
        return c
    v.cells = {k: (ch, conv(fg), conv(bg), at) for k, (ch, fg, bg, at) in screen.cells.items()}
    return v


_tmux = None
def tmux():
    global _tmux
    if _tmux is None: _tmux = Tmux()
    return _tmux
