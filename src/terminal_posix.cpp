#include "terminal.hpp"

#include <cerrno>
#include <csignal>
#include <chrono>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::string_view reset = "\x1b[0m";
volatile std::sig_atomic_t resize_requested = 0;
volatile std::sig_atomic_t interrupt_requested = 0;

void handle_resize(int) { resize_requested = 1; }
void handle_interrupt(int) { interrupt_requested = 1; }

bool wait_for_stdin(int timeout_ms) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    for (;;) {
        const int result = ::poll(&descriptor, 1, timeout_ms);
        if (result >= 0) return result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0;
        if (errno != EINTR) return false;
        if (resize_requested || interrupt_requested) return false;
    }
}

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
    if (sequence.empty()) return {Type::interrupt, {}};
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

struct TerminalSession::Impl {
    termios original_termios{};
    struct sigaction original_resize{};
    struct sigaction original_interrupt{};
    bool termios_changed{};
    bool resize_handler_changed{};
    bool interrupt_handler_changed{};
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

    resize_requested = 0;
    interrupt_requested = 0;
    std::cout << "\x1b[?1049h\x1b[2J\x1b[H" << std::flush;
    impl_->apply_mouse_tracking(true);
}

TerminalSession::~TerminalSession() {
    impl_->apply_mouse_tracking(false);
    std::cout << reset << "\x1b[?25h\x1b[?1049l" << std::flush;
    if (impl_->termios_changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &impl_->original_termios);
    if (impl_->resize_handler_changed) sigaction(SIGWINCH, &impl_->original_resize, nullptr);
    if (impl_->interrupt_handler_changed) sigaction(SIGINT, &impl_->original_interrupt, nullptr);
}

TerminalSize TerminalSession::size() const {
    winsize dimensions{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &dimensions) == 0 &&
        dimensions.ws_col != 0 && dimensions.ws_row != 0) {
        return {static_cast<int>(dimensions.ws_col), static_cast<int>(dimensions.ws_row)};
    }
    return {};
}

TerminalEvent TerminalSession::read_event() {
    using Type = TerminalEventType;
    for (;;) {
        if (resize_requested) {
            resize_requested = 0;
            return {Type::resize, {}};
        }
        if (interrupt_requested) {
            interrupt_requested = 0;
            return {Type::interrupt, {}};
        }
        if (!wait_for_stdin(-1)) continue;

        unsigned char value{};
        if (!read_byte(value)) {
            if (resize_requested || interrupt_requested) continue;
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

int TerminalSession::read_confirmation() {
    for (;;) {
        const auto event = read_event();
        if (event.type == TerminalEventType::character && !event.text.empty()) {
            return static_cast<unsigned char>(event.text.front());
        }
        if (event.type == TerminalEventType::enter || event.type == TerminalEventType::end_of_input) {
            return '\n';
        }
    }
}

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
        thread = std::thread([this] {
            while (!stop_requested.load(std::memory_order_relaxed)) {
                if (input_paused.load(std::memory_order_acquire)) {
                    pause_acknowledged.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                pause_acknowledged.store(false, std::memory_order_release);
                if (interrupt_requested || cancelled.load(std::memory_order_relaxed)) {
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
                        if (read_available_sequence(20).empty()) {
                            cancelled.store(true, std::memory_order_relaxed);
                            cancel_request();
                            return;
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
