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
    Help,             // the help card is over the console; arrows scroll it, Enter or Space closes it
};

// What the real terminal vtmux runs in is known to be able to do (found out at start-up): the size of one of its cells in pixels, and whether
// it can draw Sixel pictures. 0 and false when it is not known or not so; everything then works as it did before pictures.
struct OuterTerminal {
    int cell_w = 0, cell_h = 0;
    bool sixel = false;
};

struct AppOptions {
    std::vector<std::string> first_command;      // what the first console runs (empty: the shell)
    TransitionStyle style = TransitionStyle::Slide;
    int fade_ms = 1000;                          // what one step of a transition takes (the fade, or one console's slide)
    Theme theme;
    int void_color[3] = {16, 18, 30};            // the empty space while a console is opening or closing
    bool void_on = true;
    char prefix = 0x02;                          // Ctrl-B
    OuterTerminal outer = OuterTerminal();       // what the real terminal can do (last, with an initializer, so the earlier ones can still be listed in order)
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
    bool show_fps() const { return show_fps_; }
    int help_scroll() const { return help_scroll_; }              // the first line of the help card shown
    double fps() const { return fps_; }                           // frames a second, measured
    const std::string& command_line() const { return line_; }
    const std::string& message() const { return message_; }
    // The input modes of the shown console: the real terminal is put in them.
    InputModes input_modes();
    // The title of the shown console, for the real terminal's title.
    std::string title();
    // Bytes for the real terminal that are not the picture: the clipboard writes (OSC 52) of the shown console (a console in the background does
    // not get to set your clipboard; its writes are dropped). Call once a frame; write what it returns.
    std::string take_terminal_output();
    bool clipboard_on() const { return clipboard_on_; }

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
    void help_key(const Key& key, double now);
    void dispatch_key(const Key& key, double now);               // to whatever mode it is
    const vtscreen::Picture& frame_base(double now);             // what is shown without the help card
    void draw_help(const vtscreen::Picture& base, vtscreen::Picture& out);
    int help_max_scroll() const;
    void say(const std::string& text, double now, double seconds = 3.0);
    void draw_bar(vtscreen::Picture& pic, double now);
    void forward(const char* data, size_t size);
    void set_mode(Mode mode);
    void make_pictures(Window& w);
    Window* find(int id);
    double strip_position(double now) const;
    void cancel_transition();
    vtscreen::PicCell void_cell_() const;

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

    // What the transition in progress is:
    //   Blend: a fade (crossfade, dissolve, dip) from the console `from_id_` (or from a copy of its picture) to the active one
    //   Strip: the consoles are side by side in a row, in order, and the view slides along it from `strip_from_` (a position in consoles,
    //          not a whole number if a slide was started again part of the way) to the active one
    //   Open:  two steps: the console it came from (`from_id_`) slides off to the left, then the new one drops in from the top
    //   Close: two steps: the console that closed (its copy) drops off the bottom, then the one that takes its place slides in from `close_from_right_`'s side
    enum class Kind { Blend, Strip, Open, Close };
    Kind kind_ = Kind::Blend;
    double strip_from_ = 0;
    bool close_from_right_ = false;
    double duration_ms_ = 0;
    bool transition_active_ = false;
    int from_id_ = 0;
    vtscreen::Picture from_copy_;
    bool from_is_copy_ = false;
    double transition_start_ = 0;
    vtscreen::Picture mixed_;

    int help_scroll_ = 0;
    vtscreen::Picture card_pic_;                 // the screen with the help card over it
    bool show_fps_ = false;                      // the frame rate on the bar (set fps on)
    bool clipboard_on_ = true;                   // programs may set the clipboard (set clipboard on|off)
    double fps_ = 0;
    int fps_frames_ = 0;
    double fps_since_ = -1;

    std::vector<std::thread> closing_;           // consoles being hung up, in the background
};

}  // namespace vtmux
