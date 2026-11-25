#!/usr/bin/env python3
"""Runs the real vtmux in a pseudo-terminal, types into it, and saves screenshots (PNG) of what a terminal would show.
Needs: python3 with pyte and PIL.   Usage: tools/demo.py [outdir]   (default docs/)"""
import fcntl, os, pty, select, struct, sys, termios, time
import pyte
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'vtmux')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs')
COLS, ROWS = 100, 28
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 16)
CW, CH = 10, 20

def colour(c, default):
    if c == 'default': return default
    if len(c) == 6:
        try: return tuple(int(c[i:i+2], 16) for i in (0, 2, 4))
        except ValueError: pass
    names = {'black': (0,0,0), 'red': (205,49,49), 'green': (13,188,121), 'brown': (229,229,16), 'blue': (36,114,200),
             'magenta': (188,63,188), 'cyan': (17,168,205), 'white': (229,229,229)}
    return names.get(c, default)

def render(screen, path):
    img = Image.new('RGB', (screen.columns * CW, screen.lines * CH), (12, 12, 12))
    d = ImageDraw.Draw(img)
    for y in range(screen.lines):
        row = screen.buffer[y]
        for x in range(screen.columns):
            ch = row[x]
            fg, bg = colour(ch.fg, (204, 204, 204)), colour(ch.bg, (12, 12, 12))
            if ch.reverse: fg, bg = bg, fg
            if bg != (12, 12, 12): d.rectangle([x*CW, y*CH, x*CW+CW-1, y*CH+CH-1], fill=bg)
            if ch.data != ' ': d.text((x*CW, y*CH), ch.data, font=FONT, fill=fg)
    img.save(path)

class Run:
    def __init__(self, args=()):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ['TERM'] = 'xterm-256color'
            os.environ['SHELL'] = '/bin/sh'              # a plain prompt, so the pictures show nothing about whoever ran them
            os.environ['PS1'] = '$ '
            os.environ['PWD'] = '/'; os.chdir('/tmp')
            os.execv(BIN, ['vtmux'] + list(args))
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0, 0))
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.alive = True
    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.01)
            if r:
                try: data = os.read(self.fd, 1 << 20)
                except OSError: self.alive = False; return
                if not data: self.alive = False; return
                self.stream.feed(data)
    def send(self, text):
        os.write(self.fd, text.encode() if isinstance(text, str) else text)
    def shot(self, name):
        render(self.screen, os.path.join(OUT, name))
    def text(self):
        return '\n'.join(self.screen.display)
    def close(self):
        try: os.kill(self.pid, 15)
        except OSError: pass
        try: os.waitpid(self.pid, 0)
        except OSError: pass

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    r = Run()
    r.pump(1.5); r.shot('demo_1_normal.png')
    r.send('ls --color -l /etc | head -12\n'); r.pump(1.0); r.shot('demo_2_after_ls.png')
    r.send('\x02'); r.pump(0.5); r.shot('demo_3_command_mode.png')
    r.send(':'); r.pump(0.2); r.send('new second'); r.pump(0.3); r.shot('demo_4_command_line.png')
    r.send('\r'); r.pump(0.07); r.shot('demo_5_fade_early.png'); r.pump(0.07); r.shot('demo_6_fade_middle.png'); r.pump(0.3)
    r.send('echo this is the second console; ls --color /usr/share | head -16\n'); r.pump(1.0); r.shot('demo_7_second.png')
    r.send('\x02'); r.pump(0.3); r.send('\x1b[D'); r.pump(0.07); r.shot('demo_8_fade_back_early.png'); r.pump(0.07); r.shot('demo_9_fade_back_mid.png'); r.pump(0.5)
    r.shot('demo_10_back_on_first.png')
    print(r.text())
    r.send('\x1b'); r.pump(0.3)
    r.send('exit\n'); r.pump(1.0)
    r.close()
