// sources: ../src/session.cpp ../src/render.cpp ../src/emulator.cpp ../src/vtparser.cpp ../src/width.cpp
// Tests for Session: real programs on a real pseudo-terminal, read back through the emulator.
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "check.h"
#include "../src/session.h"

using namespace vtmux;

namespace {

std::string row_text(Emulator& e, int row) {
    std::string s;
    for (int c = 0; c < e.cols(); c++) {
        const Cell& k = e.cell(row, c);
        if (k.attrs & ATTR_WIDE_TAIL) continue;
        if (k.ch < 0x80) s += (char)k.ch; else s += '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string all_text(Session& s) {
    return s.with_emulator([](Emulator& e) {
        std::string out;
        for (int r = 0; r < e.rows(); r++) out += row_text(e, r) + "\n";
        return out;
    });
}

bool wait_for(const std::function<bool()>& pred, int ms = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

bool contains(Session& s, const std::string& what) { return all_text(s).find(what) != std::string::npos; }

}  // namespace

int main() {
    std::string error;

    // ---- a program that prints and ends: its output is on the screen, and how it ended is known
    {
        Session s(5, 30, {"sh", "-c", "printf hello; exit 3"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return s.exited(); }));
        CHECK(WIFEXITED(s.exit_status()) && WEXITSTATUS(s.exit_status()) == 3);
        CHECK(wait_for([&] { return contains(s, "hello"); }));
    }

    // ---- the environment a program on a terminal expects
    {
        Session s(5, 40, {"sh", "-c", "echo $TERM $COLORTERM $VTMUX; stty size"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return s.exited(); }));
        CHECK(contains(s, "xterm-256color truecolor 1"));
        CHECK(contains(s, "5 40"));                                // the size it was started with
    }

    // ---- input: what is typed reaches the program, and the terminal echoes it
    {
        Session s(6, 30, {"cat"});
        CHECK(s.start(error));
        s.write_input("abc\n");
        CHECK(wait_for([&] { return s.with_emulator([](Emulator& e) { return row_text(e, 0) == "abc" && row_text(e, 1) == "abc"; }); }));
        s.write_input("\x04");                                     // Ctrl-D: end of input, cat ends
        CHECK(wait_for([&] { return s.exited(); }));
        CHECK(WIFEXITED(s.exit_status()) && WEXITSTATUS(s.exit_status()) == 0);
    }

    // ---- the terminal answers the program's questions
    {
        Session s(6, 40, {"sh", "-c", "stty raw -echo; printf '\\033[6n'; head -c 6 | od -An -c"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return s.exited(); }));
        CHECK(contains(s, "033   [   1   ;   1   R"));              // ESC [ 1 ; 1 R: the cursor position, as asked
    }

    // ---- resize: the screen, and the program is told
    {
        Session s(6, 20, {"sh", "-c", "trap 'stty size' WINCH; stty size; while :; do sleep 0.05; done"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return contains(s, "6 20"); }));
        s.resize(9, 33);
        CHECK(s.rows() == 9 && s.cols() == 33);
        CHECK(wait_for([&] { return contains(s, "9 33"); }));
        CHECK(s.with_emulator([](Emulator& e) { return e.rows() == 9 && e.cols() == 33; }));
    }

    // ---- the program switches the screen to 80 columns (DECCOLM, allowed with mode 40): the emulator narrows and the program is told
    {
        Session s(6, 100, {"sh", "-c", "trap 'stty size' WINCH; printf '\\033[?40h\\033[?3l'; stty size; while :; do sleep 0.05; done"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return contains(s, "6 80"); }));
        CHECK(s.with_emulator([](Emulator& e) { return e.cols() == 80; }));
        CHECK_EQ(s.cols(), 100);                                    // the pane is still 100
        s.resize(6, 90);                                            // the pane's own size is back in charge
        CHECK(wait_for([&] { return s.with_emulator([](Emulator& e) { return e.cols() == 90; }); }));
        CHECK(wait_for([&] { return contains(s, "6 90"); }));
    }

    // ---- closing hangs the program up, and leaves nothing behind
    {
        pid_t pid;
        {
            Session s(5, 20, {"sleep", "1000"});
            CHECK(s.start(error));
            pid = s.pid();
            CHECK(kill(pid, 0) == 0);
        }
        CHECK(wait_for([&] { return kill(pid, 0) != 0; }, 2000));  // gone (and collected: a zombie would still answer kill)
    }
    {   // a program that ignores the hangup is killed
        pid_t pid;
        {
            Session s(5, 20, {"sh", "-c", "trap '' HUP; while :; do sleep 0.05; done"});
            CHECK(s.start(error));
            pid = s.pid();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        CHECK(wait_for([&] { return kill(pid, 0) != 0; }, 2000));
    }

    // ---- sessions do not leak their terminals to each other's programs
    {
        Session a(5, 20, {"sleep", "5"});
        CHECK(a.start(error));
        Session b(8, 40, {"sh", "-c", "ls /proc/self/fd | tr '\\n' ' '"});
        CHECK(b.start(error));
        CHECK(wait_for([&] { return b.exited(); }));
        // exactly 0 1 2, and 3 is ls's own look at the directory: any other number is a descriptor that leaked in
        CHECK(wait_for([&] { return b.with_emulator([](Emulator& e) { return row_text(e, 0) == "0 1 2 3"; }); }));
    }

    // ---- a program that is not there
    {
        Session s(5, 40, {"/nonexistent/program"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return s.exited(); }));
        CHECK(WIFEXITED(s.exit_status()) && WEXITSTATUS(s.exit_status()) == 127);
        CHECK(wait_for([&] { return contains(s, "could not run"); }));
    }

    // ---- a lot of output: nothing blocks or is lost, and the scrollback fills
    {
        Session s(10, 30, {"seq", "1", "30000"}, 500);
        CHECK(s.start(error));
        CHECK(wait_for([&] { return s.exited(); }, 8000));
        CHECK(wait_for([&] { return contains(s, "30000"); }));
        CHECK(s.with_emulator([](Emulator& e) { return e.scrollback_size() == 500; }));
        CHECK(s.with_emulator([](Emulator& e) {                      // the bottom lines are the last numbers, in order
            return row_text(e, 8) == "29999" && row_text(e, 9) == "30000" ? true : (row_text(e, 9) == "" && row_text(e, 8) == "30000");
        }));
    }

    // ---- publishing: what is on the screen is in the picture, once, and then nothing is written
    {
        Session s(5, 20, {"sh", "-c", "printf hi; sleep 5"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return contains(s, "hi"); }));
        vtscreen::Picture pic;
        pic.init(5, 20);
        pic.retain();
        Theme theme;
        CHECK(s.publish(pic, true, theme));
        CHECK(pic.at(0, 0).text && pic.at(0, 0).ch == U'h' && pic.at(0, 1).ch == U'i');
        pic.consume();
        CHECK(!s.publish(pic, true, theme));                        // nothing changed
        s.write_input("x");
        CHECK(wait_for([&] { return s.publish(pic, true, theme); }));   // the echo of the key
        s.invalidate();
        CHECK(s.publish(pic, true, theme));                         // a full paint again
    }

    // ---- the input modes a program sets are known
    {
        Session s(5, 30, {"sh", "-c", "printf '\\033[?1h\\033[?2004h\\033[?1006h\\033[?1000h'; sleep 5"});
        CHECK(s.start(error));
        CHECK(wait_for([&] { return s.modes().application_cursor_keys && s.modes().bracketed_paste; }));
        const InputModes m = s.modes();
        CHECK(m.mouse_sgr && m.mouse_tracking == 1000 && !m.focus_reporting);
    }

    if (failures == 0) std::printf("all session tests passed\n");
    return failures == 0 ? 0 : 1;
}
