#!/usr/bin/env python3
"""fuzz.py N: feed N streams of random and mutated bytes to a vtdump built with AddressSanitizer and UBSan (and random
resizes between the pieces) and report a crash, a sanitizer message, a timeout or a broken invariant.  No reference is
involved: the emulator must survive any input.   Usage: fuzz.py [N] [seed]
"""
import os
import random
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vtconf
import gen_corpus

ASAN = os.path.join(vtconf.WORK, "vtdump_asan")


def build():
    os.makedirs(vtconf.WORK, exist_ok=True)
    srcs = [os.path.join(vtconf.ROOT, "tools", "vtdump.cpp")] + [os.path.join(vtconf.ROOT, "src", f) for f in ("vtparser.cpp", "emulator.cpp", "width.cpp")]
    newest = max(os.path.getmtime(x) for x in srcs + [os.path.join(vtconf.ROOT, "src", f) for f in os.listdir(os.path.join(vtconf.ROOT, "src")) if f.endswith(".h")])
    if os.path.exists(ASAN) and os.path.getmtime(ASAN) >= newest: return True
    print("building the sanitizer vtdump ...", flush=True)
    r = subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-I" + vtconf.ROOT, "-o", ASAN] + srcs,
                       capture_output=True, text=True)
    if r.returncode != 0: print(r.stderr); return False
    return True


PIECES = [b"\x1b[", b"\x1b]", b"\x1bP", b"\x1b(", b"\x1b)", b"\x1b#", b"\x1b", b";", b":", b"?", b">", b"!", b" ", b"$", b"0", b"1", b"2", b"9", b"99999", b"-1",
          b"H", b"J", b"K", b"m", b"r", b"h", b"l", b"@", b"P", b"L", b"M", b"X", b"b", b"S", b"T", b"I", b"Z", b"c", b"n", b"g", b"d", b"`", b"e", b"a", b"s", b"u",
          b"\x07", b"\x08", b"\x09", b"\x0a", b"\x0b", b"\x0c", b"\x0d", b"\x0e", b"\x0f", b"\x18", b"\x1a", b"\x7f", b"\x00", b"\x9b", b"\x80", b"\xc3", b"\xe4\xb8", b"\xf0\x9f",
          "中".encode(), "é".encode(), "́".encode(), "😀".encode(), b"\xff", b"\xed\xa0\x80", b"?2l", b"\x1b<", b"\x1bY", b"?1049h", b"?1049l", b"?47h", b"?6h", b"?7l", b"4h", b"20h"]


def make(rng, n, corpus):
    kind = rng.random()
    if kind < 0.25:
        return bytes(rng.randrange(256) for _ in range(rng.randint(1, 2000)))
    if kind < 0.6:
        return b"".join(rng.choice(PIECES) for _ in range(rng.randint(5, 600)))
    if kind < 0.8 and corpus:
        d = bytearray(rng.choice(corpus))
        for _ in range(rng.randint(1, 20)):
            if not d: break
            i = rng.randrange(len(d))
            op = rng.random()
            if op < 0.4: d[i] = rng.randrange(256)
            elif op < 0.7: del d[i:i + rng.randint(1, 8)]
            else: d[i:i] = rng.choice(PIECES)
        return bytes(d[:20000])
    return b"".join(gen_corpus.random_cases(1, rng.randrange(10 ** 9))[0].tokens)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if not build(): return 2
    rng = random.Random(seed)
    corpus = [c.data[:20000] for c in gen_corpus.regress_cases()]
    cap = os.path.join(vtconf.WORK, "captured")
    if os.path.isdir(cap):
        corpus += [c.data[:20000] for c in gen_corpus.captured_cases(cap)]
    sizes = [(24, 80), (1, 1), (1, 5), (5, 1), (2, 2), (3, 7), (10, 40), (30, 120), (4, 200), (8, 3)]
    jobs = []
    for i in range(n):
        rows, cols = rng.choice(sizes)
        jobs.append((i, rows, cols, make(rng, i, corpus), rng.choice([0, 0, 1, 7]), rng.randrange(1 << 30)))

    def run(job):
        i, rows, cols, data, chunk, fseed = job
        path = os.path.join(vtconf.WORK, "fuzz_%d_%d.bin" % (os.getpid(), i))
        with open(path, "wb") as f: f.write(data)
        res = []
        try:
            for extra in ([], ["--resize-fuzz", str(fseed)]):
                cmd = [ASAN, str(rows), str(cols), path] + (["--chunk", str(chunk)] if chunk else []) + extra
                try:
                    r = subprocess.run(cmd, capture_output=True, timeout=60, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
                except subprocess.TimeoutExpired:
                    res.append("timeout"); continue
                out = r.stdout.decode("utf8", "replace")
                if r.returncode != 0: res.append("exit %d: %s" % (r.returncode, r.stderr.decode("utf8", "replace")[:600]))
                elif "invariant" in out: res.append("invariant: " + [l for l in out.split("\n") if l.startswith("invariant")][0])
        finally:
            try: os.unlink(path)
            except OSError: pass
        return (job, res)

    bad = 0
    with ThreadPoolExecutor(8) as ex:
        for job, res in ex.map(run, jobs):
            if res:
                bad += 1
                i, rows, cols, data, chunk, fseed = job
                keep = os.path.join(vtconf.WORK, "fuzz_fail_%d.bin" % i)
                with open(keep, "wb") as f: f.write(data)
                print("FAIL stream %d (%dx%d, chunk %d, resize seed %d), saved as %s: %s" % (i, rows, cols, chunk, fseed, keep, res[0]))
    print("fuzz: %d streams, %d failures" % (n, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
