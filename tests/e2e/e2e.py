#!/usr/bin/env python3
"""End-to-end tests: the real vtmux binary in a pseudo-terminal, with real programs inside, read back through a terminal emulator (pyte).
Run:  tests/e2e/run.sh   (needs build/vtmux, python3, pyte)"""
import fcntl, os, pty, select, signal, struct, sys, termios, time
import pyte

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, 'build', 'vtmux')
failures = 0

def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('  FAIL:', what)
    return cond

# What a terminal answers to the questions vtmux asks at start-up (vtpen's probe). 'plain': it knows the size of its cells but not Sixel;
# 'sixel': it can draw Sixel too; None: it answers nothing at all (the probe gives up after a second, and what was typed meanwhile must survive).
def start_up_answers(kind):
    if kind is None: return []
    da1 = b'\x1b[?62;4c' if kind == 'sixel' else b'\x1b[?62c'
    return [(b'\x1b[16t', b'\x1b[6;20;10t'), (b'\x1b[>c', b'\x1b[>0;10;1c'), (b'\x1b[18t', b'\x1b[8;30;100t'),
            (b'\x1b[14t', b'\x1b[4;600;1000t'), (b'\x1b[c', da1)]

class Run:
    def __init__(self, args=(), cols=100, rows=30, env=None, terminal='plain'):
        self.cols, self.rows = cols, rows
        self.answers = start_up_answers(terminal)
        self.asked = b''
        self.raw = b''
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ['TERM'] = 'xterm-256color'
            os.environ['SHELL'] = '/bin/bash'
            os.environ['PS1'] = 'T$ '
            os.environ['HISTFILE'] = '/dev/null'
            if env: os.environ.update(env)
            args = list(args)
            if '--transition-ms' not in args and '--fade-ms' not in args: args = ['--transition-ms', '150'] + args    # (quick: the tests wait 0.6 s)
            os.execv(BIN, ['vtmux'] + args)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.alive = True
        self.status = None

    def pump(self, seconds=0.1):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.01)
            if r:
                try: data = os.read(self.fd, 1 << 20)
                except OSError: self.alive = False; return
                if not data: self.alive = False; return
                self.raw += data
                self.stream.feed(data)
                self.answer_start_up(data)

    def answer_start_up(self, data):
        # the questions come before vtmux takes the screen over (the alternate screen); after that nothing is answered
        if not self.answers or b'\x1b[?1049h' in self.raw: return
        self.asked += data
        for q, a in self.answers:
            i = self.asked.find(q)
            if i >= 0:
                self.asked = self.asked[:i] + self.asked[i + len(q):]
                os.write(self.fd, a)

    def send(self, s):
        os.write(self.fd, s.encode() if isinstance(s, str) else s)

    def text(self):
        return '\n'.join(line.rstrip() for line in self.screen.display)

    def wait_for(self, what, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            if what in self.text(): return True
        return what in self.text()

    def wait_gone(self, what, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            if what not in self.text(): return True
        return what not in self.text()

    def bar(self):
        return self.screen.display[self.rows - 1].rstrip()

    def wait_exit(self, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            try:
                pid, st = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return True
            if pid:
                self.status = st
                self.alive = False
                return True
        return False

    def close(self):
        if self.status is None:
            try: os.kill(self.pid, signal.SIGKILL)
            except OSError: pass
            try: os.waitpid(self.pid, 0)
            except OSError: pass
        try: os.close(self.fd)
        except OSError: pass

    def cpu_seconds(self):
        with open('/proc/%d/stat' % self.pid) as f:
            parts = f.read().rsplit(')', 1)[1].split()
        return (int(parts[11]) + int(parts[12])) / os.sysconf('SC_CLK_TCK')

def start(args=(), **kw):
    r = Run(args, **kw)
    r.send('echo READY$((20+22))\n')                        # (the prompt is whatever the user's shell set up; a marker is reliable)
    ok = r.wait_for('READY42')
    check(ok, 'the shell starts and runs a command')
    return r

def test_basic():
    r = start()
    check('1:bash' in r.bar(), 'the bar names the console: ' + r.bar())
    r.send('echo $((6*7))mark\n')
    check(r.wait_for('42mark'), 'a typed command runs and its output shows')
    r.send('exit\n')
    check(r.wait_exit(), 'vtmux ends when the last console ends')
    check(r.status is not None and os.WIFEXITED(r.status) and os.WEXITSTATUS(r.status) == 0, 'it exits cleanly')
    r.pump(0.2)
    check(b'\x1b[?1049l' in r.raw, 'the terminal leaves the alternate screen')
    check(b'\x1b[?25h' in r.raw, 'the cursor is shown again')
    r.close()

def test_ctrl_c_and_z():
    r = start()
    r.send('sleep 100\n'); r.pump(0.5)
    r.send('\x03')                                          # Ctrl-C reaches the program, not vtmux
    r.send('echo $((1+1))ok\n')
    check(r.wait_for('2ok'), 'Ctrl-C interrupts the program and the shell carries on')
    r.send('sleep 100\n'); r.pump(0.5)
    r.send('\x1a')                                          # Ctrl-Z stops the job
    check(r.wait_for('Stopped'), 'Ctrl-Z reaches the shell (job control works)')
    r.send('kill %1; exit\n'); r.pump(0.3)
    r.send('exit\n')
    r.wait_exit()
    r.close()

def test_resize():
    import re
    r = start()
    mark = len(r.raw)
    fcntl.ioctl(r.fd, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 70, 0, 0))
    os.kill(r.pid, signal.SIGWINCH)
    r.pump(0.5)
    r.send('stty size\n')
    check(r.wait_for('19 70'), 'a resize reaches the program (one row is the bar): ' + r.text()[-200:])
    after = r.raw[mark:]
    check(re.search(rb'\x1b\[20;\d+H', after) is not None, 'the bar is drawn on the new last row (20)')
    check(re.search(rb'\x1b\[2[1-9];\d+H|\x1b\[30;\d+H', after) is None, 'nothing is drawn below the new bottom')
    r.send('exit\n'); r.wait_exit(); r.close()

def test_commands_and_switching():
    r = start()
    r.send('echo FIRST$((1+0))\n'); check(r.wait_for('FIRST1'), 'first console works')
    r.send('\x02:new two\r'); r.pump(0.6)
    check('2:two' in r.bar(), 'a new console is on the bar: ' + r.bar())
    r.send('echo SECOND$((1+1))\n'); check(r.wait_for('SECOND2'), 'second console works')
    check('FIRST1' not in r.text(), 'only the second console is shown')
    r.send('\x02'); r.pump(0.2)
    check('COMMAND' in r.bar(), 'command mode shows on the bar: ' + r.bar())
    r.send('\x1b[D'); r.pump(0.6)                           # Left: back to the first, with a fade
    check(r.wait_for('FIRST1'), 'the arrow switches back')
    check('SECOND2' not in r.text(), 'the first console shows alone after the fade')
    r.send('\x1b[C'); r.pump(0.6)
    check('SECOND2' in r.text(), 'Right goes to the second again')
    r.send('\r'); r.pump(0.2)
    check('COMMAND' not in r.bar(), 'Enter leaves command mode')
    r.send('\x02:close\r'); r.pump(0.8)
    check('2:two' not in r.bar() and '1:bash' in r.bar(), 'close removes the console: ' + r.bar())
    check('FIRST1' in r.text(), 'the other console shows')
    r.send('\x02:rename mine\r'); r.pump(0.3)
    check('1:mine' in r.bar(), 'rename: ' + r.bar())
    r.send('\x02:bogus\r'); r.pump(0.3)
    check('unknown command' in r.bar(), 'an unknown command says so: ' + r.bar())
    r.send('\x02:quit\r')
    check(r.wait_exit(), ':quit ends vtmux')
    r.close()

def test_alt_screen_programs():
    with open('/tmp/vtmux_e2e.txt', 'w') as f:
        f.write('\n'.join('line number %d' % i for i in range(1, 80)) + '\n')
    r = start()
    r.send('echo BEFORE$((3+4))\n'); check(r.wait_for('BEFORE7'), 'marker before')
    r.send('less /tmp/vtmux_e2e.txt\n')
    check(r.wait_for('line number 1'), 'less shows the file')
    check('BEFORE7' not in r.text(), 'less has its own screen')
    r.send(' '); r.pump(0.4)
    check('line number 29' in r.text() or 'line number 30' in r.text(), 'less scrolls a page')
    r.send('q')
    check(r.wait_for('BEFORE7'), 'the screen as it was comes back when less quits')
    if os.path.exists('/usr/bin/vi'):
        r.send('vi /tmp/vtmux_e2e.txt\n')
        check(r.wait_for('line number 1'), 'vi shows the file')
        r.send('ihello-from-vi\x1b'); r.pump(0.4)
        check('hello-from-vi' in r.text(), 'typing in vi works')
        r.send(':q!\r')
        check(r.wait_for('BEFORE7'), 'the screen comes back when vi quits')
    r.send('exit\n'); r.wait_exit(); r.close()

def test_top_and_htop():
    r = start()
    r.send('top -b -n1 | head -8\n')
    check(r.wait_for('PID'), 'top output shows')
    if os.path.exists('/usr/bin/htop'):
        r.send('htop\n'); r.pump(2.0)
        check('PID' in r.text() or 'CPU' in r.text(), 'htop draws its screen: ' + r.text()[:200])
        check(b'\x1b[?1000h' in r.raw or b'\x1b[?1006h' in r.raw, 'htop asked for the mouse and the real terminal was put in that mode')
        r.send('q'); r.pump(0.8)
        check(b'\x1b[?1000l' in r.raw, 'the mouse mode was turned off again')
    r.send('exit\n'); r.wait_exit(); r.close()

def test_mode_mirroring():
    r = start()
    r.send("printf '\\033[?1h\\033[?1000h\\033[?1006h\\033[?1004h'\n"); r.pump(0.6)
    for seq, what in ((b'\x1b[?1h', 'application cursor keys'), (b'\x1b[?1000h', 'mouse tracking'), (b'\x1b[?1006h', 'the SGR mouse encoding'), (b'\x1b[?1004h', 'focus reporting')):
        check(seq in r.raw, 'the real terminal was put in ' + what)
    r.send("printf '\\033[?1l\\033[?1000l\\033[?1006l\\033[?1004l'\n"); r.pump(0.6)
    check(r.raw.rfind(b'\x1b[?1l') > r.raw.rfind(b'\x1b[?1h'), 'and out of it again')
    # the modes follow the console that is shown
    mark = len(r.raw)
    r.send("printf '\\033[?1h'\n"); r.pump(0.3)
    r.send('\x02:new b\r'); r.pump(0.6)
    check(b'\x1b[?1l' in r.raw[mark:], 'a console without application cursor keys puts the terminal back')
    r.send('\x02\x1b[D'); r.pump(0.6)
    check(r.raw.rfind(b'\x1b[?1h') > r.raw.rfind(b'\x1b[?1l'), 'switching back to the one that wants them turns them on')
    r.send('\x02:quit\r'); r.wait_exit(); r.close()

def test_utf8_and_input_exactness():
    r = start()
    r.send('echo \xc3\xa9-\xc3\xbc-end\n'.encode('latin-1') if False else 'echo é-ü-end\n')
    check(r.wait_for('é-ü-end'), 'UTF-8 goes through both ways')
    r.send("printf '%s' 'a\x01b' | od -An -c\n"); r.pump(0.5)
    r.send('exit\n'); r.wait_exit(); r.close()

def test_attributes_and_wide():
    r = start()
    # bold, italic, underline, strikethrough, blink, and wide letters, printed by the shell and read back from what the terminal was told
    r.send("printf '\\033[1mBOLD\\033[0m \\033[3mITAL\\033[0m \\033[4mUNDR\\033[0m \\033[9mSTRK\\033[0m \\033[5mBLNK\\033[0m WIDE:\\xe6\\x97\\xa5\\xe6\\x9c\\xac|END\\n'\n")
    check(r.wait_for('BOLD'), 'the attribute line appears')
    r.pump(0.4)
    row = next((i for i, l in enumerate(r.screen.display) if 'BOLD' in l and 'printf' not in l), None)
    check(row is not None, 'found the line')
    if row is not None:
        buf = r.screen.buffer[row]
        line = r.screen.display[row]
        def attr_of(word):
            col = line.index(word)
            return buf[col]
        check(attr_of('BOLD').bold, 'bold reaches the terminal')
        check(attr_of('ITAL').italics, 'italic reaches the terminal')
        check(attr_of('UNDR').underscore, 'underline reaches the terminal')
        check(attr_of('STRK').strikethrough, 'strikethrough reaches the terminal')
        check(getattr(attr_of('BLNK'), 'blink', False), 'blink reaches the terminal')
        check(not attr_of('WIDE').bold and not attr_of('WIDE').underscore, 'the attributes stop where they should')
        check('WIDE:日本|END' in line, 'wide letters arrive as letters (not question marks): ' + line.strip())
    r.send('exit\n'); r.wait_exit(); r.close()

def test_clipboard_with_micro():
    # micro with its clipboard set to "terminal" copies with an OSC 52 sequence: it has to reach the real terminal
    import shutil, tempfile, base64
    if not shutil.which('micro'):
        print('  (micro is not installed: skipped)'); return
    d = tempfile.mkdtemp(prefix='vtmux_micro_')
    open(d + '/settings.json', 'w').write('{"clipboard": "terminal"}')
    open(d + '/file.txt', 'w').write('copy this text\nsecond line\n')
    r = Run(['--', 'micro', d + '/file.txt'], cols=80, rows=25, env={'MICRO_CONFIG_HOME': d})
    r.pump(1.5)
    r.send('\x01'); r.pump(0.4)                        # select all
    mark = len(r.raw)
    r.send('\x03'); r.pump(1.0)                        # copy
    new = r.raw[mark:]
    m = [x for x in new.split(b'\x1b]52;c;')[1:]]
    want = base64.b64encode(b'copy this text\nsecond line\n')
    check(any(x.startswith(want + b'\x07') for x in m), 'the copy reaches the real terminal as OSC 52: ' + repr(new[-120:]))
    check('Copied selection' in r.text(), 'micro says it copied')
    # with the clipboard off in vtmux nothing goes out
    r.send('\x02:set clipboard off\r'); r.pump(0.3)
    mark = len(r.raw)
    r.send('\x03'); r.pump(0.8)
    check(b'\x1b]52' not in r.raw[mark:], 'nothing is sent with the clipboard off')
    r.send('\x11'); r.pump(0.3); r.close()
    shutil.rmtree(d, ignore_errors=True)

def test_no_color_and_args():
    r = Run(['--no-color'])
    r.send('echo READY$((20+22))\n')
    check(r.wait_for('READY42'), 'starts with --no-color')
    r.send("printf '\\033[31mred\\033[0m'\n"); r.pump(0.5)
    check(b'\x1b[38;2;' not in r.raw and b'\x1b[48;2;' not in r.raw, 'no colour codes with --no-color')
    r.send('exit\n'); r.wait_exit(); r.close()
    r = Run(['--', 'sh', '-c', 'echo ran-it; sleep 0.5'])
    check(r.wait_for('ran-it'), 'a command after -- runs in the first console')
    check(r.wait_exit(), 'vtmux ends when that command ends')
    r.close()
    r = Run(['--fade', 'nonsense'])
    check(r.wait_exit(), 'a bad option ends it at once')
    r.close()

def test_heavy_output_and_idle():
    r = start()
    r.send('seq 1 200000; echo done$((1+1))\n')
    check(r.wait_for('done2', timeout=20), 'a lot of output goes through')
    check('200000' in r.text(), 'with the last lines on screen')
    r.pump(1.0)
    c0, t0 = r.cpu_seconds(), time.time()
    r.pump(4.0)
    used = (r.cpu_seconds() - c0) / (time.time() - t0)
    check(used < 0.05, 'idle costs little CPU (%.1f %%)' % (used * 100))
    r.send('exit\n'); r.wait_exit(); r.close()

def test_rapid_switching():
    r = start()
    for name in ('b', 'c', 'd'):
        r.send('\x02:new %s\r' % name); r.pump(0.3)
    check('4:d' in r.bar(), 'four consoles: ' + r.bar())
    r.send('\x02')
    for i in range(60):
        r.send('\x1b[C' if i % 3 else '\x1b[D'); r.pump(0.01)
    r.pump(1.0)
    r.send('\r'); r.pump(0.3)
    check(r.alive, 'still alive after 60 quick switches')
    r.send('echo STILL$((1+1))\n')
    check(r.wait_for('STILL2'), 'and responsive')
    r.send('\x02:quit\r'); r.wait_exit(); r.close()

def test_kill_leaves_nothing():
    r = start()
    r.send('\x02:new x -- sleep 1000\r'); r.pump(0.5)
    r.send('echo $$ > /tmp/vtmux_e2e_pid; sleep 1\n')
    r.send('\x02:close\r'); r.pump(0.8)
    # the sleep program was hung up
    time.sleep(0.5)
    out = os.popen("pgrep -fx 'sleep 1000' | head -3").read().strip()
    check(out == '', 'a closed console leaves no program behind: ' + out)
    r.send('\x02:quit\r'); r.wait_exit(); r.close()

def test_outer_terminal():
    """What the real terminal can do reaches the consoles: the size of a cell, and Sixel only if the terminal draws it. What is typed
    while the terminal is being asked is not lost, even when the terminal never answers."""
    ask = "stty raw -echo; printf '\\033[c\\033[16t'; head -c %d | tr '\\033' '@'; sleep 30"
    for kind, want, count in (('sixel', '@[?1;2;4c@[6;20;10t', 19), ('plain', '@[?1;2c@[6;20;10t', 17)):
        r = Run(['--', 'sh', '-c', ask % count], terminal=kind)
        check(r.wait_for(want, timeout=6), "a terminal that is '%s': the console sees %s (screen: %r)" % (kind, want, r.text()[:200]))
        r.send('\x02:quit\r'); r.wait_exit(); r.close()
    # a terminal that never answers: the console sees no pixel size, no Sixel, and the typing in the first second is kept
    r = Run(['--', 'sh', '-c', ask % 7], terminal=None)
    r.send('')                                              # (typing goes to the console only once it is up; the early keys wait in the tty)
    check(r.wait_for('@[?1;2c', timeout=6), "a terminal that answers nothing: plain device attributes (screen: %r)" % r.text()[:200])
    check('@[?1;2c@[6;' not in r.text(), 'and no pixel size')
    r.send('\x02:quit\r'); r.wait_exit(); r.close()
    r = Run(terminal=None)
    r.send('echo AHEAD$((20+22))\n')                        # typed at once, while vtmux is still asking
    check(r.wait_for('AHEAD42', timeout=8), 'what was typed while the terminal was asked is not lost')
    r.send('\x02:quit\r'); r.wait_exit(); r.close()
    # --no-images asks nothing
    r = Run(['--no-images', '--', 'sh', '-c', ask % 7], terminal='sixel')
    check(r.wait_for('@[?1;2c', timeout=6), '--no-images: no Sixel offered even by a terminal that draws it (screen: %r)' % r.text()[:200])
    r.send('\x02:quit\r'); r.wait_exit(); r.close()

def test_sixel_picture():
    """A program draws a Sixel picture in its console: a terminal that draws Sixel is sent the picture, at the cells it covers; a terminal
    that does not is sent none (and the program is not told it can)."""
    pic = '\\033[3;5H\\033Pq"1;1;20;40#1;2;100;0;0' + '#1!20~-' * 7 + '\\033\\\\SIXELDONE'       # at row 3, column 5: 2 x 2 cells (a cell is 10 x 20 pixels)
    cmd = "printf '%s'; sleep 30" % pic
    r = Run(['--', 'sh', '-c', cmd], terminal='sixel')
    check(r.wait_for('SIXELDONE'), 'the console shows what follows the picture (screen: %r)' % r.text()[:200])
    r.pump(0.5)
    check(r.screen.display[4].startswith('    SIXELDONE'), 'the cursor was left below the picture (row 5, column 5): %r' % r.screen.display[4][:30])
    check(b'\x1b[3;5H\x1bP0;1;0q"1;1;20;20' in r.raw, 'the picture is sent for the first row of cells it covers')
    check(b'\x1b[4;5H\x1bP0;1;0q"1;1;20;20' in r.raw, 'and for the second')
    check(r.raw.count(b'\x1bP0;1;0q') == 2, 'and for nothing else (%d pictures)' % r.raw.count(b'\x1bP0;1;0q'))
    r.send('\x02:quit\r'); r.wait_exit(); r.close()
    r = Run(['--', 'sh', '-c', cmd], terminal='plain')
    check(r.wait_for('SIXELDONE'), 'a terminal without Sixel: the console runs on (screen: %r)' % r.text()[:200])
    r.pump(0.5)
    check(b'\x1bP0;1;0q' not in r.raw, 'no picture is sent to a terminal that cannot draw it')
    check(r.screen.display[2].startswith('    SIXELDONE'), 'and the picture took no room (the text is where the cursor was left)')
    r.send('\x02:quit\r'); r.wait_exit(); r.close()

TESTS = [test_basic, test_ctrl_c_and_z, test_resize, test_commands_and_switching, test_alt_screen_programs, test_top_and_htop,
         test_mode_mirroring, test_utf8_and_input_exactness, test_no_color_and_args, test_heavy_output_and_idle,
         test_rapid_switching, test_kill_leaves_nothing, test_attributes_and_wide, test_clipboard_with_micro, test_outer_terminal,
         test_sixel_picture]

if __name__ == '__main__':
    if not os.path.exists(BIN):
        print('build first: cmake -B build && cmake --build build'); sys.exit(2)
    names = sys.argv[1:]
    for t in TESTS:
        if names and t.__name__ not in names and t.__name__[5:] not in names: continue
        before = failures
        print('==', t.__name__, flush=True)
        try:
            t()
        except Exception as e:
            failures += 1
            print('  EXCEPTION:', repr(e))
        print('   ', 'ok' if failures == before else 'FAILED', flush=True)
    print('all e2e tests passed' if failures == 0 else '%d failures' % failures)
    sys.exit(0 if failures == 0 else 1)
