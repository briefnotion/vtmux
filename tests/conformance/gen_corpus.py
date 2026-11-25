"""gen_corpus: byte streams for the conformance suite.

  random_cases(count, seed)  streams from a grammar of terminal features, over many screen sizes
  regress_cases()            hand-written streams (regress.py): vttest style screens and bugs found earlier
  captured_cases(dir)        output captured from real programs run in a pty of a known size (capture.py)

Every Case carries tokens (one escape sequence or text run each) so the minimiser can take a failing stream apart,
and tags (the features it used) so the runner can leave out a reference that does not support one of them.
"""
import os
import random

from vtconf import Case

SIZES = [  # (rows, cols, weight)
    (24, 80, 8), (10, 40, 6), (5, 20, 6), (6, 12, 6), (3, 7, 5), (1, 10, 3), (10, 1, 3), (1, 1, 1), (2, 2, 3), (8, 3, 3),
    (2, 30, 3), (30, 120, 2), (4, 200, 1), (12, 17, 4), (7, 9, 4), (1, 2, 1), (2, 1, 1), (50, 10, 1), (24, 132, 1),
]

# profile -> {group: weight}.  Text and control characters are in every profile.
BASE = {"text": 12, "ctl": 5}
PROFILES = {
    "basic":   dict(BASE, cursor=10, erase=4, wide=1, box=1),
    "edit":    dict(BASE, cursor=6, erase=4, ichdch=5, ildl=5, rep=1),
    "region":  dict(BASE, cursor=5, erase=2, ildl=3, decstbm=5, sudu=3, ind=4, origin=3, decsc=1),
    "modes":   dict(BASE, cursor=5, erase=2, nowrap=3, insert=3, lnm=2, ichdch=2, decsc=1, wide=1),
    "tabs":    dict(BASE, cursor=3, erase=1, hts=5, tbc=3, chtcbt=4, nowrap=1),
    "sgr":     dict(BASE, cursor=4, erase=4, sgr=10, ichdch=2, ildl=2, sudu=2, decstbm=1),
    "sgr256":  dict(BASE, cursor=3, erase=3, sgr=4, sgr256=6, sgrtrue=4, ildl=2),
    "charset": dict(BASE, cursor=4, charset=8, decsc=3, ind=1, erase=1),
    "save":    dict(BASE, cursor=6, decsc=6, origin=2, decstbm=2, ind=2, charset=1, sgr=1, nowrap=1),
    "alt":     dict(BASE, cursor=4, erase=2, alt=4, decsc=2, decstbm=1, sgr=1),
    "unicode": dict(BASE, cursor=4, erase=2, wide=8, box=3, utf8=4, ichdch=2, nowrap=1, insert=1, combining=1),
    "reset":   dict(BASE, cursor=4, erase=1, ris=2, decaln=2, decstbm=2, origin=1, sgr=1, tabs=0, hts=1, charset=1, cursorvis=2),
    "strings": dict(BASE, cursor=4, erase=1, strings=6, sgr=1),
    "mixed":   dict(BASE, cursor=8, erase=3, ichdch=2, ildl=2, rep=1, decstbm=2, sudu=1, ind=2, origin=1, nowrap=1, insert=1,
                    lnm=0.5, hts=1, tbc=1, chtcbt=1, sgr=3, sgr256=1, sgrtrue=1, charset=1, decsc=2, wide=1, box=1, utf8=1,
                    alt=0.5, cursorvis=0.5, decaln=0.3, strings=0.5, combining=0.3),
}

WORDS = ["alpha", "bravo", "x", "hello world", "0123456789", "A", "the quick brown fox", "---", "==", "tab\there", "~", "ok"]
WIDE = ["中", "文", "日本語", "한글", "ＡＢ", "あ", "漢字"]
BOX = ["─", "│", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼", "█", "▒", "░"]
UTF8 = ["é", "ñ", "ü", "ß", "£", "€", "©", "Ω", "я", "→", "°"]


def csi(params, final, private=""):
    return ("\x1b[" + private + ";".join("" if p is None else str(p) for p in params) + final).encode()


class Gen:
    def __init__(self, rng, rows, cols, weights):
        self.rng, self.rows, self.cols = rng, rows, cols
        self.weights = weights
        self.counter = rng.randrange(94)
        # one family per stream.  47 and 1047 are entered once only: xterm keeps what the alternate screen held (47), tmux clears it
        self.alt_mode = rng.choice([1049, 1049, 1049, 47, 1047])
        self.in_alt = False
        self.alt_used = False
        self.tokens = []
        self.tags = set()

    # ---- parameters
    def num(self, hi=None):
        rng = self.rng
        hi = hi or max(self.rows, self.cols)
        r = rng.random()
        if r < 0.12: return None
        if r < 0.20: return 0
        if r < 0.62: return rng.randint(1, 3)
        if r < 0.90: return rng.randint(1, max(1, hi))
        return rng.choice([hi + 1, hi + 5, 99, 999, 9999])

    def row(self): return self.num(self.rows)
    def col(self): return self.num(self.cols)

    # ---- text that makes every cell tell its own story (so a shifted row is seen)
    def cycle(self, n):
        s = ""
        for _ in range(n):
            s += chr(33 + (self.counter % 94))
            self.counter += 1
        return s

    def fill(self):
        """The whole screen written with changing characters, no scrolling."""
        toks = [b"\x1b[H"]
        for r in range(self.rows):
            toks.append(csi([r + 1, 1], "H"))
            toks.append(self.cycle(self.cols).encode())
        toks.append(csi([self.row() or 1, self.col() or 1], "H"))
        return toks

    # ---- the groups: each returns a list of byte tokens
    def g_text(self):
        r = self.rng.random()
        if r < 0.35: return [self.cycle(self.rng.randint(1, 6)).encode()]
        if r < 0.55: return [self.cycle(self.rng.randint(self.cols - 2 if self.cols > 3 else 1, self.cols + 3)).encode()]   # to the right edge and over
        return [self.rng.choice(WORDS).replace("\t", " ").encode()]

    def g_ctl(self):
        return [self.rng.choice([b"\r", b"\n", b"\r\n", b"\b", b"\t", b"\v", b"\f", b"\n", b"\r\n", b"\b\b", b"\t\t", b"\x07", b"\x00"])]

    def g_cursor(self):
        rng = self.rng
        k = rng.choice("ABCDEFGHfd`aesu" + "HHAABBCCDD")
        if k in "su" and self.alt_used: k = "A"
        if k in "HfH": return [csi([self.row(), self.col()], "H" if k != "f" else "f")] if rng.random() < 0.8 else [csi([self.row()], "H")]
        if k in "su":
            if self.alt_used: k = "A"          # DECSC is per screen in xterm (and tmux has one slot): not compared across an alternate screen
            else: self.tags.add("scp"); return [csi([], k)]
        if k == "`": self.tags.add("hpa"); return [csi([self.col()], k)]
        if k == "a": self.tags.add("hpr"); return [csi([self.col()], k)]
        if k == "e": self.tags.add("vpr"); return [csi([self.row()], "e")]
        if k == "d": return [csi([self.row()], "d")]
        if k == "G": return [csi([self.col()], "G")]
        return [csi([self.num(max(self.rows, self.cols))], k)]

    def g_erase(self):
        rng = self.rng
        k = rng.choice("JKXJK")
        if k == "X": return [csi([self.num(self.cols)], "X")]
        return [csi([rng.choice([None, 0, 1, 2, 0, 1, 2, 3 if k == "J" and rng.random() < 0.1 else 0])], k)]

    def g_ichdch(self):
        k = self.rng.choice("@P")
        return [csi([self.num(self.cols)], k)]

    def g_ildl(self):
        # IL and DL: ECMA-48 and the VT102 put the cursor in column 1; tmux and (I believe) xterm leave it.  The CR makes the
        # two readings agree so this does not hide other differences (see FINDINGS.md, "IL/DL and the column").
        k = self.rng.choice("LM")
        return [csi([self.num(self.rows)], k) + b"\r"]       # one token: the minimiser must not take the CR away

    def g_rep(self):
        self.tags.add("rep")
        return [self.cycle(1).encode(), csi([self.num(self.cols + 3)], "b")]

    def g_sudu(self):
        self.tags.add("sudu")
        return [csi([self.num(self.rows)], self.rng.choice("ST"))]

    def g_decstbm(self):
        rng = self.rng
        if rng.random() < 0.15: return [csi([], "r")]
        t = rng.randint(1, self.rows + 1)
        b = rng.randint(t, self.rows + 1) if rng.random() < 0.85 else rng.randint(1, self.rows + 1)
        return [csi([t, b], "r")]

    def g_ind(self):
        return [self.rng.choice([b"\x1bD", b"\x1bE", b"\x1bM", b"\x1bM", b"\x1bD"])]

    def g_origin(self):
        return [csi([6], self.rng.choice("hl"), "?")]

    def g_nowrap(self):
        return [csi([7], self.rng.choice("hl"), "?")]

    def g_insert(self):
        return [csi([4], self.rng.choice("hl"))]

    def g_lnm(self):
        self.tags.add("lnm")
        self.tags.add("lnm")
        return [csi([20], self.rng.choice("hl"))]

    def g_hts(self):
        return [self.rng.choice([b"\x1bH", csi([self.col()], "G") + b"\x1bH"])]

    def g_tbc(self):
        return [csi([self.rng.choice([None, 0, 0, 3])], "g")]

    def g_chtcbt(self):
        k = self.rng.choice("IZ")
        self.tags.add("cht" if k == "I" else "cbt")
        return [csi([self.num(4)], k)]

    def g_sgr(self):
        rng = self.rng
        pool = [0, 1, 2, 3, 4, 5, 7, 8, 9, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 46, 47, 49,
                90, 91, 97, 100, 101, 107]
        n = rng.randint(0, 4)
        return [csi([rng.choice(pool) for _ in range(n)], "m")]

    def g_sgr256(self):
        rng = self.rng
        k = rng.choice([38, 48])
        return [csi([k, 5, rng.choice([0, 1, 7, 8, 15, 16, 21, 46, 100, 196, 231, 232, 255, rng.randint(0, 255)])], "m")]

    def g_sgrtrue(self):
        rng = self.rng
        k = rng.choice([38, 48])
        return [csi([k, 2, rng.randint(0, 255), rng.choice([0, 128, 255]), rng.randint(0, 255)], "m")]

    def g_charset(self):
        rng = self.rng
        k = rng.random()
        self.tags.add("graphics")
        if k < 0.25: return [b"\x1b(0"]
        if k < 0.4: return [b"\x1b(B"]
        if k < 0.5: return [b"\x1b)0"]
        if k < 0.6: return [b"\x1b)B"]
        if k < 0.7: return [b"\x0e"]
        if k < 0.8: return [b"\x0f"]
        return ["".join(rng.choice("lqkxmjtuvwna`~_0abcfgh") for _ in range(rng.randint(1, 8))).encode()]

    def g_decsc(self):
        if self.alt_used: return []
        return [self.rng.choice([b"\x1b7", b"\x1b8", b"\x1b7", b"\x1b8"])]

    def g_wide(self):
        if self.cols < 2: return []                   # tmux draws a wide character in a one column screen
        return [self.rng.choice(WIDE).encode()]

    def g_box(self):
        return [("".join(self.rng.choice(BOX) for _ in range(self.rng.randint(1, 6)))).encode()]

    def g_utf8(self):
        return [self.rng.choice(UTF8).encode()]

    def g_alt(self):
        rng = self.rng
        self.tags.add("alt")
        m = self.alt_mode
        self.tags.add("alt%d" % m)
        if self.alt_mode != 1049 and self.alt_used and not self.in_alt: return []
        self.alt_used = True
        self.in_alt = not self.in_alt                # only a real switch: leaving without having entered is a different matter
        return [csi([m], "h" if self.in_alt else "l", "?")]

    def g_cursorvis(self):
        return [csi([25], self.rng.choice("hl"), "?")]

    def g_softreset(self):
        return [csi([], "p", "")[:-1] + b"!p"]

    def g_strings(self):
        """OSC, DCS, APC, PM: strings the emulator must swallow whole (and not print)."""
        rng = self.rng
        body = rng.choice(["hello", "a;b;c", "x" * rng.randint(1, 40), "title \u00e9", ""])
        kind = rng.choice(["osc0", "osc2", "osc8", "dcs", "apc", "pm", "osc-st"])        # (CAN inside an OSC string ends it in xterm, not in tmux: regress.py)
        if kind == "osc0": return [("\x1b]0;%s\x07" % body).encode()]
        if kind == "osc2": return [("\x1b]2;%s\x1b\\" % body).encode()]
        if kind == "osc8": return [("\x1b]8;;http://example.com/%s\x07" % body.replace(" ", "")).encode()]
        if kind == "dcs": return [("\x1bP1$r%s\x1b\\" % body).encode()]
        if kind == "apc": return [("\x1b_G%s\x1b\\" % body).encode()]
        if kind == "pm": return [("\x1b^%s\x1b\\" % body).encode()]
        if kind == "osc-st": return [("\x1b]52;c;%s\x1b\\" % "aGVsbG8=").encode()]
        return [("\x1b]0;%s\x18" % body).encode() + b"cancelled"]            # CAN ends the string and is not part of the text

    def g_combining(self):
        # (not ZWJ or VS16: tmux gives those a width)
        return [self.rng.choice(["e\u0301", "a\u0308", "n\u0303", "\u0301", "o\u0302\u0323"]).encode()]

    def g_ris(self):
        return [b"\x1bc"]

    def g_decaln(self):
        return [b"\x1b#8"]

    # ---- a whole stream
    def stream(self, length):
        rng = self.rng
        if rng.random() < 0.7: self.tokens += self.fill()
        names = list(self.weights)
        wts = [self.weights[n] for n in names]
        for _ in range(length):
            g = rng.choices(names, wts)[0]
            toks = getattr(self, "g_" + g)()
            self.tokens += toks
            self.tags.add(g)
        return self.tokens


def pick_size(rng):
    total = sum(w for _, _, w in SIZES)
    x = rng.uniform(0, total)
    for r, c, w in SIZES:
        x -= w
        if x <= 0: return r, c
    return SIZES[0][:2]


def random_cases(count, seed=1, profiles=None):
    rng = random.Random(seed)
    names = profiles or list(PROFILES)
    out = []
    for i in range(count):
        prof = names[i % len(names)]
        rows, cols = pick_size(rng)
        g = Gen(random.Random(rng.random()), rows, cols, PROFILES[prof])
        length = rng.choice([10, 20, 40, 80, 150])
        toks = g.stream(length)
        out.append(Case("rnd-%s-%dx%d-%d" % (prof, rows, cols, i), rows, cols, toks, g.tags, "random", prof))
    return out


def regress_cases():
    import regress
    return regress.cases()


def captured_cases(directory):
    """capture.py writes NAME.ROWSxCOLS.bin files; each is one case."""
    out = []
    if not os.path.isdir(directory): return out
    for fn in sorted(os.listdir(directory)):
        if not fn.endswith(".bin"): continue
        parts = fn[:-4].rsplit(".", 1)
        try:
            rows, cols = (int(x) for x in parts[1].split("x"))
        except (IndexError, ValueError):
            continue
        with open(os.path.join(directory, fn), "rb") as f:
            data = f.read()
        out.append(Case("cap-" + parts[0] + "-%dx%d" % (rows, cols), rows, cols, [data], {"captured"}, "captured"))
    return out


if __name__ == "__main__":
    import sys
    from vtconf import esc_literal
    for c in random_cases(int(sys.argv[1]) if len(sys.argv) > 1 else 5):
        print(c.name, sorted(c.tags))
        print("   ", esc_literal(c.data)[:300])
