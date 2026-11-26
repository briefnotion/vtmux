#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace vtmux {

namespace {

// UTF-8 helpers: how many characters, and the first n of them (one cell each is assumed on the bar)
size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string utf8_cut(const std::string& s, size_t n) {
    size_t seen = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) == 0x80) continue;
        if (seen == n) return s.substr(0, i);
        seen++;
    }
    return s;
}

void append_utf8(std::string& s, char32_t cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

// words of a command line, with "double quotes" keeping spaces together
std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> words;
    std::string cur;
    bool in_quote = false, have = false;
    for (char c : line) {
        if (c == '"') { in_quote = !in_quote; have = true; }
        else if ((c == ' ' || c == '\t') && !in_quote) { if (have) { words.push_back(cur); cur.clear(); have = false; } }
        else { cur += c; have = true; }
    }
    if (have) words.push_back(cur);
    return words;
}

// Opening and closing a console are two steps (the old screen goes, then the new one comes); the whole takes this many times what one step does.
constexpr double TWO_STEP_FACTOR = 1.5;

std::string base_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ set up

App::App(int rows, int cols, AppOptions options) : rows_(rows), cols_(cols), options_(std::move(options)) {
    mixed_.init(std::max(rows - 1, 1), cols);
}

App::~App() {
    windows_.clear();                            // hangs each program up
    for (std::thread& t : closing_) if (t.joinable()) t.join();
}

void App::make_pictures(Window& w) {
    w.pic.init(std::max(rows_ - 1, 1), cols_);
    w.pic.retain();
}

bool App::start(std::string& error) {
    return new_window(options_.first_command, "", 0.0, error);
}

App::Window* App::find(int id) {
    for (Window& w : windows_) if (w.id == id) return &w;
    return nullptr;
}

std::string App::window_name(int index) const {
    if (index < 0 || index >= (int)windows_.size()) return "";
    return windows_[(size_t)index].name;
}

// ------------------------------------------------------------------------------------------------ windows

bool App::new_window(const std::vector<std::string>& command, const std::string& name, double now, std::string& error) {
    Window w;
    w.id = next_id_++;
    w.session = std::make_unique<Session>(std::max(rows_ - 1, 1), cols_, command, 2000);
    if (!w.session->start(error)) return false;
    if (!name.empty()) w.name = name;
    else if (!command.empty()) w.name = base_name(command[0]);
    else {
        const char* shell = std::getenv("SHELL");
        w.name = base_name(shell && *shell ? shell : "sh");
    }
    w.session->name = w.name;
    make_pictures(w);
    if (windows_.empty()) {
        windows_.push_back(std::move(w));
        active_ = 0;
        return true;
    }
    // it goes in next to the one shown (on its right) and is shown
    cancel_transition();
    const int old_id = windows_[(size_t)active_].id;
    windows_.insert(windows_.begin() + active_ + 1, std::move(w));
    active_++;
    from_id_ = old_id;
    from_is_copy_ = false;
    kind_ = options_.style == TransitionStyle::Slide ? Kind::Open : Kind::Blend;
    duration_ms_ = kind_ == Kind::Open ? options_.fade_ms * TWO_STEP_FACTOR : options_.fade_ms;
    transition_active_ = options_.fade_ms > 0;
    transition_start_ = now;
    return true;
}

// Whatever is in progress ends where it was going.
void App::cancel_transition() {
    transition_active_ = false;
    from_is_copy_ = false;
}

// The empty space left while one screen has gone and the next has not come: the void colour (a blank cell if colour is off or the void is off).
vtscreen::PicCell App::void_cell_() const {
    vtscreen::PicCell c;
    c.text = true;
    c.ch = U' ';
    if (options_.theme.color && options_.void_on) {
        c.alt_lum = 1;
        c.alt_r = options_.void_color[0]; c.alt_g = options_.void_color[1]; c.alt_b = options_.void_color[2];
    }
    return c;
}

// Where the view of the row of consoles is now, in consoles (a Strip transition).
double App::strip_position(double now) const {
    const double span = std::max(duration_ms_, 1.0);
    const double t = (now - transition_start_) * 1000.0 / span;
    return strip_from_ + ((double)active_ - strip_from_) * ease_out(t);
}

void App::switch_to(int index, double now) {
    if (index < 0 || index >= (int)windows_.size() || index == active_) return;
    if (options_.style == TransitionStyle::Slide) {
        // slide along the row of consoles; if a slide is under way the new one starts from where the view is now
        const double from = (transition_active_ && kind_ == Kind::Strip) ? strip_position(now) : (double)active_;
        active_ = index;
        kind_ = Kind::Strip;
        strip_from_ = from;
        duration_ms_ = slide_ms(std::fabs((double)index - from), options_.fade_ms);
        transition_active_ = options_.fade_ms > 0;
        transition_start_ = now;
        from_is_copy_ = false;
        return;
    }
    // the one on screen now is where the fade starts from (if a fade is under way, this snaps it: the new fade starts from where that one was going)
    from_id_ = windows_[(size_t)active_].id;
    from_is_copy_ = false;
    active_ = index;
    kind_ = Kind::Blend;
    duration_ms_ = options_.fade_ms;
    transition_active_ = options_.fade_ms > 0;
    transition_start_ = now;
}

void App::close_window(int index, double now, bool hang_up) {
    if (index < 0 || index >= (int)windows_.size()) return;
    const bool was_active = index == active_;
    Window gone = std::move(windows_[(size_t)index]);
    windows_.erase(windows_.begin() + index);
    if (windows_.empty()) { done_ = true; closing_.emplace_back([s = std::move(gone.session)]() mutable { s.reset(); }); return; }
    if (index < active_) active_--;
    if (transition_active_ && !was_active) cancel_transition();   // the row of consoles changed under it
    if (was_active) {
        active_ = index > 0 ? index - 1 : 0;                     // the console to its left (or the first)
        // the console that closed goes away (a fade, or it drops off the screen): its last picture is kept as a copy
        from_copy_ = gone.pic;
        from_is_copy_ = true;
        from_id_ = 0;
        if (options_.style == TransitionStyle::Slide) {
            kind_ = Kind::Close;
            close_from_right_ = index == 0;                      // the one that takes its place was on the right only if it was the first that closed
        } else {
            kind_ = Kind::Blend;
        }
        duration_ms_ = kind_ == Kind::Close ? options_.fade_ms * TWO_STEP_FACTOR : options_.fade_ms;
        transition_active_ = options_.fade_ms > 0;
        transition_start_ = now;
    }
    (void)hang_up;
    closing_.emplace_back([s = std::move(gone.session)]() mutable { s.reset(); });      // the hang up waits for the program: not on the UI thread
}

void App::settle_transition(double now) {
    if (!transition_active_) return;
    if ((now - transition_start_) * 1000.0 >= duration_ms_) cancel_transition();
}

// ------------------------------------------------------------------------------------------------ input

void App::set_mode(Mode mode) {
    mode_ = mode;
    if (mode != Mode::CommandLine) line_.clear();
}

void App::say(const std::string& text, double now, double seconds) {
    message_ = text;
    message_until_ = now + seconds;
}

void App::forward(const char* data, size_t size) {
    if (size == 0 || windows_.empty()) return;
    windows_[(size_t)active_].session->scroll_reset();          // typing brings the live screen back
    windows_[(size_t)active_].session->write_input(data, size);
}

void App::handle_input(const char* data, size_t size, double now) {
    if (done_ || windows_.empty()) return;
    if (mode_ == Mode::Normal) {
        size_t start = 0;
        for (size_t i = 0; i < size; i++) {
            if ((unsigned char)data[i] != (unsigned char)options_.prefix) continue;
            forward(data + start, i - start);                    // what came before the prefix goes to the console
            set_mode(Mode::Command);
            keys_.feed(data + i + 1, size - i - 1);              // and what follows is for command mode
            Key key;
            while (keys_.next(key)) {
                dispatch_key(key, now);    // command mode ended in the middle of this: the rest is normal input again
            }
            return;
        }
        forward(data + start, size - start);
        return;
    }
    keys_.feed(data, size);
    Key key;
    while (keys_.next(key)) {
        dispatch_key(key, now);
    }
}

void App::tick(double now) {
    keys_.flush();
    Key key;
    while (keys_.next(key)) {                                    // a lone Escape settles here
        dispatch_key(key, now);
    }
    for (int i = (int)windows_.size() - 1; i >= 0; i--)          // consoles whose program ended close
        if (windows_[(size_t)i].session->exited()) close_window(i, now, false);
    settle_transition(now);
    if (!message_.empty() && now >= message_until_) message_.clear();
}

void App::dispatch_key(const Key& key, double now) {
    if (mode_ == Mode::Command) command_key(key, now);
    else if (mode_ == Mode::CommandLine) line_key(key, now);
    else if (mode_ == Mode::Help) help_key(key, now);
    else forward(key.raw.data(), key.raw.size());
}

void App::command_key(const Key& key, double now) {
    const int count = (int)windows_.size();
    switch (key.kind) {
        case KeyKind::Left: switch_to((active_ + count - 1) % count, now); return;          // stays in command mode: flip through them
        case KeyKind::Right: switch_to((active_ + 1) % count, now); return;
        case KeyKind::Escape: case KeyKind::Enter: windows_[(size_t)active_].session->scroll_reset(); set_mode(Mode::Normal); return;
        case KeyKind::Up: windows_[(size_t)active_].session->scroll_by(1); return;                // the history
        case KeyKind::Down: windows_[(size_t)active_].session->scroll_by(-1); return;
        case KeyKind::PageUp: windows_[(size_t)active_].session->scroll_by(std::max(1, rows_ - 2)); return;
        case KeyKind::PageDown: windows_[(size_t)active_].session->scroll_by(-std::max(1, rows_ - 2)); return;
        case KeyKind::Home: windows_[(size_t)active_].session->scroll_by(1L << 30); return;
        case KeyKind::End: windows_[(size_t)active_].session->scroll_reset(); return;
        case KeyKind::Char:
            if (key.ch == (char32_t)(unsigned char)options_.prefix) {                       // the prefix again: send it on
                const char c = options_.prefix;
                forward(&c, 1);
                set_mode(Mode::Normal);
            } else if (key.ch == U':') {
                set_mode(Mode::CommandLine);
            } else if (key.ch >= U'1' && key.ch <= U'9') {
                const int n = (int)(key.ch - U'0') - 1;
                if (n < count) { switch_to(n, now); set_mode(Mode::Normal); }
                else say("no console " + std::to_string(n + 1), now);
            } else if (key.ch == U'c') {
                set_mode(Mode::Normal);
                say(run_command("new", now), now);
            } else if (key.ch == U'n') {
                switch_to((active_ + 1) % count, now);
            } else if (key.ch == U'p') {
                switch_to((active_ + count - 1) % count, now);
            }
            return;
        default: return;                                                                     // other keys do nothing here
    }
}

void App::line_key(const Key& key, double now) {
    switch (key.kind) {
        case KeyKind::Escape: set_mode(Mode::Command); return;
        case KeyKind::Enter: {
            const std::string line = line_;
            set_mode(Mode::Normal);
            const std::string reply = run_command(line, now);
            if (!reply.empty()) say(reply, now);
            return;
        }
        case KeyKind::Backspace: {
            if (line_.empty()) { set_mode(Mode::Command); return; }
            // drop the last character (not a byte)
            size_t i = line_.size() - 1;
            while (i > 0 && ((unsigned char)line_[i] & 0xC0) == 0x80) i--;
            line_.erase(i);
            return;
        }
        case KeyKind::Char:
            if (key.ch == 0x15) { line_.clear(); return; }                                   // Ctrl-U
            if (key.ch >= 0x20 && key.ch != 0x7F) append_utf8(line_, key.ch);
            return;
        case KeyKind::Paste:
            for (char c : key.text) if (c != '\n' && c != '\r') line_ += c;
            return;
        default: return;
    }
}

// ------------------------------------------------------------------------------------------------ commands

std::string App::run_command(const std::string& line, double now) {
    const std::vector<std::string> w = split_words(line);
    if (w.empty()) return "";
    const std::string& cmd = w[0];
    std::string error;
    if (cmd == "new" || cmd == "window") {
        std::vector<std::string> command;
        std::string name;
        size_t i = 1;
        if (i < w.size() && w[i] != "--") name = w[i++];
        if (i < w.size() && w[i] == "--") {
            i++;
            command.assign(w.begin() + (long)i, w.end());
            if (command.empty()) return "new: nothing after --";
        } else if (i < w.size()) {
            return "new: use  new [name] [-- command ...]";
        }
        if (!new_window(command, name, now, error)) return "new: " + error;
        return "";
    }
    if (cmd == "close" || cmd == "kill") {
        close_window(active_, now, true);
        return "";
    }
    if (cmd == "next") { if (windows_.size() > 1) switch_to((active_ + 1) % (int)windows_.size(), now); return ""; }
    if (cmd == "prev" || cmd == "previous") { if (windows_.size() > 1) switch_to((active_ + (int)windows_.size() - 1) % (int)windows_.size(), now); return ""; }
    if (cmd == "goto" || cmd == "select" || cmd == "go") {
        if (w.size() < 2) return "goto: which console? (a number)";
        const int n = std::atoi(w[1].c_str());
        if (n < 1 || n > (int)windows_.size()) return "no console " + w[1];
        switch_to(n - 1, now);
        return "";
    }
    if (cmd == "rename") {
        if (w.size() < 2) return "rename: to what?";
        windows_[(size_t)active_].name = w[1];
        windows_[(size_t)active_].session->name = w[1];
        return "";
    }
    if (cmd == "list" || cmd == "ls") {
        std::ostringstream out;
        for (size_t i = 0; i < windows_.size(); i++) out << (i ? "  " : "") << (i + 1) << ":" << windows_[i].name;
        return out.str();
    }
    if (cmd == "set") {
        if (w.size() >= 3 && (w[1] == "transition" || w[1] == "fade")) {          // (fade is the older name)
            TransitionStyle style;
            if (!parse_style(w[2], style)) return "transition: slide, dip, dissolve or crossfade";
            cancel_transition();
            options_.style = style;
            return "transition: " + w[2];
        }
        if (w.size() >= 3 && (w[1] == "transition-ms" || w[1] == "fade-ms")) {
            const int ms = std::atoi(w[2].c_str());
            if (ms < 0 || ms > 5000) return "transition-ms: 0 to 5000";
            options_.fade_ms = ms;
            return "transition-ms: " + std::to_string(ms);
        }
        if (w.size() >= 3 && w[1] == "void-color") {
            if (w[2] == "off") { options_.void_on = false; return "void-color: off"; }
            std::string hex = w[2][0] == '#' ? w[2].substr(1) : w[2];
            if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return "void-color: RRGGBB (hex) or off";
            for (int i = 0; i < 3; i++) options_.void_color[i] = (int)std::strtol(hex.substr((size_t)i * 2, 2).c_str(), nullptr, 16);
            options_.void_on = true;
            return "void-color: #" + hex;
        }
        if (w.size() >= 3 && w[1] == "clipboard") {
            if (w[2] != "on" && w[2] != "off") return "clipboard: on or off";
            clipboard_on_ = w[2] == "on";
            return "clipboard: " + w[2];
        }
        if (w.size() >= 3 && w[1] == "fps") {
            if (w[2] != "on" && w[2] != "off") return "fps: on or off";
            show_fps_ = w[2] == "on";
            return "fps: " + w[2];
        }
        return "set: transition slide|dip|dissolve|crossfade   transition-ms N   void-color RRGGBB|off   fps on|off   clipboard on|off";
    }
    if (cmd == "quit" || cmd == "q" || cmd == "exit") { done_ = true; return ""; }
    if (cmd == "help") { help_scroll_ = 0; set_mode(Mode::Help); return ""; }
    return "unknown command: " + cmd + "  (help)";
}

// ------------------------------------------------------------------------------------------------ the help card

namespace {

struct HelpItem { int kind; const char* key; const char* text; };       // kind: 0 a blank line, 1 a heading, 2 a key (or command) and what it does

const HelpItem HELP[] = {
    {1, "", "In normal mode every key goes to the console, except:"},
    {2, "Ctrl-B", "Command mode: the bar turns amber and the keys below work"},
    {0, "", ""},
    {1, "", "In command mode"},
    {2, "\xE2\x86\x90 \xE2\x86\x92", "The previous or next console. Stay in command mode to keep flipping; from the last, Right goes all the way back to the first"},
    {2, "1 to 9", "Go to that console"},
    {2, "c  n  p", "A new console, the next, the previous"},
    {2, "\xE2\x86\x91 \xE2\x86\x93 PgUp PgDn Home", "Look back through the console's history; End or Esc comes back"},
    {2, ":", "Type a command (see below)"},
    {2, "Esc  Enter", "Back to normal mode"},
    {2, "Ctrl-B", "Send a real Ctrl-B to the program"},
    {0, "", ""},
    {1, "", "Commands, typed after the colon"},
    {2, "new [name] [-- cmd]", "A new console next to this one, running your shell or the command"},
    {2, "close", "Close this console (the last one closing leaves vtmux)"},
    {2, "next   prev", "Switch to the next or previous console"},
    {2, "goto N", "Switch to console N"},
    {2, "rename NAME", "Name this console on the bar"},
    {2, "list", "Show the consoles on the bar for a moment"},
    {2, "set transition STYLE", "How switching looks: slide, dip, dissolve or crossfade"},
    {2, "set transition-ms N", "How long one step takes, 0 to 5000 (0 switches at once)"},
    {2, "set void-color RRGGBB", "The colour of the empty space while a console is opening or closing (or off)"},
    {2, "set fps on|off", "Show the frame rate on the bar"},
    {2, "set clipboard on|off", "Let programs (micro, vim ...) copy to your clipboard with OSC 52 (on by default)"},
    {2, "help", "This card"},
    {2, "quit", "Close everything and leave"},
};

// UTF-8 text as code points (a cell each is assumed)
std::u32string to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : (n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07));
        for (int k = 1; k < n && i + (size_t)k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 0x3F);
        out += cp;
        i += (size_t)n;
    }
    return out;
}

// the words of text broken into lines of at most `width` cells (a word longer than that is cut)
std::vector<std::string> wrap_words(const std::string& text, int width) {
    std::vector<std::string> lines;
    std::string cur;
    size_t cur_len = 0;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        while ((int)utf8_len(word) > width && width > 0) {                 // too long for a line by itself
            if (!cur.empty()) { lines.push_back(cur); cur.clear(); cur_len = 0; }
            lines.push_back(utf8_cut(word, (size_t)width));
            word = word.substr(utf8_cut(word, (size_t)width).size());
        }
        const size_t wl = utf8_len(word);
        if (!cur.empty() && cur_len + 1 + wl > (size_t)width) { lines.push_back(cur); cur.clear(); cur_len = 0; }
        if (!cur.empty()) { cur += ' '; cur_len++; }
        cur += word;
        cur_len += wl;
    }
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    return lines;
}

struct HelpLine {
    std::string key;                             // drawn at the left, in the key colour
    std::string text;
    int text_col = 0;
    bool heading = false;
};

// The help as lines of a card `width` cells wide: two columns (the key, then what it does, wrapped) if there is room, otherwise the key on a line
// of its own and what it does indented under it.
std::vector<HelpLine> help_lines(int width) {
    std::vector<HelpLine> lines;
    const int key_col = 24;
    const bool two_columns = width >= key_col + 20;
    for (const HelpItem& item : HELP) {
        if (item.kind == 0) { lines.push_back(HelpLine()); continue; }
        if (item.kind == 1) {
            for (const std::string& t : wrap_words(item.text, width)) { HelpLine l; l.text = t; l.heading = true; lines.push_back(l); }
            continue;
        }
        const int indent = two_columns ? key_col : 2;
        const std::vector<std::string> parts = wrap_words(item.text, std::max(width - indent, 1));
        if (two_columns) {
            for (size_t i = 0; i < parts.size(); i++) {
                HelpLine l;
                if (i == 0) l.key = item.key;
                l.text = parts[i];
                l.text_col = indent;
                lines.push_back(l);
            }
        } else {
            HelpLine k; k.key = item.key; lines.push_back(k);
            for (const std::string& t : parts) { HelpLine l; l.text = t; l.text_col = indent; lines.push_back(l); }
        }
    }
    return lines;
}

// Where the card is on a body of rows x cols: about two thirds of it each way (not under a small size, not wider than 90).
struct CardBox { int left, top, width, height; };
CardBox card_box(int body_rows, int cols) {
    CardBox b;
    b.height = std::min(body_rows, std::max(body_rows * 2 / 3, 12));
    b.width = std::min(cols, std::min(90, std::max(cols * 2 / 3, 44)));
    b.left = (cols - b.width) / 2;
    b.top = (body_rows - b.height) / 2;
    return b;
}

}  // namespace

int App::help_max_scroll() const {
    const CardBox box = card_box(std::max(rows_ - 1, 1), cols_);
    if (box.width < 8 || box.height < 3) return 0;
    const int inner_rows = box.height - 2;
    return std::max(0, (int)help_lines(box.width - 4).size() - inner_rows);
}

void App::help_key(const Key& key, double now) {
    (void)now;
    const int page = std::max(1, card_box(std::max(rows_ - 1, 1), cols_).height - 3);
    const int last = help_max_scroll();
    switch (key.kind) {
        case KeyKind::Escape: case KeyKind::Enter: set_mode(Mode::Normal); return;
        case KeyKind::Char:
            if (key.ch == U' ' || key.ch == U'q') set_mode(Mode::Normal);
            return;
        case KeyKind::Up: help_scroll_ -= 1; break;
        case KeyKind::Down: help_scroll_ += 1; break;
        case KeyKind::PageUp: help_scroll_ -= page; break;
        case KeyKind::PageDown: help_scroll_ += page; break;
        case KeyKind::Home: help_scroll_ = 0; break;
        case KeyKind::End: help_scroll_ = last; break;
        default: return;
    }
    help_scroll_ = std::max(0, std::min(help_scroll_, last));
}

// `out` becomes `base` with the help card drawn over it (about two thirds of the screen, in the middle; the console shows round it).
void App::draw_help(const vtscreen::Picture& base, vtscreen::Picture& out) {
    using vtscreen::PicCell;
    out.retained = false;
    out.rows = base.rows; out.cols = base.cols;
    out.cell = base.cell;
    out.touch_all();
    const int body = base.rows - 1;
    const CardBox box = card_box(std::max(body, 1), base.cols);
    if (box.width < 8 || box.height < 3 || body < 3) return;                // too small for a card: the bar says what to do

    const bool color = options_.theme.color;
    const int text_w = box.width - 4, inner_rows = box.height - 2;
    const std::vector<HelpLine> lines = help_lines(text_w);
    const int last = std::max(0, (int)lines.size() - inner_rows);
    help_scroll_ = std::max(0, std::min(help_scroll_, last));

    auto put = [&](int row, int col, char32_t ch, const int fg[3]) {
        if (row < 0 || row >= body || col < 0 || col >= base.cols) return;
        PicCell p;
        p.text = true;
        p.ch = ch;
        if (color) {
            p.r = fg[0]; p.g = fg[1]; p.b = fg[2];
            p.alt_lum = 1; p.alt_r = 22; p.alt_g = 26; p.alt_b = 38;       // the card's own background
        }
        out.cell[(size_t)row * base.cols + (size_t)col] = p;
    };
    auto put_text = [&](int row, int col, const std::string& s, const int fg[3], int limit) {
        int n = 0;
        for (char32_t ch : to_u32(s)) { if (n >= limit) break; put(row, col + n, ch, fg); n++; }
    };

    const int amber[3] = {255, 165, 40}, white[3] = {240, 240, 240}, grey[3] = {204, 204, 204}, blue[3] = {90, 160, 220};
    const int r0 = box.top, c0 = box.left, r1 = box.top + box.height - 1, c1 = box.left + box.width - 1;
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            char32_t ch = U' ';
            if (r == r0 || r == r1) ch = c == c0 ? (r == r0 ? U'┌' : U'└') : c == c1 ? (r == r0 ? U'┐' : U'┘') : U'─';
            else if (c == c0 || c == c1) ch = U'│';
            put(r, c, ch, amber);
        }
    put_text(r0, c0 + 2, " vtmux help ", white, box.width - 4);
    for (int i = 0; i < inner_rows; i++) {
        const size_t at = (size_t)(help_scroll_ + i);
        if (at >= lines.size()) break;
        const HelpLine& l = lines[at];
        const int row = r0 + 1 + i, col = c0 + 2;
        if (l.heading) put_text(row, col, l.text, blue, text_w);
        else {
            if (!l.key.empty()) put_text(row, col, l.key, white, text_w);
            if (!l.text.empty()) put_text(row, col + l.text_col, l.text, grey, text_w - l.text_col);
        }
    }
    std::string foot = " Enter or Space to close ";
    if (last > 0) {
        foot = " \xE2\x86\x91\xE2\x86\x93 scroll  " + std::to_string(help_scroll_ + 1) + "-" + std::to_string(std::min((int)lines.size(), help_scroll_ + inner_rows)) +
               " of " + std::to_string(lines.size()) + "  Enter or Space to close ";
    }
    const std::u32string f = to_u32(foot);
    const int room = box.width - 4;
    const int start = c0 + 2;
    int n = 0;
    for (char32_t ch : f) { if (n >= room) break; put(r1, start + n, ch, amber); n++; }
}

// ------------------------------------------------------------------------------------------------ drawing

void App::draw_bar(vtscreen::Picture& pic, double now) {
    const int row = pic.bar_row();
    const int width = pic.cols;
    int accent[3] = {90, 160, 220};                                        // normal: blue
    if (mode_ != Mode::Normal) { accent[0] = 255; accent[1] = 165; accent[2] = 40; }       // command: amber
    if (!message_.empty() && now < message_until_ && mode_ == Mode::Normal) { accent[0] = 90; accent[1] = 200; accent[2] = 120; }      // a message: green
    pic.text(row, 0, std::string((size_t)width, ' '), accent[0], accent[1], accent[2], true);

    if (mode_ == Mode::CommandLine) {
        std::string text = ":" + line_ + "_";
        if (!message_.empty() && now < message_until_) text += "    " + message_;
        pic.text(row, 0, utf8_cut(text, (size_t)width), accent[0], accent[1], accent[2], true);
        return;
    }
    // the consoles, with the one shown standing out
    int col = 0;
    for (size_t i = 0; i < windows_.size() && col < width; i++) {
        const bool active = (int)i == active_;
        const std::string seg = " " + std::to_string(i + 1) + ":" + windows_[i].name + " ";
        const std::string cut = utf8_cut(seg, (size_t)(width - col));
        if (active) pic.text(row, col, cut, 240, 240, 240, true);
        else pic.text(row, col, cut, accent[0], accent[1], accent[2], true);
        col += (int)utf8_len(cut);
    }
    std::string right;
    size_t history = 0;
    const size_t back = windows_.empty() ? 0 : windows_[(size_t)active_].session->scroll_back(&history);
    if (mode_ == Mode::Help) right = "HELP  \xE2\x86\x91\xE2\x86\x93 scroll   Enter or Space: close";
    else if (mode_ == Mode::Command && back > 0) right = "SCROLL -" + std::to_string(back) + "/" + std::to_string(history) + "  \xE2\x86\x91\xE2\x86\x93 PgUp PgDn Home End   Esc live";
    else if (mode_ == Mode::Command) right = "COMMAND  \xE2\x86\x90 \xE2\x86\x92 switch  \xE2\x86\x91 history  : command  1-9 go  Esc back";
    else if (!message_.empty() && now < message_until_) right = message_;
    else right = "vtmux  ^B for commands";
    if (show_fps_) right = std::to_string((int)std::lround(fps_)) + " fps  " + right;
    const int room = width - col - 1;
    if (room > 4) {
        const std::string cut = utf8_cut(right, (size_t)room);
        const int at = width - (int)utf8_len(cut) - 1;
        pic.text(row, std::max(at, col), cut, accent[0], accent[1], accent[2], true);
    }
}

const vtscreen::Picture& App::frame(double now) {
    if (windows_.empty()) return mixed_;

    // the frame rate, measured over about half a second
    // (the frame that starts a window is not counted in it: n gaps between frames are n frames after the first)
    if (fps_since_ < 0 || now < fps_since_) { fps_since_ = now; fps_frames_ = 0; }
    else {
        fps_frames_++;
        if (now - fps_since_ >= 0.5) { fps_ = fps_frames_ / (now - fps_since_); fps_since_ = now; fps_frames_ = 0; }
    }

    const vtscreen::Picture& base = frame_base(now);
    if (mode_ != Mode::Help) return base;
    draw_help(base, card_pic_);                  // the console goes on underneath: it was published into `base` just now
    return card_pic_;
}

const vtscreen::Picture& App::frame_base(double now) {
    Window& act = windows_[(size_t)active_];
    const bool moving = transition_active_ && kind_ != Kind::Blend;        // (while things slide, no cursor is drawn)
    act.session->publish(act.pic, !moving, options_.theme);
    draw_bar(act.pic, now);
    if (!transition_active_) return act.pic;

    const double pct = std::min(1.0, std::max(0.0, (now - transition_start_) * 1000.0 / std::max(duration_ms_, 1.0)));

    if (kind_ == Kind::Strip) {
        const double p = strip_position(now);
        const int lo = std::max(0, (int)std::floor(p)), hi = std::min((int)windows_.size() - 1, (int)std::ceil(p));
        std::vector<Layer> layers;
        for (int i = lo; i <= hi; i++) {
            Window& w = windows_[(size_t)i];
            if (i != active_) w.session->publish(w.pic, false, options_.theme);
            layers.push_back({&w.pic, (int)std::lround(((double)i - p) * act.pic.cols), 0});
        }
        compose(layers, act.pic, mixed_);
        return mixed_;
    }
    const int body = act.pic.rows - 1;
    // Opening and closing are two steps, one after the other, with the void between: the first half is the old screen leaving, the second
    // the new one arriving.
    const bool leaving = pct < 0.5;
    const double leave = ease_in(std::min(1.0, pct * 2.0));                   // 0 to 1 over the first half: speeding up as it goes
    const double arrive = ease_out(std::max(0.0, pct * 2.0 - 1.0));           // 0 to 1 over the second half: slowing to a stop
    const vtscreen::PicCell void_cell = void_cell_();
    if (kind_ == Kind::Open) {                       // the old console slides off to the left, then the new one drops in from the top
        if (leaving) {
            Window* old = find(from_id_);
            if (!old) return act.pic;
            old->session->publish(old->pic, false, options_.theme);
            compose({{&old->pic, -(int)std::lround(leave * act.pic.cols), 0}}, act.pic, mixed_, void_cell);
        } else {
            compose({{&act.pic, 0, -(int)std::lround((1.0 - arrive) * body)}}, act.pic, mixed_, void_cell);
        }
        return mixed_;
    }
    if (kind_ == Kind::Close) {                      // the console that closed drops off the bottom, then the one that replaces it slides in
        if (leaving) {
            compose({{&from_copy_, 0, (int)std::lround(leave * body)}}, act.pic, mixed_, void_cell);
        } else {
            const int side = close_from_right_ ? 1 : -1;
            compose({{&act.pic, side * (int)std::lround((1.0 - arrive) * act.pic.cols), 0}}, act.pic, mixed_, void_cell);
        }
        return mixed_;
    }
    const vtscreen::Picture* from = nullptr;
    if (from_is_copy_) from = &from_copy_;
    else if (Window* f = find(from_id_)) {
        f->session->publish(f->pic, false, options_.theme);
        from = &f->pic;
    }
    if (!from || from->rows != act.pic.rows || from->cols != act.pic.cols) return act.pic;
    blend(*from, act.pic, pct, options_.style, mixed_);
    return mixed_;
}

void App::resize(int rows, int cols) {
    rows_ = std::max(rows, 2);
    cols_ = std::max(cols, 1);
    cancel_transition();
    for (Window& w : windows_) {
        w.session->resize(rows_ - 1, cols_);
        make_pictures(w);
        w.session->invalidate();
    }
    mixed_.init(rows_ - 1, cols_);
}

InputModes App::input_modes() {
    if (windows_.empty()) return InputModes();
    return windows_[(size_t)active_].session->modes();
}

std::string App::take_terminal_output() {
    std::string out;
    for (size_t i = 0; i < windows_.size(); i++) {
        Session& s = *windows_[i].session;
        if (!s.has_clipboard()) continue;
        const std::vector<std::string> writes = s.take_clipboard();
        if (clipboard_on_ && (int)i == active_)
            for (const std::string& w : writes) out += w;
    }
    return out;
}

std::string App::title() {
    if (windows_.empty()) return "";
    const std::string t = windows_[(size_t)active_].session->title();
    return t.empty() ? windows_[(size_t)active_].name : t;
}

}  // namespace vtmux
