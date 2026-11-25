#!/usr/bin/env python3
"""conform.py: run byte streams through vtmux (tools/vtdump), tmux and pyte, compare, classify, summarise.

  conform.py [--count N] [--seed S] [--profiles a,b] [--only random,regress,captured] [--no-pyte] [--no-tmux]
             [--attrs] [--minimize K] [--json FILE] [--verbose]

Classes of a difference (text rows, cursor position, cursor visibility):
  BUG-HI   vtmux differs from tmux and pyte says what tmux says           (two references agree: very likely ours)
  REVIEW   vtmux differs from tmux; pyte is silent (it does not model this) or differs from both   (tmux is the authority)
  QUIRK    vtmux differs from tmux but agrees with pyte                   (probably a tmux quirk: look at it by hand)
  HISTORY  only the lines that scrolled off the top differ (tmux is the only reference that keeps them)
  SPLIT    vtmux gives a different screen when the same bytes arrive in pieces        (ours, no reference needed)
  CRASH    vtdump failed (exit status, sanitizer, timeout)
  KNOWN    the stream uses something listed in KNOWN_LIMITS (documented as not done): shown, not counted as a bug
Streams are run twice: as is, and with a probe character at the end ("Z"), which makes the pending-wrap state of the
cursor visible (tmux reports it but the emulator's public API does not).

A stream is cut short (and the probe left off) at the first operation the references are known to do differently from xterm
(first_tainted: operations with the deferred wrap pending, BS in column 0, a wide character in the last column, IL/DL outside
the scroll region, ICH over half the line, printing at a pending wrap with a background colour).  What follows the cut is
checked only by the unit tests (tests/vt100_tests.cpp ...).  tools/vtdump.cpp --trace is what tells where those moments are.
The exit status is 1 for BUG-HI, HISTORY, SPLIT and CRASH; REVIEW and QUIRK are printed for reading (FINDINGS.md lists what
they were).
"""
import argparse
import json
import os
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vtconf
import wcwidth
from vtconf import Case, compare, esc_literal
import gen_corpus

# Features the emulator documents as not done: differences caused only by these are KNOWN, not bugs.
KNOWN_LIMITS = "combining marks, joiners and variation selectors are dropped (emulator.h): class KNOWN (see classify)"
# Features a reference is not trusted for (tags of a Case).  pyte has no alternate screen and lacks several sequences.
PYTE_UNSUPPORTED = {"alt", "sudu", "rep", "cht", "cbt", "scp", "hpa", "hpr", "vpr", "wide", "utf8", "graphics"}
# tmux has no LNM (CSI 20 h), no HPR (CSI a), no VPR (CSI e), no CHT (CSI I); it caps REP at the end of the line
TMUX_UNSUPPORTED = {"lnm", "hpr", "vpr", "cht", "rep"}


# What may follow a deferred wrap without tmux (and pyte) disagreeing with xterm about the state: tmux keeps the cursor
# one column past the edge (column == width) where xterm keeps it ON the last column with a flag, so every operation
# that looks at the cursor column or the flag differs (BS, HT, EL, ED, ECH, ICH, DCH, CUB, CUU, CUD, LF ...).
# Only printing, CR, CUP/HVP/CHA and SGR are safe.  A stream is compared up to the first unsafe operation done while the
# wrap is pending; the semantics after it are pinned by tests/vt100_tests.cpp instead.
SAFE_AFTER_PENDING = re.compile(rb"\x1b\[[0-9;]*[mHfG]")


def first_tainted(data, ranges, col0=None, lastcol=None, outside=None, bg=None, curs=None, cols=0, attrs=False):
    """Offset of the first byte at which an operation is done that tmux (or pyte) does differently from xterm:
    anything but printing, CR, CUP and SGR while the wrap is pending, BS in column 0 (tmux backs over a wrapped line), and a
    wide character that starts in the last column (it does not fit: tmux leaves the old cell, the others blank it)."""
    found = []
    pend = set()
    for lo, hi in ranges or []:
        for p in range(lo, min(hi, len(data) - 1) + 1):
            pend.add(p)
            b = data[p]
            if b == 0x1b:
                if not SAFE_AFTER_PENDING.match(data, p):
                    found.append(p); break
            elif b < 0x20 and b != 0x0d:
                found.append(p); break
    for lo, hi in col0 or []:
        for p in range(lo, min(hi, len(data) - 1) + 1):
            if data[p] == 0x08: found.append(p); break
    for lo, hi in lastcol or []:
        for p in range(lo, min(hi, len(data) - 1) + 1):
            if p in pend or data[p] < 0xE1: continue
            ch = data[p:p + 4].decode("utf8", "ignore")[:1]
            if ch and wcwidth.wcwidth(ch) == 2: found.append(p); break
    # ICH with a count over half of what is left of the line: tmux clears only (columns - col - n) cells and leaves old text
    if curs:
        for m in re.finditer(rb"\x1b\[([0-9]*)@", data):
            n = int(m.group(1) or b"1") or 1
            col = curs[m.start()][1]
            if n > cols - col - n and col < cols: found.append(m.start()); break
    # a printable character at the deferred wrap with a background colour: the line it scrolls in is filled with the
    # background by xterm and by us, with the default by tmux (its wrap path passes the default background)
    if attrs:
        for lo, hi in ranges or []:
            for p in range(lo, min(hi, len(data) - 1) + 1):
                if data[p] >= 0x20 and any(a <= p <= b for a, b in (bg or [])): found.append(p); break
    # IL and DL with the cursor outside the scroll region: xterm (and the VT102) do nothing, tmux deletes or inserts anyway
    for lo, hi in outside or []:
        for p in range(lo, min(hi, len(data) - 1) + 1):
            if data[p] == 0x1b and re.match(rb"\x1b\[[0-9;]*[LM]", data[p:p + 12]): found.append(p); break
    return min(found) if found else None


def detect_tags(data):
    """Features used by a byte stream, found in the bytes themselves (so a minimised stream has only its own)."""
    t = set()
    if re.search(rb"\x1b\[[0-9]*S", data) or re.search(rb"\x1b\[[0-9]*T", data): t.add("sudu")
    if re.search(rb"\x1b\[[0-9]*b", data): t.add("rep")
    if re.search(rb"\x1b\[[0-9]*I", data): t.add("cht")
    if re.search(rb"\x1b\[[0-9]*Z", data): t.add("cbt")
    if re.search(rb"\x1b\[\?(47|1047|1048|1049)[hl]", data): t.add("alt")
    if re.search(rb"\x1b\[[0-9]*`", data): t.add("hpa")
    if re.search(rb"\x1b\[[0-9]*a", data): t.add("hpr")
    if re.search(rb"\x1b\[[0-9]*e", data): t.add("vpr")
    if re.search(rb"\x1b\[[0-9]*[su]", data): t.add("scp")
    if re.search(rb"\x1b\[(?:[0-9;]*;)?20[hl]", data): t.add("lnm")
    if re.search(rb"\x1b[()]0|[\x0e\x0f]", data): t.add("graphics")
    if re.search(rb"\x1b\[[23]J|\x1bc|\x1b#8", data): t.add("clearscreen")
    if re.search(rb"\x1b\[[0-9]*;?[0-9]*r", data) and re.search(rb"\x1b\[[0-9]+;[0-9]+r", data): t.add("region")
    if re.search(rb"[\x80-\xff]", data):
        t.add("utf8")
        if any(wcwidth.wcwidth(ch) == 0 and ch >= " " for ch in data.decode("utf8", "ignore") if ord(ch) > 0x7f): t.add("zerowidth")
        try:
            if any(wcwidth.wcwidth(ch) == 2 for ch in data.decode("utf8", "ignore")): t.add("wide")
        except Exception:
            pass
    return t


def evaluate(cases, use_tmux=True, use_pyte=True, attrs=False, jobs=8):
    """-> list of dict(case, eff, vt, tmux, pyte, chunk) in order.  eff is the case that was really run (cut short at
    the first operation the references are not trusted for, see first_tainted)."""
    results = [dict(case=c) for c in cases]
    with ThreadPoolExecutor(jobs) as ex:
        traces = list(ex.map(lambda c: vtconf.run_vt(c, trace=True), cases))
        effs = []
        histok = []
        for c, t in zip(cases, traces):
            histok.append(not (t.curs and any(t.curs[m.start()] == (0, 0) for m in re.finditer(rb"\x1b\[0?J", c.data) if m.start() < len(t.curs))))
            cut = None if t.error else first_tainted(c.data, t.pending, t.col0, t.lastcol, t.outside, t.bg, t.curs, c.cols, attrs)
            if cut is None: effs.append(c)
            else: effs.append(c.cut(cut, drop_suffix=True))      # cut short: the probe would only repeat the plain case, in a state nobody checked
        vts = list(ex.map(lambda c: vtconf.run_vt(c, attrs=attrs, history=True), effs))
        chunk1 = list(ex.map(lambda c: vtconf.run_vt(c, chunk=1), effs))
        chunk5 = list(ex.map(lambda c: vtconf.run_vt(c, chunk=5), effs))
    for r, e, v, c1, c5, h in zip(results, effs, vts, chunk1, chunk5, histok):
        r["eff"], r["vt"], r["chunk"], r["histok"] = e, v, [], h
        for name, other in (("1", c1), ("5", c5)):
            if v.error or other.error: continue
            if v.rows != other.rows or v.cursor() != other.cursor() or v.visible != other.visible or v.alt != other.alt:
                r["chunk"].append(name)
    if use_tmux:
        tm = vtconf.tmux()
        B = 40
        for i in range(0, len(effs), B):
            for r, s in zip(results[i:i + B], tm.run_batch(effs[i:i + B], attrs=attrs, history=True)):
                r["tmux"] = s
    if use_pyte:
        for r, c in zip(results, effs):
            r["pyte"] = vtconf.run_pyte(c, attrs=attrs)
    return results


def strip_zero_width(screen):
    """A copy of a Screen with the zero-width characters (combining marks, joiners, variation selectors) left out of its rows:
    the emulator keeps one character per cell and drops them (documented in emulator.h), the references keep them."""
    v = vtconf.Screen(screen.nrows, screen.ncols)
    v.__dict__.update(screen.__dict__)
    v.rows = ["".join(ch for ch in row if wcwidth.wcwidth(ch) != 0) for row in screen.rows]
    if screen.cells is not None:
        v.cells = {k: c for k, c in screen.cells.items() if wcwidth.wcwidth(c[0]) != 0}
    if screen.history is not None:
        v.history = ["".join(ch for ch in row if wcwidth.wcwidth(ch) != 0) for row in screen.history]
    return v


def classify(r, attrs=False):
    """-> (class, [differences])"""
    c, vt = r["eff"], r["vt"]
    tags = detect_tags(c.data)
    if vt.error: return "CRASH", [vt.error]
    if vt.invariant: return "CRASH", ["invariant broken:" + vt.invariant]
    if r.get("chunk"): return "SPLIT", ["chunk sizes %s differ from one piece" % ",".join(r["chunk"])]
    tm, py = r.get("tmux"), r.get("pyte")
    tm_ok = tm is not None and not tm.error and not (tags & TMUX_UNSUPPORTED)
    py_ok = py is not None and not py.error and not (tags & PYTE_UNSUPPORTED)
    # tmux also keeps what a region at the top scrolls out, has no LNM, and moves the screen into its history when it clears it
    history = r.get("histok", True) and not (tags & {"alt", "region", "lnm", "clearscreen"})
    known = False
    if "zerowidth" in tags:
        # what a stream with combining marks shows is judged without them, and counted as known when that is all that differs
        plain = compare(vt, tm, attrs=attrs, ignore_attrs={"hidden"}, history=history) if tm_ok else None
        if tm_ok: tm = strip_zero_width(tm)
        if py_ok: py = strip_zero_width(py)
        known = bool(plain)
    d_tm = compare(vt, tm, attrs=attrs, ignore_attrs={"hidden"}, history=history) if tm_ok else None    # tmux does not give conceal back
    # pyte does not erase with the background, nor keep a history
    d_py = compare(vtconf.pyte_view(vt), py, attrs=attrs, ignore_attrs={"hidden", "dim"}, skip_blank_cells=True) if py_ok else None
    if tm_ok and d_tm and all(x.startswith("history") for x in d_tm):
        return "HISTORY", d_tm                       # only what scrolled off differs: pyte has no history to vote with
    if tm_ok:
        if not d_tm: return ("KNOWN", ["combining marks, joiners and variation selectors are dropped (emulator.h)"]) if known else ("OK", [])
        if d_py is None: return "REVIEW", d_tm
        if not d_py: return "QUIRK", d_tm
        # vtmux differs from both: does pyte say what tmux says?
        if not compare(py, tm, attrs=False): return "BUG-HI", d_tm
        return "REVIEW", d_tm
    if py_ok:
        return ("OK", []) if not d_py else ("REVIEW", d_py)
    return "OK", []


# ------------------------------------------------------------------------------------------------ the minimiser

def minimize(case, differs_batch, rounds=60):
    """ddmin on the tokens: differs_batch(list of Case) -> list of bool (still shows the difference)."""
    toks = list(case.tokens)
    suffix = []
    if toks and case.name.endswith("+Z"):
        suffix = [toks[-1]]; toks = toks[:-1]

    def mk(ts):
        return Case(case.name, case.rows, case.cols, ts + suffix, case.tags, case.source, case.profile)

    n = 2
    while len(toks) >= 2 and rounds > 0:
        rounds -= 1
        chunk = max(1, len(toks) // n)
        subsets = [toks[i:i + chunk] for i in range(0, len(toks), chunk)]
        complements = [[t for j, s in enumerate(subsets) if j != i for t in s] for i in range(len(subsets))]
        flags = differs_batch([mk(x) for x in subsets] + [mk(x) for x in complements])
        k = len(subsets)
        hit = None
        for i in range(k):
            if flags[i]: hit = (subsets[i], 2); break
        if hit is None:
            for i in range(k):
                if flags[k + i]: hit = (complements[i], max(n - 1, 2)); break
        if hit:
            toks, n = hit
        elif n < len(toks):
            n = min(len(toks), n * 2)
        else:
            break
    # a single token left or still several: try dropping single tokens, then trimming text tokens
    changed = True
    while changed and len(toks) > 1:
        changed = False
        cands = [toks[:i] + toks[i + 1:] for i in range(len(toks))]
        flags = differs_batch([mk(x) for x in cands])
        for i, f in enumerate(flags):
            if f: toks = cands[i]; changed = True; break
    for i in range(len(toks)):                      # shorten text tokens
        t = toks[i]
        if t.startswith(b"\x1b") or len(t) < 2: continue
        try: txt = t.decode("utf8")
        except UnicodeDecodeError: continue
        while len(txt) > 1:
            cands = [txt[:len(txt) // 2], txt[len(txt) // 2:], txt[:-1], txt[1:]]
            cands = [x.encode() for x in cands]
            flags = differs_batch([mk(toks[:i] + [x] + toks[i + 1:]) for x in cands])
            nxt = next((x for x, f in zip(cands, flags) if f), None)
            if nxt is None: break
            t = nxt
            txt = t.decode("utf8")
        toks[i] = t
    return mk(toks)


def make_differs(use_tmux, use_pyte, attrs):
    def differs_batch(cands):
        res = evaluate(cands, use_tmux=use_tmux, use_pyte=use_pyte, attrs=attrs)
        out = []
        for r in res:
            cls, _ = classify(r, attrs)
            out.append(cls not in ("OK",))
        return out
    return differs_batch


# ------------------------------------------------------------------------------------------------ main

def build_cases(args):
    cases = []
    only = set(args.only.split(","))
    if "regress" in only: cases += gen_corpus.regress_cases()
    if "captured" in only: cases += gen_corpus.captured_cases(os.path.join(vtconf.WORK, "captured"))
    if "random" in only:
        cases += gen_corpus.random_cases(args.count, args.seed, args.profiles.split(",") if args.profiles else None)
    return cases


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=600)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--profiles", default="")
    ap.add_argument("--only", default="random,regress,captured")
    ap.add_argument("--no-pyte", action="store_true")
    ap.add_argument("--no-tmux", action="store_true")
    ap.add_argument("--attrs", action="store_true", help="also compare cell colours and attributes")
    ap.add_argument("--minimize", type=int, default=0, help="minimise at most K failing streams (distinct by first difference)")
    ap.add_argument("--json", default="")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--show", type=int, default=8, help="how many differences to print per class")
    args = ap.parse_args()

    if not vtconf.build_vtdump(): return 2
    os.makedirs(vtconf.WORK, exist_ok=True)
    t0 = time.time()
    base = build_cases(args)
    cases = []
    for c in base:
        cases.append(c)
        cases.append(c.variant(b"Z"))
    print("%d streams, %d runs (each also with a probe character at the end)" % (len(base), len(cases)), flush=True)
    results = evaluate(cases, not args.no_tmux, not args.no_pyte, args.attrs)
    classes = {}
    for r in results:
        cls, diffs = classify(r, args.attrs)
        r["cls"], r["diffs"] = cls, diffs
        classes.setdefault(cls, []).append(r)
    print("ran in %.1fs" % (time.time() - t0))

    # minimise a sample of failures: one per distinct first difference
    findings = []
    if args.minimize:
        seen = set()
        differs = make_differs(not args.no_tmux, not args.no_pyte, args.attrs)
        for cls in ("CRASH", "SPLIT", "BUG-HI", "REVIEW", "HISTORY", "QUIRK"):
            for r in classes.get(cls, []):
                if len(findings) >= args.minimize: break
                c = r["case"]
                key = (cls, c.profile, r["diffs"][0][:20] if r["diffs"] else "")
                if key in seen: continue
                seen.add(key)
                m = minimize(c, differs)
                res = evaluate([m], not args.no_tmux, not args.no_pyte, args.attrs)[0]
                mcls, mdiffs = classify(res, args.attrs)
                findings.append(dict(cls=mcls, was=cls, name=c.name, rows=m.rows, cols=m.cols, repro=esc_literal(res["eff"].data), diffs=mdiffs[:6],
                                     vt=res["vt"].rows, tmux=res["tmux"].rows if "tmux" in res else None,
                                     pyte=res["pyte"].rows if "pyte" in res else None,
                                     vt_cur=res["vt"].cursor(), tmux_cur=res["tmux"].cursor() if "tmux" in res else None,
                                     pyte_cur=res["pyte"].cursor() if "pyte" in res else None))

    print()
    print("== summary ==")
    for cls in ("OK", "BUG-HI", "REVIEW", "HISTORY", "QUIRK", "SPLIT", "CRASH", "KNOWN"):
        print("  %-8s %d" % (cls, len(classes.get(cls, []))))
    byprof = {}
    for r in results:
        p = r["case"].profile or r["case"].source
        byprof.setdefault(p, [0, 0])
        byprof[p][0] += 1
        if r["cls"] != "OK": byprof[p][1] += 1
    print("  by profile (runs, not OK): " + ", ".join("%s %d/%d" % (p, v[1], v[0]) for p, v in sorted(byprof.items())))
    for cls in ("CRASH", "SPLIT", "BUG-HI", "REVIEW", "HISTORY", "QUIRK"):
        lst = classes.get(cls, [])
        if not lst: continue
        print("\n== %s: %d ==" % (cls, len(lst)))
        for r in lst[:args.show]:
            c = r["eff"]
            print("- %s (%dx%d)" % (c.name, c.rows, c.cols))
            for d in r["diffs"][:3]: print("    " + d)
    if findings:
        print("\n== minimised ==")
        for f in findings:
            print("[%s] %s  %dx%d  %s" % (f["cls"], f["name"], f["rows"], f["cols"], f["repro"]))
            for d in f["diffs"][:4]: print("      " + d)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(dict(findings=findings, counts={k: len(v) for k, v in classes.items()},
                           failures=[dict(name=r["case"].name, cls=r["cls"], rows=r["case"].rows, cols=r["case"].cols,
                                          repro=esc_literal(r["eff"].data), diffs=r["diffs"][:5])
                                     for r in results if r["cls"] != "OK"]), fh, indent=1)
    # the exit status: real differences only (REVIEW and QUIRK are for reading: tmux and pyte quirks, see FINDINGS)
    fails = sum(len(v) for k, v in classes.items() if k in ("BUG-HI", "HISTORY", "SPLIT", "CRASH"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
