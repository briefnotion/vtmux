// sources: ../src/app.cpp ../src/session.cpp ../src/render.cpp ../src/transition.cpp ../src/keys.cpp ../src/emulator.cpp ../src/vtparser.cpp ../src/width.cpp
// Tests for App: the modes, the commands, the consoles and the fade, driven by bytes and a made-up clock, with real shells behind it.
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "check.h"
#include "../src/app.h"

using namespace vtmux;

namespace {

double g_now = 0;                                          // the made-up clock (seconds)

std::string row_text(const vtscreen::Picture& p, int row) {
    std::string s;
    for (int c = 0; c < p.cols; c++) {
        const vtscreen::PicCell& cell = p.at(row, c);
        s += cell.text && cell.ch < 0x80 ? (char)cell.ch : (cell.text ? '?' : ' ');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string screen_text(const vtscreen::Picture& p) {
    std::string out;
    for (int r = 0; r < p.rows; r++) out += row_text(p, r) + "\n";
    return out;
}

// run frames for up to ms of real time (the clock g_now moves with it), until pred is true
bool pump(App& app, const std::function<bool()>& pred, int ms = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        app.tick(g_now);
        if (!app.done()) app.frame(g_now);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        g_now += 0.01;
    }
    return pred();
}

// the text of the screen without the bar's row (the last)
std::string body_text(const vtscreen::Picture& pic) {
    std::string out;
    for (int r = 0; r < pic.rows - 1; r++) out += row_text(pic, r) + "\n";
    return out;
}

bool shows(App& app, const std::string& what) {
    app.tick(g_now);
    return !app.done() && screen_text(app.frame(g_now)).find(what) != std::string::npos;
}

// wait until the screen shows `what`; if it never does, say what it showed
bool expect_shows(App& app, const std::string& what, int ms = 4000) {
    const bool ok = pump(app, [&] { return shows(app, what); }, ms);
    if (!ok) std::printf("  (waited for \"%s\"; the screen shows:)\n%s", what.c_str(), screen_text(app.frame(g_now)).c_str());
    return ok;
}

void type(App& app, const std::string& bytes) { app.handle_input(bytes.data(), bytes.size(), g_now); }

AppOptions options() {
    AppOptions o;
    o.first_command = {"sh"};
    o.style = TransitionStyle::Dip;                          // (the tests of the fade; the slide has its own below)
    o.fade_ms = 200;
    return o;
}

std::string bar(App& app) { return row_text(app.frame(g_now), 9); }        // the last row of a 10 row screen

}  // namespace

int main() {
    std::string error;

    // ---- the slide (the default): the consoles are side by side in a row and the view slides along it
    {
        CHECK(AppOptions().style == TransitionStyle::Slide);
        AppOptions o = options();
        o.style = TransitionStyle::Slide;
        o.first_command = {"sh", "-c", "printf '%060d' 0 | tr 0 A; sleep 30"};           // a first row of A, of B, of C ... one for each console
        App app(10, 60, o);
        CHECK(app.start(error));
        CHECK(expect_shows(app, std::string(60, 'A')));
        CHECK(app.run_command("new b -- sh -c \"printf '%060d' 0 | tr 0 B; sleep 30\"", g_now).empty());
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'B')));
        CHECK(app.run_command("new c -- sh -c \"printf '%060d' 0 | tr 0 C; sleep 30\"", g_now).empty());
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'C')));
        CHECK_EQ(app.window_count(), 3);
        CHECK_EQ(app.active_index(), 2);

        // 3 to 1: the screens slide right, the one between going past: A, B and C all show at some time, and often two at once
        const double t0 = g_now;
        CHECK(app.run_command("goto 1", t0).empty());
        CHECK_EQ(app.active_index(), 0);
        CHECK(app.transitioning());
        bool saw_a = false, saw_b = false, saw_c = false, two_at_once = false, bar_still = true;
        int mixed_rows_wrong = 0;
        for (int k = 0; k <= 60; k++) {
            const vtscreen::Picture& pic = app.frame(t0 + k * 0.01);
            const std::string row = row_text(pic, 0);
            const bool a = row.find('A') != std::string::npos, b = row.find('B') != std::string::npos, c = row.find('C') != std::string::npos;
            saw_a |= a; saw_b |= b; saw_c |= c;
            if ((a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0) >= 2) two_at_once = true;
            if (row_text(pic, 9).find("1:") == std::string::npos) bar_still = false;                // the bar does not slide
            if (row.size() < 60) mixed_rows_wrong++;
        }
        CHECK(saw_a && saw_b && saw_c);
        CHECK(two_at_once);
        CHECK(bar_still);
        CHECK_EQ(mixed_rows_wrong, 0);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'A')));
        CHECK(body_text(app.frame(g_now)).find('B') == std::string::npos);

        // from the last back to the first it goes all the way back through the others, it does not wrap round
        app.run_command("goto 3", g_now);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        const double t2 = g_now;
        type(app, "\x02\x1b[C");                                                  // Right from the last: the first
        CHECK_EQ(app.active_index(), 0);
        bool saw_b_between = false;
        for (int k = 0; k <= 60; k++)
            if (row_text(app.frame(t2 + k * 0.01), 0).find('B') != std::string::npos) saw_b_between = true;
        CHECK(saw_b_between);                                                      // the second console went by
        type(app, "\x1b");

        CHECK(pump(app, [&] { return !app.transitioning(); }));
    }

    {   // a key while sliding starts again from where the view is, not from the end
        AppOptions o = options();
        o.style = TransitionStyle::Slide;
        o.first_command = {"sh", "-c", "printf '%060d' 0 | tr 0 A; sleep 30"};
        App app(10, 60, o);
        CHECK(app.start(error));
        CHECK(expect_shows(app, std::string(60, 'A')));
        app.run_command("new b -- sh -c \"printf '%060d' 0 | tr 0 B; sleep 30\"", g_now);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'B')));
        const double t0 = g_now;
        app.run_command("goto 1", t0);                                             // B to A
        const std::string halfway = row_text(app.frame(t0 + 0.08), 0);              // part of the way
        const size_t b_at = halfway.find('B');
        CHECK(b_at != std::string::npos && b_at > 5 && halfway.find('A') != std::string::npos);
        app.run_command("goto 2", t0 + 0.08);                                      // and back to B before it got there
        CHECK_EQ(app.active_index(), 1);
        const std::string again = row_text(app.frame(t0 + 0.0801), 0);             // the view has not jumped: nearly the same picture
        const size_t b_again = again.find('B');
        CHECK(b_again != std::string::npos);
        CHECK(b_again + 3 >= b_at && b_again <= b_at + 3);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'B')));
    }

    {   // a new console goes in next to this one, drops in from the top while this one slides off; no cursor until it has landed
        AppOptions o = options();
        o.style = TransitionStyle::Slide;
        App app(10, 60, o);
        CHECK(app.start(error));
        app.run_command("new b", g_now);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        app.run_command("new c", g_now);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK_EQ(app.window_count(), 3);
        CHECK(app.run_command("goto 1", g_now).empty());
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        app.run_command("rename first", g_now);
        const double t0 = g_now;
        app.run_command("new inserted", t0);
        CHECK_EQ(app.window_count(), 4);
        CHECK_EQ(app.active_index(), 1);                                           // next to the first, not at the end
        CHECK_EQ(app.window_name(1), std::string("inserted"));
        CHECK_EQ(app.window_name(0), std::string("first"));
        CHECK(app.transitioning());
        (void)app.frame(t0 + 0.05);                                                // in the middle of it: it draws without trouble
        (void)app.frame(t0 + 0.15);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
    }

    {   // closing: the console drops off the bottom, the one to its left slides in; closing the first brings the next in from the right
        AppOptions o = options();
        o.style = TransitionStyle::Slide;
        o.first_command = {"sh", "-c", "printf '%060d' 0 | tr 0 A; sleep 30"};
        App app(10, 60, o);
        CHECK(app.start(error));
        CHECK(expect_shows(app, std::string(60, 'A')));
        app.run_command("new b -- sh -c \"printf '%060d' 0 | tr 0 B; sleep 30\"", g_now);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'B')));
        const double t0 = g_now;
        app.run_command("close", t0);                                              // B closes: A takes its place, from the left
        CHECK_EQ(app.window_count(), 1);
        CHECK(app.transitioning());
        const std::string early = body_text(app.frame(t0 + 0.02));
        CHECK(early.find('B') != std::string::npos);                               // B is still going (on top, dropping)
        // B has dropped part of the way: the top rows show A (coming in) and the bottom rows B
        bool a_above_b = false;
        for (int k = 3; k <= 12; k++) {
            const vtscreen::Picture& pic = app.frame(t0 + k * 0.01);
            for (int r = 0; r < 9; r++) {
                const std::string row = row_text(pic, r);
                if (row.find('A') != std::string::npos && row.find('B') == std::string::npos) { a_above_b = true; break; }
            }
        }
        CHECK(a_above_b);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(expect_shows(app, std::string(60, 'A')));
        CHECK(body_text(app.frame(g_now)).find('B') == std::string::npos);
    }

    {   // the frame rate counter: off by default; on, it is on the bar
        App app(10, 60, options());
        CHECK(app.start(error));
        CHECK(!app.show_fps());
        for (int k = 0; k < 100; k++) app.frame(k * 0.016);
        CHECK(row_text(app.frame(1.7), 9).find("fps") == std::string::npos);
        app.run_command("set fps on", 1.7);
        for (int k = 0; k < 100; k++) app.frame(2.0 + k * 0.01);                    // 100 frames a second
        CHECK(row_text(app.frame(3.01), 9).find("fps") != std::string::npos);
        CHECK(app.fps() > 80 && app.fps() < 120);
    }

    // ---- the history: arrows and pages in command mode scroll back, Escape or typing comes back live
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        type(app, "i=1; while [ $i -le 60 ]; do echo \"row$((i+1000))x\"; i=$((i+1)); done; echo DO''NE\n");
        CHECK(expect_shows(app, "DONE"));
        CHECK(!shows(app, "row1001x"));                                          // long gone off the top
        type(app, "\x02");
        type(app, "\x1b[5~");                                                    // PageUp
        CHECK(pump(app, [&] { return bar(app).find("SCROLL -") != std::string::npos; }));
        CHECK(!shows(app, "DONE"));                                              // scrolled away from the live screen
        type(app, "\x1b[H");                                                     // Home: as far back as it goes
        CHECK(pump(app, [&] { return shows(app, "row1001x"); }));
        type(app, "\x1b[F");                                                     // End: live again
        CHECK(pump(app, [&] { return shows(app, "DONE"); }));
        CHECK(bar(app).find("SCROLL") == std::string::npos);
        type(app, "\x1b[A");                                                     // Up: one line back
        CHECK(pump(app, [&] { return bar(app).find("SCROLL -1/") != std::string::npos; }));
        type(app, "\x1b");                                                       // Escape: normal mode, and live
        CHECK(pump(app, [&] { return app.mode() == Mode::Normal && bar(app).find("SCROLL") == std::string::npos; }));
        CHECK(shows(app, "DONE"));
    }

    // ---- start: one console, normal mode, the bar names it
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        CHECK_EQ(app.window_count(), 1);
        CHECK(app.mode() == Mode::Normal);
        CHECK(pump(app, [&] { return bar(app).find("1:sh") != std::string::npos; }));
        CHECK(bar(app).find("^B for commands") != std::string::npos);
    }

    // ---- normal mode: the keys go to the program, exactly
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        type(app, "echo $((6*7))marker\n");
        CHECK(expect_shows(app, "42marker"));
        type(app, "printf '%s' \"caf\xC3\xA9\" | od -An -c\n");                    // UTF-8 goes through whole
        CHECK(pump(app, [&] { return shows(app, "303 251"); }));
    }

    // ---- the prefix: command mode, the bar changes; Escape and Enter go back; the prefix twice sends it on
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        type(app, "\x02");
        CHECK(app.mode() == Mode::Command);
        CHECK(bar(app).find("COMMAND") != std::string::npos);
        type(app, "\x1b");                                          // a lone Escape needs a tick to settle
        CHECK(app.mode() == Mode::Command);
        app.tick(g_now);
        CHECK(app.mode() == Mode::Normal);
        type(app, "\x02\r");
        CHECK(app.mode() == Mode::Normal);
        type(app, "cat -v\n");
        pump(app, [&] { return shows(app, "cat -v"); });
        type(app, "\x02\x02\n");                                    // Ctrl-B Ctrl-B: a Ctrl-B for the program
        CHECK(pump(app, [&] { return shows(app, "^B"); }));
        type(app, "\x04");
    }
    {   // the prefix at the end of one chunk and the key in the next
        App app(10, 60, options());
        CHECK(app.start(error));
        type(app, "echo a\n\x02");
        CHECK(app.mode() == Mode::Command);
        type(app, ":");
        CHECK(app.mode() == Mode::CommandLine);
        type(app, "\x1b");
        app.tick(g_now);
        CHECK(app.mode() == Mode::Command);                         // Escape in the command line goes back to command mode
    }

    // ---- the command line
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        type(app, "\x02:new two");
        CHECK(app.mode() == Mode::CommandLine && app.command_line() == "new two");
        CHECK(row_text(app.frame(g_now), 9).find(":new two_") == 0);
        type(app, "\x7f\x7f");                                       // backspace edits
        CHECK(app.command_line() == "new t");
        type(app, "\x15");                                           // Ctrl-U clears
        CHECK(app.command_line().empty());
        type(app, "new second\r");
        CHECK(app.mode() == Mode::Normal);
        CHECK_EQ(app.window_count(), 2);
        CHECK_EQ(app.active_index(), 1);
        CHECK_EQ(app.window_name(1), std::string("second"));
        CHECK(app.transitioning());
    }

    // ---- the fade: in progress it is a mix of the two, and it ends after fade_ms
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        type(app, "echo ON$((1+0))\n");
        CHECK(expect_shows(app, "ON1"));
        type(app, "\x02:new two\r");
        type(app, "echo TW$((1+1))\n");
        CHECK(app.transitioning());
        const double start = g_now;
        const std::string early = screen_text(app.frame(start + 0.02));        // just begun: still the first console
        CHECK(early.find("ON1") != std::string::npos);
        CHECK(pump(app, [&] { return !app.transitioning(); }));
        CHECK(g_now - start >= 0.19);                                          // it took (about) fade_ms
        CHECK(expect_shows(app, "TW2"));
        CHECK(screen_text(app.frame(g_now)).find("ON1") == std::string::npos);        // and only the new console is shown
    }
    {   // fade-ms 0: no fade at all
        App app(10, 60, options());
        CHECK(app.start(error));
        CHECK(app.run_command("set fade-ms 0", g_now) == "transition-ms: 0");
        CHECK(app.run_command("new b", g_now).empty());
        CHECK(!app.transitioning());
    }

    // ---- flipping with the arrows: it stays in command mode, wraps round, and each key starts a fade
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        app.run_command("new b", g_now);
        app.run_command("new c", g_now);
        CHECK_EQ(app.window_count(), 3);
        CHECK_EQ(app.active_index(), 2);
        type(app, "\x02\x1b[C");                                     // Right: wraps to the first
        CHECK_EQ(app.active_index(), 0);
        CHECK(app.mode() == Mode::Command);
        type(app, "\x1b[C\x1b[C");
        CHECK_EQ(app.active_index(), 2);
        type(app, "\x1b[D");
        CHECK_EQ(app.active_index(), 1);
        type(app, "\x1bOD");                                          // the other form of the key
        CHECK_EQ(app.active_index(), 0);
        type(app, "\x1b[D");                                          // Left from the first wraps to the last
        CHECK_EQ(app.active_index(), 2);
        CHECK(app.mode() == Mode::Command);
        type(app, "\r");
        CHECK(app.mode() == Mode::Normal);
        // a number goes to that console and leaves command mode
        type(app, "\x02" "2");
        CHECK_EQ(app.active_index(), 1);
        CHECK(app.mode() == Mode::Normal);
        type(app, "\x02" "9");                                        // there is no 9
        CHECK(app.mode() == Mode::Command);
        CHECK(app.message().find("no console 9") != std::string::npos);
        type(app, "\r");
    }

    // ---- the other commands
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        CHECK(app.run_command("new w2 -- sh", g_now).empty());
        CHECK(app.run_command("new -- cat", g_now).empty());
        CHECK_EQ(app.window_name(2), std::string("cat"));
        CHECK(app.run_command("rename mine", g_now).empty());
        CHECK_EQ(app.window_name(2), std::string("mine"));
        CHECK_EQ(app.run_command("list", g_now), std::string("1:sh  2:w2  3:mine"));
        CHECK(app.run_command("goto 1", g_now).empty());
        CHECK_EQ(app.active_index(), 0);
        CHECK(app.run_command("next", g_now).empty());
        CHECK_EQ(app.active_index(), 1);
        CHECK(app.run_command("prev", g_now).empty());
        CHECK_EQ(app.active_index(), 0);
        CHECK(app.run_command("goto 7", g_now).find("no console") == 0);
        CHECK(app.run_command("goto", g_now).find("goto:") == 0);
        CHECK(app.run_command("bogus", g_now).find("unknown command: bogus") == 0);
        CHECK(app.run_command("new x y", g_now).find("new: use") == 0);
        CHECK(app.run_command("new x --", g_now).find("new: nothing after") == 0);
        CHECK(app.run_command("new bad -- /nonexistent/prog", g_now).empty());      // it starts, then ends with an error message of its own
        CHECK(app.run_command("set fade dissolve", g_now) == "transition: dissolve");          // (fade is the older name)
        CHECK(app.run_command("set transition dip", g_now) == "transition: dip");
        CHECK(app.run_command("set transition slide", g_now) == "transition: slide");
        CHECK(app.run_command("set transition nonsense", g_now).find("transition:") == 0);
        CHECK(app.run_command("set transition-ms 9999", g_now).find("0 to 5000") != std::string::npos);
        CHECK(app.run_command("set fps on", g_now) == "fps: on");
        CHECK(app.show_fps());
        CHECK(app.run_command("set fps off", g_now) == "fps: off");
        CHECK(!app.show_fps());
        CHECK(app.run_command("set fps maybe", g_now).find("fps:") == 0);
        CHECK(app.run_command("set transition-ms 250", g_now) == "transition-ms: 250");
        CHECK(app.run_command("help", g_now).find("new") == 0);
        CHECK(app.run_command("", g_now).empty());
        CHECK(app.run_command("   ", g_now).empty());
        CHECK(app.run_command("new \"two words\"", g_now).empty());                  // quotes keep a name together
        CHECK_EQ(app.window_name(app.active_index()), std::string("two words"));
    }

    // ---- closing: the one on screen goes, its left neighbour shows; the last one closing ends the program
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        app.run_command("new b", g_now);
        app.run_command("new c", g_now);
        CHECK_EQ(app.active_index(), 2);
        CHECK(app.run_command("close", g_now).empty());
        CHECK_EQ(app.window_count(), 2);
        CHECK_EQ(app.active_index(), 1);
        app.run_command("goto 1", g_now);
        app.run_command("close", g_now);
        CHECK_EQ(app.window_count(), 1);
        CHECK_EQ(app.active_index(), 0);
        CHECK(!app.done());
        app.run_command("close", g_now);
        CHECK(app.done());
    }
    {   // quit
        App app(10, 60, options());
        CHECK(app.start(error));
        app.run_command("new b", g_now);
        CHECK(app.run_command("quit", g_now).empty());
        CHECK(app.done());
    }

    // ---- a console whose program ends closes by itself
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        app.run_command("new b", g_now);
        type(app, "exit\n");                                          // the shell in the active console ends
        CHECK(pump(app, [&] { return app.window_count() == 1; }));
        CHECK(!app.done());
        CHECK_EQ(app.window_name(0), std::string("sh"));
        type(app, "exit\n");
        CHECK(pump(app, [&] { return app.done(); }));
    }

    // ---- resize: every console is told, and the pictures are the new size
    {
        App app(10, 60, options());
        CHECK(app.start(error));
        app.run_command("new b", g_now);
        pump(app, [&] { return !app.transitioning(); });
        app.resize(14, 33);
        type(app, "stty size\n");
        CHECK(pump(app, [&] { return shows(app, "13 33"); }));
        const vtscreen::Picture& p = app.frame(g_now);
        CHECK(p.rows == 14 && p.cols == 33);
        CHECK(!app.transitioning());
        app.run_command("goto 1", g_now);
        pump(app, [&] { return !app.transitioning(); });
        type(app, "stty size\n");
        CHECK(pump(app, [&] { return shows(app, "13 33"); }));
    }

    // ---- the bar fits any width and never writes outside the picture
    for (int cols : {1, 2, 5, 12, 30, 80}) {
        App app(5, cols, options());
        CHECK(app.start(error));
        for (int i = 0; i < 6; i++) app.run_command("new window_with_a_long_name_" + std::to_string(i), g_now);
        type(app, "\x02");
        const vtscreen::Picture& p = app.frame(g_now);
        CHECK(p.cols == cols && p.rows == 5);
        CHECK((int)row_text(p, 4).size() <= cols * 4);                // (the arrows are 3-byte characters in the text, but one cell each)
        type(app, ":new abc");
        app.frame(g_now);
        type(app, "\x1b\x1b");
        app.tick(g_now);
    }

    // ---- the input modes of the shown console are known
    {
        App app(10, 60, {{"sh", "-c", "printf '\\033[?1h\\033[?2004h'; sleep 5"}, TransitionStyle::Dip, 200, Theme(), 0x02});
        CHECK(app.start(error));
        CHECK(pump(app, [&] { return app.input_modes().application_cursor_keys && app.input_modes().bracketed_paste; }));
    }

    if (failures == 0) std::printf("all app tests passed\n");
    return failures == 0 ? 0 : 1;
}
