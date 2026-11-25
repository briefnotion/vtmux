#!/usr/bin/env python3
"""capture.py: run real programs in a pty of a known size and keep what they write, as NAME.ROWSxCOLS.bin files
(by default in $VTCONF_WORK/captured; conform.py then runs them like any other stream).

The programs are what is installed: ls, cat, ps, top, less, vi, nano, htop, python curses, printf.  Output varies from run
to run (times, pids), which does not matter: the bytes are captured once and then given to every emulator.
Each program is run with a TERM that selects different sequences: xterm-256color, vt100, vt102, screen, linux.
Usage: capture.py [OUTDIR] [--quick]
"""
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import sys
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("VTCONF_WORK", "/tmp/vtconf")


def run(argv, rows, cols, term="xterm-256color", keys=(), timeout=8.0, env_extra=None):
    """keys: list of (delay_seconds, bytes) sent to the program.  Returns the bytes it wrote."""
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM=term, LINES=str(rows), COLUMNS=str(cols), LANG="C.UTF-8", LC_ALL="C.UTF-8")
        env.pop("TMUX", None)
        if env_extra: env.update(env_extra)
        try:
            os.execvpe(argv[0], argv, env)
        except OSError:
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = bytearray()
    start = time.time()
    pending = sorted(keys, key=lambda k: k[0])
    status = None
    try:
        while time.time() - start < timeout:
            while pending and time.time() - start >= pending[0][0]:
                try: os.write(fd, pending.pop(0)[1])
                except OSError: pending = []
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk: break
                out += chunk
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                # read what is left
                try:
                    while select.select([fd], [], [], 0.05)[0]:
                        chunk = os.read(fd, 65536)
                        if not chunk: break
                        out += chunk
                except OSError:
                    pass
                pid = None
                break
    finally:
        if pid:
            try: os.kill(pid, signal.SIGKILL)
            except OSError: pass
            try: os.waitpid(pid, 0)
            except OSError: pass
        os.close(fd)
    return bytes(out)


def which(name): return shutil.which(name)


def jobs(quick):
    big_file = os.path.join(HERE, "conform.py")
    j = []
    sizes = [(24, 80), (10, 40), (40, 120)] if not quick else [(24, 80)]
    for rows, cols in sizes:
        for term in ("xterm-256color", "vt100", "linux") if not quick else ("xterm-256color",):
            tag = term.replace("-256color", "")
            if which("ls"): j.append(("ls-%s" % tag, rows, cols, term, ["ls", "--color=always", "-la", "/usr/bin"], []))
            j.append(("cat-%s" % tag, rows, cols, term, ["cat", big_file], []))
            j.append(("printf-%s" % tag, rows, cols, term, ["printf", "\\033[1mbold\\033[0m \\033[31mred\\033[0m\\n\\ttab\\n%s\\n" % ("x" * (cols + 10))], []))
            if which("ps"): j.append(("ps-%s" % tag, rows, cols, term, ["ps", "aux", "--width", str(cols)], []))
            if which("top"): j.append(("top-%s" % tag, rows, cols, term, ["top", "-n", "2", "-d", "0.5"], []))
            if which("less"):
                j.append(("less-%s" % tag, rows, cols, term, ["less", big_file],
                          [(0.6, b" "), (0.9, b" "), (1.2, b"b"), (1.5, b"G"), (1.8, b"/def\r"), (2.2, b"n"), (2.6, b"g"), (3.0, b"q")]))
            if which("vi"):
                j.append(("vi-%s" % tag, rows, cols, term, ["vi", "-u", "NONE", "-N", big_file],
                          [(0.8, b"G"), (1.1, b"dd"), (1.4, b"O"), (1.5, b"hello vi world"), (1.7, b"\x1b"), (1.9, b"gg"), (2.1, b":set nu\r"),
                           (2.5, b"/import\r"), (2.8, b"3j"), (3.1, b"x"), (3.4, b":q!\r")]))
            if which("nano"):
                j.append(("nano-%s" % tag, rows, cols, term, ["nano", "--ignorercfiles", big_file],
                          [(0.8, b"\x16"), (1.1, b"\x16"), (1.4, b"hello nano"), (1.8, b"\x17"), (2.2, b"\x18"), (2.6, b"n")]))
            if which("htop") and term == "xterm-256color":
                j.append(("htop-%s" % tag, rows, cols, term, ["htop"], [(1.2, b"\x1b[B"), (1.5, b"\x1b[B"), (2.0, b"q")]))
    # python curses: a small program that uses colours, boxes, scrolling and insert/delete line
    prog = os.path.join(WORK, "curses_demo.py")
    os.makedirs(WORK, exist_ok=True)
    with open(prog, "w") as f:
        f.write('''import curses, time
def main(s):
    curses.start_color(); curses.use_default_colors()
    for i in range(1, 8): curses.init_pair(i, i, -1)
    s.box()
    s.addstr(1, 2, "curses demo", curses.A_BOLD)
    for i in range(1, 8): s.addstr(2 + i, 2, "colour %d" % i, curses.color_pair(i))
    s.addstr(11, 2, "reverse", curses.A_REVERSE); s.addstr(11, 12, "underline", curses.A_UNDERLINE)
    s.refresh(); time.sleep(0.3)
    s.scrollok(True); s.idlok(True)
    h, w = s.getmaxyx()
    s.setscrreg(2, h - 3)
    for i in range(6):
        s.scroll(1); s.addstr(h - 3, 2, "scrolled %d" % i); s.refresh(); time.sleep(0.05)
    s.move(4, 2); s.insertln(); s.addstr(4, 2, "inserted"); s.deleteln(); s.refresh(); time.sleep(0.2)
    s.addstr(h - 2, 2, "x" * (w - 4)); s.insstr(h - 2, 3, "INS"); s.delch(h - 2, 5); s.refresh(); time.sleep(0.2)
curses.wrapper(main)
''')
    for rows, cols in sizes:
        for term in ("xterm-256color", "vt100", "screen") if not quick else ("xterm-256color",):
            j.append(("curses-%s" % term.replace("-256color", ""), rows, cols, term, [sys.executable, "-I", prog], []))
    return j


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quick = "--quick" in sys.argv
    outdir = args[0] if args else os.path.join(WORK, "captured")
    os.makedirs(outdir, exist_ok=True)
    for fn in os.listdir(outdir):
        if fn.endswith(".bin"): os.unlink(os.path.join(outdir, fn))
    n = 0
    for name, rows, cols, term, argv, keys in jobs(quick):
        data = run(argv, rows, cols, term, keys)
        if len(data) < 20:
            print("  skipped %s (%d bytes)" % (name, len(data)))
            continue
        with open(os.path.join(outdir, "%s.%dx%d.bin" % (name, rows, cols)), "wb") as f:
            f.write(data)
        n += 1
    print("captured %d streams in %s" % (n, outdir))


if __name__ == "__main__":
    main()
