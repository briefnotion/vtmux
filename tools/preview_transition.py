#!/usr/bin/env python3
"""Render frames of vtmux's screen-to-screen fades to PNG, so the three styles can be looked at.

How it works (no terminal is needed):
  1. a small C++ program (its source is embedded below, built into a temp directory) feeds two realistic
     terminal screens to the Emulator (an `ls --color` listing and an editor with a status line and line drawing),
     paints them into two retained Pictures with PanePainter, blends them at several percentages for each style,
     and shows each result through a vtscreen::Display; Display::render returns the bytes it would send to a terminal;
  2. those bytes are decoded by pyte (a terminal model, 24-bit colour included);
  3. the screen is drawn with PIL, DejaVu Sans Mono, 10 x 20 pixel cells, honouring foreground, background, reverse.

Usage: tools/preview_transition.py [--out DIR] [--pct 0,25,50,75,100] [--style crossfade,dissolve,dip] [--cols 100] [--rows 28]
Writes DIR/transition_<style>_<pct>.png and DIR/transition_<style>_sheet.png (all the frames of a style on one sheet).
Needs: g++, python3 with pyte and PIL.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import pyte
from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'
CW, CH = 10, 20
TERM_BG = (12, 12, 12)          # the terminal's own background (what a cell with no background colour shows)
TERM_FG = (204, 204, 204)

CPP = r'''
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "src/emulator.h"
#include "src/render.h"
#include "src/transition.h"
#include "vtscreen/vtscreen.h"
using namespace vtmux;

static std::string at(int row, int col) { return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H"; }

// screen A: a shell, a long listing with colours, and a prompt
static std::string screen_a() {
    std::string s = "\x1b[2J";
    const char* G = "\x1b[1;32m"; const char* B = "\x1b[1;34m"; const char* C = "\x1b[1;36m"; const char* R = "\x1b[1;31m";
    const char* N = "\x1b[0m";
    auto prompt = [&](const std::string& cmd) {
        return std::string(G) + "user@host" + N + ":" + B + "~/source/vtmux" + N + "$ " + cmd + "\r\n";
    };
    s += at(1, 1);
    s += prompt("ls --color -l");
    s += "total 212\r\n";
    struct E { const char* perm; const char* size; const char* date; std::string name; };
    std::vector<E> list = {
        {"drwxr-xr-x", "4096", "Nov 25 09:12", std::string(B) + "build" + N},
        {"-rw-r--r--", "1203", "Nov 24 18:40", "CMakeLists.txt"},
        {"-rw-r--r--", "9211", "Nov 25 08:55", "DESIGN.md"},
        {"drwxr-xr-x", "4096", "Nov 25 09:30", std::string(B) + "docs" + N},
        {"lrwxrwxrwx", "  11", "Nov 20 12:01", std::string(C) + "latest" + N + " -> build/vtmux"},
        {"-rw-r--r--", "5120", "Nov 25 09:02", std::string(R) + "snapshot.tar.gz" + N},
        {"drwxr-xr-x", "4096", "Nov 25 09:21", std::string(B) + "src" + N},
        {"drwxr-xr-x", "4096", "Nov 25 09:21", std::string(B) + "tests" + N},
        {"drwxr-xr-x", "4096", "Nov 22 17:30", std::string(B) + "tools" + N},
        {"-rwxr-xr-x", "88112", "Nov 25 09:12", std::string(G) + "vtmux" + N},
        {"drwxr-xr-x", "4096", "Nov 21 10:15", std::string(B) + "vtscreen" + N},
        {"-rw-r--r--", " 612", "Nov 21 10:15", "LICENSE"},
    };
    for (const E& e : list) s += std::string(e.perm) + " 1 dan dan " + e.size + " " + e.date + " " + e.name + "\r\n";
    s += prompt("git status -s");
    s += " \x1b[31mM\x1b[0m src/render.cpp\r\n";
    s += " \x1b[31mM\x1b[0m src/transition.cpp\r\n";
    s += "\x1b[31m??\x1b[0m docs/\r\n";
    s += prompt("make -j8 2>&1 | tail -3");
    s += "\x1b[1;33mwarning:\x1b[0m unused variable '\x1b[1;35mn\x1b[0m' [-Wunused-variable]\r\n";
    s += "[100%] Built target vtmux\r\n";
    s += std::string(G) + "user@host" + N + ":" + B + "~/source/vtmux" + N + "$ ";
    return s;
}

// screen B: an editor (line numbers, syntax colours, a box drawn with line drawing, a status line, a command line)
static std::string screen_b(int rows, int cols) {
    std::string s = "\x1b[?1049h\x1b[2J";
    const char* KW = "\x1b[38;5;203m"; const char* TY = "\x1b[38;5;81m"; const char* STR = "\x1b[38;5;186m";
    const char* CM = "\x1b[38;5;244m"; const char* FN = "\x1b[38;5;150m"; const char* NUM = "\x1b[38;5;141m";
    const char* LN = "\x1b[38;5;240m"; const char* N = "\x1b[0m";
    struct L { std::string text; };
    std::vector<std::string> code = {
        std::string(CM) + "// the blend of two screens" + N,
        std::string(KW) + "#include" + N + " " + STR + "\"transition.h\"" + N,
        "",
        std::string(KW) + "namespace" + N + " vtmux {",
        "",
        std::string(TY) + "double" + N + " " + FN + "threshold" + N + "(" + TY + "int" + N + " row, " + TY + "int" + N + " col) {",
        "    " + std::string(TY) + "uint32_t" + N + " h = row * " + NUM + "0x9E3779B1u" + N + " ^ col * " + NUM + "0x85EBCA6Bu" + N + ";",
        "    h ^= h >> " + std::string(NUM) + "16" + N + "; h *= " + NUM + "0x85EBCA6Bu" + N + ";",
        "    " + std::string(KW) + "return" + N + " h / " + NUM + "4294967296.0" + N + ";",
        "}",
        "",
        std::string(TY) + "void" + N + " " + FN + "blend" + N + "(" + KW + "const" + N + " Picture& a, " + KW + "const" + N + " Picture& b, " + TY + "double" + N + " pct) {",
        "    " + std::string(KW) + "if" + N + " (pct <= " + NUM + "0" + N + ") { " + CM + "/* the ends are exact */" + N + " }",
        "    " + std::string(KW) + "for" + N + " (" + TY + "int" + N + " r = " + NUM + "0" + N + "; r < body; r++)",
        "        " + std::string(KW) + "for" + N + " (" + TY + "int" + N + " c = " + NUM + "0" + N + "; c < cc; c++)",
        "            out(r, c) = threshold(r, c) < pct ? b(r, c) : a(r, c);",
        "}",
        "",
        "}  " + std::string(CM) + "// namespace vtmux" + N,
    };
    int row = 1;
    for (size_t i = 0; i < code.size() && row <= rows - 2; i++, row++) {
        char num[16];
        std::snprintf(num, sizeof num, "%3zu ", i + 1);
        s += at(row, 1) + LN + num + N + code[i];
    }
    for (; row <= rows - 2; row++) s += at(row, 1) + "\x1b[34m~" + N;
    // a box (DEC line drawing) at the right: a small popup
    const int bx = cols - 38, by = 3, bw = 34, bh = 7;
    s += "\x1b[48;5;236m\x1b[38;5;252m";
    s += at(by, bx) + "\x1b(0l";
    for (int i = 0; i < bw - 2; i++) s += "q";
    s += "k\x1b(B";
    for (int j = 1; j < bh - 1; j++) {
        s += at(by + j, bx) + "\x1b(0x\x1b(B" + std::string(bw - 2, ' ') + "\x1b(0x\x1b(B";
    }
    s += at(by + bh - 1, bx) + "\x1b(0m";
    for (int i = 0; i < bw - 2; i++) s += "q";
    s += "j\x1b(B";
    s += at(by + 1, bx + 2) + "\x1b[1;38;5;220mBuild finished" + "\x1b[22;38;5;252m";
    s += at(by + 3, bx + 2) + "\x1b[38;5;114m" + "6 tests passed" + "\x1b[38;5;252m";
    s += at(by + 4, bx + 2) + "\x1b[38;5;203m" + "0 failed" + "\x1b[38;5;252m";
    s += N;
    // the status line: a blue bar with the file name, then a mode in reverse
    s += at(rows - 1, 1) + "\x1b[1;38;5;231;48;5;24m" + " src/transition.cpp [+] " + "\x1b[0;38;5;250;48;5;238m" + " cpp  utf-8  unix ";
    s += "\x1b[0;48;5;238m" + std::string(cols - 23 - 18 - 12, ' ');
    s += "\x1b[1;38;5;16;48;5;114m" + std::string(" 14:21  Top ");
    s += N + at(rows, 1) + "-- INSERT --";
    s += at(14, 24);                                     // the cursor, in the code
    return s;
}

static void bar(vtscreen::Picture& p, const std::string& text) {
    for (int c = 0; c < p.cols; c++) {
        vtscreen::PicCell k; k.text = true; k.ch = U' '; k.inv = true; k.r = 90; k.g = 160; k.b = 220;
        p.set(p.bar_row(), c, k);
    }
    p.text(p.bar_row(), 1, text, 90, 160, 220, true);
}

static void write_file(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::perror(path.c_str()); std::exit(1); }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::string show(const vtscreen::Picture& p, int rows, int cols) {
    vtscreen::Display d;
    d.init(rows, cols);
    return d.render(p, 0.0);
}

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: prog outdir rows cols pct,pct,...\n"); return 2; }
    const std::string out = argv[1];
    const int rows = std::atoi(argv[2]), cols = std::atoi(argv[3]);
    std::vector<int> pcts;
    for (char* tok = std::strtok(argv[4], ","); tok; tok = std::strtok(nullptr, ",")) pcts.push_back(std::atoi(tok));

    Emulator ea(rows, cols), eb(rows, cols);
    ea.feed(screen_a());
    eb.feed(screen_b(rows, cols));
    vtscreen::Picture pa, pb;
    pa.init(rows, cols); pa.retain();
    pb.init(rows, cols); pb.retain();
    PanePainter painter_a, painter_b;
    Theme theme;
    painter_a.paint(ea, pa, true, theme);
    painter_b.paint(eb, pb, true, theme);
    bar(pa, "[1:bash]  2:vim          vtmux  ^B for commands");
    bar(pb, " 1:bash  [2:vim]         vtmux  ^B for commands");

    for (TransitionStyle st : {TransitionStyle::Crossfade, TransitionStyle::Dissolve, TransitionStyle::Dip}) {
        for (int pct : pcts) {
            vtscreen::Picture res;
            blend(pa, pb, pct / 100.0, st, res);
            write_file(out + "/" + style_name(st) + "_" + std::to_string(pct) + ".bytes", show(res, rows, cols));
        }
    }
    write_file(out + "/screen_a.bytes", show(pa, rows, cols));
    write_file(out + "/screen_b.bytes", show(pb, rows, cols));
    return 0;
}
'''

SRC = ['src/render.cpp', 'src/transition.cpp', 'src/emulator.cpp', 'src/vtparser.cpp', 'src/width.cpp']


def build(tmp):
    cpp = os.path.join(tmp, 'preview_driver.cpp')
    exe = os.path.join(tmp, 'preview_driver')
    with open(cpp, 'w') as f:
        f.write(CPP)
    cmd = ['g++', '-std=c++17', '-O1', '-I', REPO, '-o', exe, cpp] + [os.path.join(REPO, s) for s in SRC]
    subprocess.run(cmd, check=True)
    return exe


def colour(c, default):
    if c == 'default':
        return default
    if len(c) == 6:
        try:
            return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))
        except ValueError:
            pass
    names = {'black': (0, 0, 0), 'red': (205, 49, 49), 'green': (13, 188, 121), 'brown': (229, 229, 16),
             'blue': (36, 114, 200), 'magenta': (188, 63, 188), 'cyan': (17, 168, 205), 'white': (229, 229, 229)}
    return names.get(c, default)


def render(screen, font):
    img = Image.new('RGB', (screen.columns * CW, screen.lines * CH), TERM_BG)
    d = ImageDraw.Draw(img)
    for y in range(screen.lines):
        row = screen.buffer[y]
        for x in range(screen.columns):
            ch = row[x]
            fg = colour(ch.fg, TERM_FG)
            bg = colour(ch.bg, TERM_BG)
            if ch.reverse:
                fg, bg = bg, fg
            if bg != TERM_BG:
                d.rectangle([x * CW, y * CH, x * CW + CW - 1, y * CH + CH - 1], fill=bg)
            if ch.data != ' ':
                d.text((x * CW, y * CH), ch.data, font=font, fill=fg)
    return img


def decode(path, rows, cols):
    screen = pyte.Screen(cols, rows + 1)
    stream = pyte.ByteStream(screen)
    with open(path, 'rb') as f:
        stream.feed(f.read())
    return screen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(REPO, 'docs'))
    ap.add_argument('--pct', default='0,25,50,75,100')
    ap.add_argument('--style', default='crossfade,dissolve,dip')
    ap.add_argument('--rows', type=int, default=28, help='rows of the picture; the bar is one more')
    ap.add_argument('--cols', type=int, default=100)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    pcts = [int(p) for p in args.pct.split(',')]
    styles = args.style.split(',')
    font = ImageFont.truetype(FONT_PATH, 16)
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        subprocess.run([exe, tmp, str(args.rows), str(args.cols), args.pct], check=True)
        for name in ('screen_a', 'screen_b'):
            render(decode(os.path.join(tmp, name + '.bytes'), args.rows, args.cols), font).save(
                os.path.join(args.out, 'transition_%s.png' % name))
        for st in styles:
            frames = []
            for p in pcts:
                img = render(decode(os.path.join(tmp, '%s_%d.bytes' % (st, p)), args.rows, args.cols), font)
                img.save(os.path.join(args.out, 'transition_%s_%d.png' % (st, p)))
                frames.append(img)
            # a sheet: every frame at half size, two to a row
            w, h = frames[0].size
            hw, hh = w // 2, h // 2
            cols = 2
            nrows = (len(frames) + cols - 1) // cols
            sheet = Image.new('RGB', (hw * cols + 6 * (cols + 1), hh * nrows + 6 * (nrows + 1)), (60, 60, 60))
            for i, im in enumerate(frames):
                sheet.paste(im.resize((hw, hh), Image.LANCZOS), (6 + (i % cols) * (hw + 6), 6 + (i // cols) * (hh + 6)))
            sheet.save(os.path.join(args.out, 'transition_%s_sheet.png' % st))
    print('wrote PNGs to', args.out)


if __name__ == '__main__':
    sys.exit(main())
