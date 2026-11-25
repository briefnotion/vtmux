// App: what vtmux is, apart from the real terminal. The list of consoles, which is shown, the modes (normal, command, command line), the
// commands, the bar, and the fade between consoles. It is driven by bytes (the keyboard) and by time (a frame), and gives back a Picture to
// show, so it can be tested without a terminal.
#pragma once

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "keys.h"
#include "render.h"
#include "session.h"
#include "transition.h"
#include "vtscreen/vtscreen.h"

namespace vtmux {

enum class Mode {
    Normal,           // the keys go to the console
    Command,          // after the prefix key: arrows switch consoles, : starts a command, Esc goes back
    CommandLine,      // typing a command after the colon
};

struct AppOptions {
    std::vector<std::string> first_command;      // what the first console runs (empty: the shell)
    TransitionStyle style = TransitionStyle::Dip;
    int fade_ms = 250;
    Theme theme;
    char prefix = 0x02;                          // Ctrl-B
};

class App {
public:
    // rows x cols is the whole terminal: the consoles get one row less, the bar has the last.
    App(int rows, int cols, AppOptions options);
    ~App();

    // Open the first console. False, with why, if it could not be started.
    bool start(std::string& error);

    // The bytes the keyboard sent (any amount, split anywhere).
    void handle_input(const char* data, size_t size, double now);
    // Once a frame: settle a lone Escape, remove consoles that ended, move the fade on.
    void tick(double now);
    // The picture to show at this time (the bar drawn in).
    const vtscreen::Picture& frame(double now);

    void resize(int rows, int cols);

    bool done() const { return done_; }
    Mode mode() const { return mode_; }
    int window_count() const { return (int)windows_.size(); }
    int active_index() const { return active_; }
    std::string window_name(int index) const;
    bool transitioning() const { return transition_active_; }
    const std::string& command_line() const { return line_; }
    const std::string& message() const { return message_; }
    // The input modes of the shown console: the real terminal is put in them.
    InputModes input_modes();
    // The title of the shown console, for the real terminal's title.
    std::string title();

    // Run a command line as if typed after the colon (also for tests). Returns the message to show, if any.
    std::string run_command(const std::string& line, double now);

private:
    struct Window {
        int id = 0;
        std::string name;
        std::unique_ptr<Session> session;
        vtscreen::Picture pic;
    };

    bool new_window(const std::vector<std::string>& command, const std::string& name, double now, std::string& error);
    void close_window(int index, double now, bool hang_up);
    void switch_to(int index, double now);
    void settle_transition(double now);
    void command_key(const Key& key, double now);
    void line_key(const Key& key, double now);
    void say(const std::string& text, double now, double seconds = 3.0);
    void draw_bar(vtscreen::Picture& pic, double now);
    void forward(const char* data, size_t size);
    void set_mode(Mode mode);
    void make_pictures(Window& w);
    Window* find(int id);

    int rows_, cols_;
    AppOptions options_;
    std::vector<Window> windows_;
    int active_ = 0;
    int next_id_ = 1;
    bool done_ = false;

    Mode mode_ = Mode::Normal;
    KeyParser keys_;
    std::string line_;
    std::string message_;
    double message_until_ = 0;

    // the fade in progress: from the console `from_id_` (or from a copy of its picture if it has closed) to the active one
    bool transition_active_ = false;
    int from_id_ = 0;
    vtscreen::Picture from_copy_;
    bool from_is_copy_ = false;
    double transition_start_ = 0;
    vtscreen::Picture mixed_;

    std::vector<std::thread> closing_;           // consoles being hung up, in the background
};

}  // namespace vtmux
