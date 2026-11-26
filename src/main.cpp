// vtmux: a terminal multiplexer with fades. See DESIGN.md.
//
//   vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]
//
// With no command the first console runs your shell. Ctrl-B starts command mode (the bar changes colour): the arrow keys switch
// consoles, a colon starts a command (new, close, goto, rename, list, set, quit, help), Esc goes back.
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.h"
#include "vtscreen/vtscreen.h"

using namespace vtmux;

namespace {

void usage() {
    std::printf("vtmux [--transition slide|dip|dissolve|crossfade] [--transition-ms N] [--no-color] [--fps N] [-- command args ...]\n"
                "  Ctrl-B     command mode: arrows switch consoles, a colon starts a command, Esc goes back\n"
                "  commands   new [name] [-- command], close, next, prev, goto N, rename NAME, list, set transition|transition-ms|fps, quit, help\n");
}

// Put the terminal in the modes the console on screen wants (application cursor keys, bracketed paste, the mouse ...), so that the keys it
// sends, which pass straight through to the program, are the ones the program expects. Undone on the way out.
class TerminalModes {
public:
    void apply(const InputModes& want) {
        std::string out;
        if (!known_ || want.application_cursor_keys != have_.application_cursor_keys) out += want.application_cursor_keys ? "\x1b[?1h" : "\x1b[?1l";
        if (!known_ || want.application_keypad != have_.application_keypad) out += want.application_keypad ? "\x1b=" : "\x1b>";
        if (!known_ || want.bracketed_paste != have_.bracketed_paste) out += want.bracketed_paste ? "\x1b[?2004h" : "\x1b[?2004l";
        if (!known_ || want.focus_reporting != have_.focus_reporting) out += want.focus_reporting ? "\x1b[?1004h" : "\x1b[?1004l";
        if (!known_ || want.mouse_tracking != have_.mouse_tracking) {
            out += "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?9l";
            if (want.mouse_tracking != 0) out += "\x1b[?" + std::to_string(want.mouse_tracking) + "h";
        }
        if (!known_ || want.mouse_sgr != have_.mouse_sgr) out += want.mouse_sgr ? "\x1b[?1006h" : "\x1b[?1006l";
        if (!out.empty()) vtscreen::write_all(out);
        have_ = want;
        known_ = true;
    }
    void reset() { apply(InputModes()); }

private:
    InputModes have_;
    bool known_ = false;
};

// Raw keys: no signals (Ctrl-C and Ctrl-Z are for the program in the console), no flow control, no translation of Enter.
void make_keyboard_raw() {
    termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return;
    t.c_lflag &= ~(tcflag_t)(ISIG | IEXTEN | ICANON | ECHO);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR | BRKINT | ISTRIP | INPCK);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

bool terminal_size(int& cols, int& rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return false;
    cols = ws.ws_col;
    rows = ws.ws_row;
    return true;
}

constexpr int MIN_COLS = 20, MIN_ROWS = 4;

int run(int argc, char** argv, std::string& message) {
    AppOptions options;
    vtscreen::Options screen;
    screen.fps = 60;                                       // the most frames a second (--fps changes it)
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--") { for (int k = i + 1; k < argc; k++) options.first_command.push_back(argv[k]); break; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--no-color") { screen.color = false; options.theme.color = false; }
        else if (a == "--fps" && i + 1 < argc) screen.fps = std::min(std::max(std::atoi(argv[++i]), 1), 240);
        else if ((a == "--fade-ms" || a == "--transition-ms") && i + 1 < argc) options.fade_ms = std::min(std::max(std::atoi(argv[++i]), 0), 5000);
        else if ((a == "--fade" || a == "--transition") && i + 1 < argc) {
            if (!parse_style(argv[++i], options.style)) { message = a + ": slide, dip, dissolve or crossfade"; return 2; }
        } else { usage(); return 2; }
    }

    int cols = 80, rows = 24;
    if (!terminal_size(cols, rows)) { cols = 100; rows = 40; }             // (not a terminal: a plain default)
    if (cols < MIN_COLS || rows < MIN_ROWS) { message = "the terminal is too small"; return 1; }

    vtscreen::Display display;
    display.open(screen, cols, rows);
    display.quit_on_q = false;
    make_keyboard_raw();
    TerminalModes terminal_modes;
    std::string title_sent;

    int status = 0;
    {
        App app(rows, cols, options);
        std::string error;
        if (!app.start(error)) { message = "could not start: " + error; status = 1; }
        else {
            const auto begin = std::chrono::steady_clock::now();
            while (!display.stopping() && !app.done()) {
                display.start();                           // the schedule is this frame's: a burst of keys must not push the next frames away
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();

                int c, r;
                if (display.resized(c, r) && c >= MIN_COLS && r >= MIN_ROWS && (c != cols || r != rows)) {
                    cols = c; rows = r;
                    app.resize(rows, cols);
                    display.resize(cols, rows);
                }

                std::string typed;
                for (char k; (k = display.next_key()) != 0;) typed += k;
                if (!typed.empty()) app.handle_input(typed.data(), typed.size(), now);
                app.tick(now);
                if (app.done()) break;
                { const std::string term_out = app.take_terminal_output(); if (!term_out.empty()) vtscreen::write_all(term_out); }      // clipboard writes

                terminal_modes.apply(app.input_modes());
                const std::string title = "vtmux: " + app.title();
                if (title != title_sent) { vtscreen::write_all("\x1b]0;" + title + "\x07"); title_sent = title; }

                display.show(app.frame(now), display.time());
                display.end_frame();
            }
        }
    }                                                       // the consoles are hung up here, before the terminal is put back
    terminal_modes.reset();
    vtscreen::write_all("\x1b]0;\x07");
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    std::string message;
    const int status = run(argc, argv, message);
    if (!message.empty()) std::fprintf(stderr, "vtmux: %s\n", message.c_str());
    return status;
}
