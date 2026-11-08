#include "terminal.hpp"
#include "terminal_posix_view.hpp"
#include "confirmation_input.hpp"

#include <cerrno>
#include <csignal>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view reset = "\x1b[0m";
volatile std::sig_atomic_t resize_requested = 0;
volatile std::sig_atomic_t interrupt_requested = 0;
// SIGTERM or SIGHUP: the session ends as if input ended. Never cleared, so
// every later read reports end of input until ARN has unwound and exited.
volatile std::sig_atomic_t terminate_requested = 0;
// Self-pipe: a signal may be delivered to any thread, so the handlers also
// write here to wake whichever thread is waiting for input.
int wake_read = -1, wake_write = -1;

void wake() {
    if (wake_write < 0) return;
    const int saved = errno;
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(wake_write, &byte, 1);
    errno = saved;
}

void handle_resize(int) { resize_requested = 1; wake(); }
void handle_interrupt(int) { interrupt_requested = 1; wake(); }
void handle_terminate(int) { terminate_requested = 1; wake(); }

// Returns true when input is readable. A signal returns false so the caller
// checks the flags above.
bool wait_for_stdin(int timeout_ms) {
    pollfd descriptors[2]{{STDIN_FILENO, POLLIN, 0}, {wake_read, POLLIN, 0}};
    const nfds_t count = wake_read >= 0 ? 2 : 1;
    for (;;) {
        const int result = ::poll(descriptors, count, timeout_ms);
        if (result >= 0) {
            if (count == 2 && (descriptors[1].revents & POLLIN) != 0) {
                char drain[64];
                while (::read(wake_read, drain, sizeof drain) > 0) {}
                return false;
            }
            return result > 0 && (descriptors[0].revents & (POLLIN | POLLHUP)) != 0;
        }
        if (errno != EINTR) return false;
        if (resize_requested || interrupt_requested || terminate_requested) return false;
    }
}

bool view_event(arn::TerminalEventType type) {
    using Type = arn::TerminalEventType;
    return type == Type::page_up || type == Type::page_down || type == Type::end ||
           type == Type::wheel_up || type == Type::wheel_down;
}

std::mutex deferred_mutex;
std::vector<arn::TerminalEvent> deferred_view_events;

bool read_byte(unsigned char& value) {
    for (;;) {
        const auto count = ::read(STDIN_FILENO, &value, 1);
        if (count == 1) return true;
        if (count == 0) return false;
        if (errno == EINTR) return false;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    }
}

std::string read_available_sequence(int first_timeout_ms = 20) {
    std::string sequence;
    int timeout = first_timeout_ms;
    while (wait_for_stdin(timeout)) {
        unsigned char value{};
        if (!read_byte(value)) break;
        sequence.push_back(static_cast<char>(value));
        timeout = 2;
        if (sequence.size() >= 64) break;
    }
    return sequence;
}

arn::TerminalEvent decode_escape_sequence(const std::string& sequence) {
    using Type = arn::TerminalEventType;
    if (sequence.empty()) return {Type::escape, {}};
    if (sequence == "OQ" || sequence == "[12~") return {Type::f2, {}};
    if (sequence == "[5~") return {Type::page_up, {}};
    if (sequence == "[6~") return {Type::page_down, {}};
    if (sequence == "[F" || sequence == "OF" || sequence == "[4~" || sequence == "[8~") {
        return {Type::end, {}};
    }
    if (sequence.starts_with("[<64;")) return {Type::wheel_up, {}};
    if (sequence.starts_with("[<65;")) return {Type::wheel_down, {}};
    return {Type::none, {}};
}

std::size_t utf8_length(unsigned char first) {
    if ((first & 0x80) == 0) return 1;
    if ((first & 0xE0) == 0xC0) return 2;
    if ((first & 0xF0) == 0xE0) return 3;
    if ((first & 0xF8) == 0xF0) return 4;
    return 1;
}

} // namespace

namespace arn {

std::vector<TerminalEvent> take_deferred_view_events() {
    std::lock_guard lock(deferred_mutex);
    return std::exchange(deferred_view_events, {});
}

struct TerminalSession::Impl {
    termios original_termios{};
    struct sigaction original_resize{};
    struct sigaction original_interrupt{};
    struct sigaction original_terminate{};
    struct sigaction original_hangup{};
    bool termios_changed{};
    bool resize_handler_changed{};
    bool interrupt_handler_changed{};
    bool terminate_handler_changed{};
    bool hangup_handler_changed{};
    bool copy_mode{};

    void apply_mouse_tracking(bool enabled) {
        // SGR mouse mode reports wheel events without reserving screen lines.
        // Turning it off returns normal text selection to the terminal.
        std::cout << (enabled ? "\x1b[?1000h\x1b[?1006h" : "\x1b[?1000l\x1b[?1006l")
                  << std::flush;
    }
};

TerminalSession::TerminalSession() : impl_(std::make_unique<Impl>()) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &impl_->original_termios) == 0) {
        termios raw = impl_->original_termios;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) impl_->termios_changed = true;
    }

    if (int ends[2]; ::pipe(ends) == 0) {
        for (const int end : ends) {
            ::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
            ::fcntl(end, F_SETFD, FD_CLOEXEC);
        }
        wake_read = ends[0];
        wake_write = ends[1];
    }

    struct sigaction resize_action{};
    resize_action.sa_handler = handle_resize;
    sigemptyset(&resize_action.sa_mask);
    if (sigaction(SIGWINCH, &resize_action, &impl_->original_resize) == 0) {
        impl_->resize_handler_changed = true;
    }

    struct sigaction interrupt_action{};
    interrupt_action.sa_handler = handle_interrupt;
    sigemptyset(&interrupt_action.sa_mask);
    if (sigaction(SIGINT, &interrupt_action, &impl_->original_interrupt) == 0) {
        impl_->interrupt_handler_changed = true;
    }

    // The first SIGTERM or SIGHUP ends the session through the normal exit
    // path (the transcript is printed, the terminal restored). The handler
    // is reset when it runs, so a second signal stops ARN at once.
    struct sigaction terminate_action{};
    terminate_action.sa_handler = handle_terminate;
    terminate_action.sa_flags = static_cast<int>(SA_RESETHAND); // An unsigned constant on Linux.
    sigemptyset(&terminate_action.sa_mask);
    if (sigaction(SIGTERM, &terminate_action, &impl_->original_terminate) == 0) {
        impl_->terminate_handler_changed = true;
    }
    if (sigaction(SIGHUP, &terminate_action, &impl_->original_hangup) == 0) {
        impl_->hangup_handler_changed = true;
    }

    resize_requested = 0;
    interrupt_requested = 0;
    terminate_requested = 0;
    // The renderer (terminal_ui::Ui) owns the alternate screen: it must leave
    // it before printing the conversation, which happens before this session
    // ends.
    impl_->apply_mouse_tracking(true);
}

TerminalSession::~TerminalSession() {
    impl_->apply_mouse_tracking(false);
    std::cout << reset << "\x1b[?25h" << std::flush;
    if (impl_->termios_changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &impl_->original_termios);
    if (impl_->resize_handler_changed) sigaction(SIGWINCH, &impl_->original_resize, nullptr);
    if (impl_->interrupt_handler_changed) sigaction(SIGINT, &impl_->original_interrupt, nullptr);
    if (impl_->terminate_handler_changed) sigaction(SIGTERM, &impl_->original_terminate, nullptr);
    if (impl_->hangup_handler_changed) sigaction(SIGHUP, &impl_->original_hangup, nullptr);
    // The handlers no longer write to the pipe; close it.
    for (int* end : {&wake_read, &wake_write}) {
        if (*end >= 0) ::close(*end);
        *end = -1;
    }
}

TerminalSize TerminalSession::size() const {
    winsize dimensions{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &dimensions) == 0 &&
        dimensions.ws_col != 0 && dimensions.ws_row != 0) {
        return {static_cast<int>(dimensions.ws_col), static_cast<int>(dimensions.ws_row)};
    }
    return {};
}

TerminalEvent TerminalSession::read_event() { return read_event(nullptr); }

TerminalEvent TerminalSession::read_event(const std::atomic_bool* cancelled) {
    using Type = TerminalEventType;
    for (;;) {
        if (terminate_requested) return {Type::end_of_input, {}};
        if (cancelled && cancelled->load()) return {TerminalEventType::interrupt, {}};
        if (resize_requested) {
            resize_requested = 0;
            return {Type::resize, {}};
        }
        if (interrupt_requested) {
            interrupt_requested = 0;
            return {Type::interrupt, {}};
        }
        if (!wait_for_stdin(cancelled ? 25 : -1)) continue;

        unsigned char value{};
        if (!read_byte(value)) {
            if (resize_requested || interrupt_requested || terminate_requested) continue;
            return {Type::end_of_input, {}};
        }

        if (value == 0x1B) return decode_escape_sequence(read_available_sequence());
        if (value == '\r' || value == '\n') return {Type::enter, {}};
        if (value == 0x7F || value == '\b') return {Type::backspace, {}};
        if (value == '\t') return {Type::tab, {}};
        if (value == 3) return {Type::interrupt, {}};
        if (value < 0x20) continue;

        std::string character(1, static_cast<char>(value));
        const auto length = utf8_length(value);
        while (character.size() < length && wait_for_stdin(20)) {
            unsigned char continuation{};
            if (!read_byte(continuation)) break;
            character.push_back(static_cast<char>(continuation));
        }
        return {Type::character, std::move(character)};
    }
}

int TerminalSession::read_confirmation(const std::function<void(std::string_view)>& display) {
    return read_confirmation(display, nullptr);
}

int TerminalSession::read_confirmation(const std::function<void(std::string_view)>& display,
                                       const std::atomic_bool* cancelled) {
    return edit_confirmation([this, cancelled] { return read_event(cancelled); }, display);
}

void TerminalSession::discard_pending_input() {
    interrupt_requested = 0;
    tcflush(STDIN_FILENO, TCIFLUSH);
}

bool TerminalSession::native_scrollback() const noexcept { return false; }

void TerminalSession::set_copy_mode(bool enabled) {
    if (impl_->copy_mode == enabled) return;
    impl_->copy_mode = enabled;
    impl_->apply_mouse_tracking(!enabled);
}

bool TerminalSession::copy_mode() const noexcept { return impl_->copy_mode; }

struct TerminalCancellationMonitor::Impl {
    std::atomic_bool& cancelled;
    std::function<void()> cancel_request;
    std::atomic_bool input_paused{false};
    std::atomic_bool pause_acknowledged{false};
    std::atomic_bool stop_requested{false};
    std::thread thread;

    Impl(std::atomic_bool& flag, std::function<void()> callback)
        : cancelled(flag), cancel_request(std::move(callback)) {
        interrupt_requested = 0;
        (void)take_deferred_view_events(); // Keys from an earlier request are stale.
        thread = std::thread([this] {
            while (!stop_requested.load(std::memory_order_relaxed)) {
                if (input_paused.load(std::memory_order_acquire)) {
                    pause_acknowledged.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                pause_acknowledged.store(false, std::memory_order_release);
                if (interrupt_requested || terminate_requested || cancelled.load(std::memory_order_relaxed)) {
                    interrupt_requested = 0;
                    cancelled.store(true, std::memory_order_relaxed);
                    cancel_request();
                    return;
                }

                if (wait_for_stdin(25)) {
                    unsigned char value{};
                    if (!read_byte(value)) continue;
                    if (value == 3) {
                        cancelled.store(true, std::memory_order_relaxed);
                        cancel_request();
                        return;
                    }
                    if (value == 0x1B) {
                        // Escape sequences from function keys and the mouse begin
                        // with ESC too. Only an ESC with no following bytes cancels.
                        const auto sequence = read_available_sequence(20);
                        if (sequence.empty()) {
                            cancelled.store(true, std::memory_order_relaxed);
                            cancel_request();
                            return;
                        }
                        // Scrolling stays usable while output arrives.
                        if (auto event = decode_escape_sequence(sequence); view_event(event.type)) {
                            std::lock_guard lock(deferred_mutex);
                            if (deferred_view_events.size() < 256) deferred_view_events.push_back(std::move(event));
                        }
                    }
                }
            }
        });
    }

    ~Impl() {
        stop_requested.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }
};

TerminalCancellationMonitor::TerminalCancellationMonitor(
    std::atomic_bool& cancelled, std::function<void()> cancel_request)
    : impl_(std::make_unique<Impl>(cancelled, std::move(cancel_request))) {}

TerminalCancellationMonitor::~TerminalCancellationMonitor() = default;

void TerminalCancellationMonitor::pause_input() {
    impl_->input_paused.store(true, std::memory_order_release);
    while (!impl_->pause_acknowledged.load(std::memory_order_acquire) &&
           !impl_->cancelled.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void TerminalCancellationMonitor::resume_input() {
    impl_->input_paused.store(false, std::memory_order_release);
}

} // namespace arn
