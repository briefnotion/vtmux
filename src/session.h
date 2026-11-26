// Session: one console. A program (the shell, or a command) running on a pseudo-terminal, and the Emulator that keeps the screen
// its output makes. A reader thread feeds the emulator as the program writes; the UI thread writes the keys, resizes, and publishes
// what changed into a Picture. Everything the two threads share is behind one lock, held only for a feed or for a publish.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "emulator.h"
#include "render.h"
#include "vtscreen/vtscreen.h"

namespace vtmux {

// The input modes of the program in a session, which the real terminal must be put in so that it sends keys the way the program
// expects (see DESIGN.md: Input).
struct InputModes {
    bool application_cursor_keys = false;
    bool application_keypad = false;
    bool bracketed_paste = false;
    bool focus_reporting = false;
    int mouse_tracking = 0;
    bool mouse_sgr = false;
    bool operator==(const InputModes& o) const {
        return application_cursor_keys == o.application_cursor_keys && application_keypad == o.application_keypad &&
               bracketed_paste == o.bracketed_paste && focus_reporting == o.focus_reporting &&
               mouse_tracking == o.mouse_tracking && mouse_sgr == o.mouse_sgr;
    }
    bool operator!=(const InputModes& o) const { return !(*this == o); }
};

class Session {
public:
    // rows x cols is the size of the program's screen. command is the program and its arguments; empty means the user's shell.
    Session(int rows, int cols, std::vector<std::string> command = {}, size_t scrollback_lines = 2000);
    ~Session();                                  // hangs the program up, and waits for it and the reader thread
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Start the program. False, with the reason, if it could not be started.
    bool start(std::string& error);

    // ---- from the UI thread
    void write_input(const char* data, size_t size);
    void write_input(const std::string& s) { write_input(s.data(), s.size()); }
    void resize(int rows, int cols);             // the screen, and the program is told (SIGWINCH)

    // Write what changed on the screen into pic (a retained Picture, rows + 1 by cols); see PanePainter. True if any row was written.
    bool publish(vtscreen::Picture& pic, bool show_cursor, const Theme& theme);
    void invalidate() { std::lock_guard<std::mutex> lock(mutex_); painter_.invalidate(); }

    // The view into the history. 0 is the live screen. scroll_by moves it back (positive) or forward (negative) by lines and says
    // where it is now; scroll_reset goes back to live. New output while scrolled back keeps the lines on the screen where they were.
    size_t scroll_by(long lines, size_t* history = nullptr);
    void scroll_reset();
    size_t scroll_back(size_t* history = nullptr);

    InputModes modes();
    std::string title();
    // Clipboard writes (OSC 52) the program made since last asked, as sequences for the real terminal. has_clipboard() is a cheap look.
    bool has_clipboard() const { return clip_flag_.load(); }
    std::vector<std::string> take_clipboard();
    int rows();
    int cols();

    // ---- the program's end
    bool exited() const { return exited_.load(); }
    int exit_status() const { return exit_status_.load(); }     // as waitpid gives it (see WIFEXITED), once exited() is true
    pid_t pid() const { return pid_; }

    // ---- for tests
    template <class F> auto with_emulator(F f) { std::lock_guard<std::mutex> lock(mutex_); return f(emulator_); }

    std::string name;                            // what the bar calls it

private:
    void reader();
    void hang_up();
    void tell_size();

    int rows_, cols_;
    std::vector<std::string> command_;
    pid_t pid_ = -1;
    int master_ = -1;
    int wake_[2] = {-1, -1};                     // a pipe to wake the reader thread when it is to stop
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> exited_{false};
    std::atomic<bool> clip_flag_{false};
    std::atomic<int> exit_status_{0};

    std::mutex mutex_;                           // guards the emulator and the painter
    Emulator emulator_;
    PanePainter painter_;
    size_t scroll_back_ = 0;                     // lines scrolled back (guarded by the mutex)
    int told_rows_ = 0, told_cols_ = 0;          // the size the program was last told (guarded by the mutex)
    size_t last_history_ = 0;                    // the history's size when last published
};

}  // namespace vtmux
