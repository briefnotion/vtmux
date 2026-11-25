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
    o.fade_ms = 200;
    return o;
}

std::string bar(App& app) { return row_text(app.frame(g_now), 9); }        // the last row of a 10 row screen

}  // namespace

int main() {
    std::string error;

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
        CHECK(app.run_command("set fade-ms 0", g_now) == "fade-ms: 0");
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
        CHECK(app.run_command("set fade dissolve", g_now) == "fade: dissolve");
        CHECK(app.run_command("set fade nonsense", g_now).find("fade:") == 0);
        CHECK(app.run_command("set fade-ms 9999", g_now).find("0 to 5000") != std::string::npos);
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
