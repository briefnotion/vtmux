#include "session.h"

#include <algorithm>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace vtmux {

namespace {

void set_cloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// write all of it (the pty may take it in pieces); give up on a real error
void write_fully(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {                               // the program is not reading: wait a moment, rather than spin
                pollfd p{fd, POLLOUT, 0};
                ::poll(&p, 1, 50);
                continue;
            }
            return;
        }
        data += n;
        size -= (size_t)n;
    }
}

// In the child: close every descriptor above 2. Whatever the launcher left open (pipes, files, other consoles' terminals) is nothing
// the program being run should hold on to.
void close_inherited_descriptors() {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) limit = 65536;
    for (int fd = 3; fd < limit; fd++) ::close(fd);
}

}  // namespace

Session::Session(int rows, int cols, std::vector<std::string> command, size_t scrollback_lines)
    : rows_(rows), cols_(cols), command_(std::move(command)), emulator_(rows, cols, scrollback_lines) {}

Session::~Session() {
    hang_up();
}

bool Session::start(std::string& error) {
    if (pipe(wake_) != 0) { error = std::string("pipe: ") + std::strerror(errno); return false; }
    set_cloexec(wake_[0]);
    set_cloexec(wake_[1]);

    winsize ws{};
    ws.ws_row = (unsigned short)rows_;
    ws.ws_col = (unsigned short)cols_;
    pid_ = forkpty(&master_, nullptr, nullptr, &ws);
    if (pid_ < 0) { error = std::string("forkpty: ") + std::strerror(errno); return false; }

    if (pid_ == 0) {
        // the child: a clean environment for a program on a terminal, then the program
        close_inherited_descriptors();                           // (this includes the master side of the terminal)
        signal(SIGPIPE, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("VTMUX", "1", 1);
        unsetenv("LINES");
        unsetenv("COLUMNS");
        if (command_.empty()) {
            const char* shell = std::getenv("SHELL");
            if (!shell || !*shell) shell = "/bin/sh";
            execlp(shell, shell, (char*)nullptr);
        } else {
            std::vector<char*> argv;
            for (std::string& a : command_) argv.push_back(a.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
        }
        const char* msg = "vtmux: could not run the program\r\n";
        if (::write(STDERR_FILENO, msg, std::strlen(msg)) < 0) {}
        _exit(127);
    }

    set_cloexec(master_);                                        // other sessions' programs must not inherit this one's pty
    thread_ = std::thread([this] { reader(); });
    return true;
}

void Session::reader() {
    char buf[65536];
    for (;;) {
        pollfd fds[2] = {{master_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        const int n = ::poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) break;                      // told to stop
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t got = ::read(master_, buf, sizeof buf);
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (got <= 0) break;                                 // EOF or EIO: the program closed its terminal (it ended)
            std::string reply;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                emulator_.feed(buf, (size_t)got);
                reply = emulator_.take_reply();
            }
            if (!reply.empty()) write_fully(master_, reply.data(), reply.size());
        }
    }
    if (!stopping_.load()) {                                     // it ended by itself: collect how
        int status = 0;
        for (int i = 0; i < 200; i++) {                          // (it may take a moment to be a zombie)
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_ || (r < 0 && errno != EINTR)) break;
            ::usleep(5000);
        }
        exit_status_ = status;
        exited_ = true;
    }
}

void Session::hang_up() {
    if (pid_ < 0) return;
    stopping_ = true;
    if (wake_[1] >= 0) { const char c = 0; if (::write(wake_[1], &c, 1) < 0) {} }
    if (thread_.joinable()) thread_.join();
    if (master_ >= 0) { ::close(master_); master_ = -1; }        // the kernel hangs up the program's terminal
    int status = 0;
    bool gone = false;
    for (int i = 0; i < 60 && !gone; i++) {                      // up to about 300 ms to leave on its own
        if (i == 0) { ::kill(pid_, SIGHUP); ::kill(pid_, SIGCONT); }
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || (r < 0 && errno != EINTR)) gone = true;
        else ::usleep(5000);
    }
    if (!gone) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &status, 0);
    }
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
    wake_[0] = wake_[1] = -1;
    pid_ = -1;
}

void Session::write_input(const char* data, size_t size) {
    if (master_ >= 0 && !exited_.load()) write_fully(master_, data, size);
}

void Session::resize(int rows, int cols) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_ = rows; cols_ = cols;
        emulator_.resize(rows, cols);
        painter_.invalidate();
    }
    if (master_ >= 0) {
        winsize ws{};
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        ::ioctl(master_, TIOCSWINSZ, &ws);                       // the program gets SIGWINCH
    }
}

bool Session::publish(vtscreen::Picture& pic, bool show_cursor, const Theme& theme) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t history = emulator_.scrollback_size();
    if (scroll_back_ > 0 && history > last_history_) scroll_back_ += history - last_history_;       // the lines on the screen stay put
    last_history_ = history;
    scroll_back_ = std::min(scroll_back_, history);
    return painter_.paint(emulator_, pic, show_cursor && scroll_back_ == 0, theme, scroll_back_) > 0;
}

size_t Session::scroll_by(long lines, size_t* history) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t total = emulator_.scrollback_size();
    long now = (long)scroll_back_ + lines;
    now = std::max(0L, std::min(now, (long)total));
    scroll_back_ = (size_t)now;
    if (history) *history = total;
    return scroll_back_;
}

void Session::scroll_reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    scroll_back_ = 0;
}

size_t Session::scroll_back(size_t* history) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history) *history = emulator_.scrollback_size();
    return scroll_back_;
}

InputModes Session::modes() {
    std::lock_guard<std::mutex> lock(mutex_);
    InputModes m;
    m.application_cursor_keys = emulator_.application_cursor_keys();
    m.application_keypad = emulator_.application_keypad();
    m.bracketed_paste = emulator_.bracketed_paste();
    m.focus_reporting = emulator_.focus_reporting();
    m.mouse_tracking = emulator_.mouse_tracking();
    m.mouse_sgr = emulator_.mouse_sgr();
    return m;
}

std::string Session::title() {
    std::lock_guard<std::mutex> lock(mutex_);
    return emulator_.title();
}

int Session::rows() { std::lock_guard<std::mutex> lock(mutex_); return rows_; }
int Session::cols() { std::lock_guard<std::mutex> lock(mutex_); return cols_; }

}  // namespace vtmux
