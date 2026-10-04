#include "terminal.hpp"
#include "confirmation_input.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view reset = "\x1b[0m";
std::atomic<std::atomic_bool*> active_cancel_flag = nullptr;
std::atomic_bool interrupt_pending = false;
HANDLE interrupt_event = nullptr;

BOOL WINAPI handle_console_control(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (auto* flag = active_cancel_flag.load(std::memory_order_acquire)) {
        flag->store(true, std::memory_order_relaxed);
    }
    interrupt_pending.store(true, std::memory_order_release);
    if (interrupt_event) SetEvent(interrupt_event);
    // Never terminate ARN at the prompt. During a request the monitor below
    // converts Ctrl+C into a clean HTTP cancellation.
    return TRUE;
}

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

} // namespace

namespace arn {

struct TerminalSession::Impl {
    HANDLE output{INVALID_HANDLE_VALUE};
    HANDLE input{INVALID_HANDLE_VALUE};
    DWORD output_mode{};
    DWORD input_mode{};
    UINT output_code_page{};
    UINT input_code_page{};
    bool output_mode_changed{};
    bool input_mode_changed{};
    bool copy_mode{};
    wchar_t pending_high_surrogate{};

    void apply_input_mode() {
        if (!input_mode_changed) return;
        // Use the normal screen buffer and let the host own wheel/selection.
        // Quick Edit supplies selection in legacy conhost too. Keep processed
        // Ctrl+C so Windows Terminal can consume it when a selection exists.
        DWORD mode = input_mode | ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE |
                     ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT;
        mode &= ~(ENABLE_MOUSE_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                  ENABLE_VIRTUAL_TERMINAL_INPUT);
        SetConsoleMode(input, mode);
        copy_mode = false;
    }
};

TerminalSession::TerminalSession() : impl_(std::make_unique<Impl>()) {
    impl_->output_code_page = GetConsoleOutputCP();
    impl_->input_code_page = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    interrupt_pending.store(false);
    interrupt_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SetConsoleCtrlHandler(handle_console_control, TRUE);

    impl_->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (impl_->output != INVALID_HANDLE_VALUE && GetConsoleMode(impl_->output, &impl_->output_mode)) {
        impl_->output_mode_changed = true;
        SetConsoleMode(impl_->output, impl_->output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    impl_->input = GetStdHandle(STD_INPUT_HANDLE);
    if (impl_->input != INVALID_HANDLE_VALUE && GetConsoleMode(impl_->input, &impl_->input_mode)) {
        impl_->input_mode_changed = true;
        impl_->apply_input_mode();
    }

    std::cout << "\x1b[?1000l\x1b[?1006l" << std::flush;
}

TerminalSession::~TerminalSession() {
    std::cout << reset << "\x1b[?25h\r\n" << std::flush;
    if (impl_->output_mode_changed) SetConsoleMode(impl_->output, impl_->output_mode);
    if (impl_->input_mode_changed) SetConsoleMode(impl_->input, impl_->input_mode);
    if (impl_->output_code_page != 0) SetConsoleOutputCP(impl_->output_code_page);
    if (impl_->input_code_page != 0) SetConsoleCP(impl_->input_code_page);
    SetConsoleCtrlHandler(handle_console_control, FALSE);
    if (interrupt_event) CloseHandle(interrupt_event);
    interrupt_event = nullptr;
}

TerminalSize TerminalSession::size() const {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (impl_->output != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(impl_->output, &info)) {
        return {info.srWindow.Right - info.srWindow.Left + 1,
                info.srWindow.Bottom - info.srWindow.Top + 1};
    }
    return {};
}

TerminalEvent TerminalSession::read_event() { return read_event(nullptr); }

TerminalEvent TerminalSession::read_event(const std::atomic_bool* cancelled) {
    for (;;) {
        if (cancelled && cancelled->load()) return {TerminalEventType::interrupt, {}};
        if (interrupt_pending.exchange(false)) return {TerminalEventType::interrupt, {}};
        if (impl_->input == INVALID_HANDLE_VALUE || !interrupt_event)
            return {TerminalEventType::end_of_input, {}};
        const HANDLE handles[]{impl_->input, interrupt_event};
        const DWORD ready = WaitForMultipleObjects(2, handles, FALSE, cancelled ? 25 : INFINITE);
        if (ready == WAIT_TIMEOUT) continue;
        if (ready == WAIT_OBJECT_0 + 1) continue;
        if (ready != WAIT_OBJECT_0) return {TerminalEventType::end_of_input, {}};
        INPUT_RECORD event{};
        DWORD count{};
        if (impl_->input == INVALID_HANDLE_VALUE ||
            !ReadConsoleInputW(impl_->input, &event, 1, &count)) {
            return {TerminalEventType::end_of_input, {}};
        }
        if (count == 0) continue;

        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            return {TerminalEventType::resize, {}};
        }
        if (event.EventType == MOUSE_EVENT) continue;
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;

        const auto& key = event.Event.KeyEvent;
        if (key.wVirtualKeyCode == VK_RETURN) return {TerminalEventType::enter, {}};
        if (key.wVirtualKeyCode == 'C' &&
            (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
            return {TerminalEventType::interrupt, {}};
        switch (key.wVirtualKeyCode) {
        case VK_F2: continue;
        case VK_PRIOR: return {TerminalEventType::page_up, {}};
        case VK_NEXT: return {TerminalEventType::page_down, {}};
        case VK_END: return {TerminalEventType::end, {}};
        case VK_BACK: return {TerminalEventType::backspace, {}};
        case VK_ESCAPE: return {TerminalEventType::escape, {}};
        case VK_TAB: return {TerminalEventType::tab, {}};
        default: break;
        }

        const wchar_t value = key.uChar.UnicodeChar;
        if (value == L'\r' || value == L'\n') return {TerminalEventType::enter, {}};
        if (value == 3) return {TerminalEventType::interrupt, {}};
        if (value < L' ') continue;

        if (value >= 0xD800 && value <= 0xDBFF) {
            impl_->pending_high_surrogate = value;
            continue;
        }
        std::wstring characters;
        if (value >= 0xDC00 && value <= 0xDFFF && impl_->pending_high_surrogate != 0) {
            characters.push_back(impl_->pending_high_surrogate);
            impl_->pending_high_surrogate = 0;
        } else {
            impl_->pending_high_surrogate = 0;
        }
        characters.push_back(value);
        return {TerminalEventType::character, to_utf8(characters)};
    }
}

int TerminalSession::read_confirmation(const std::function<void(std::string_view)>& display) {
    return read_confirmation(display, nullptr);
}

int TerminalSession::read_confirmation(const std::function<void(std::string_view)>& display,
                                       const std::atomic_bool* cancelled) {
    if (impl_->input != INVALID_HANDLE_VALUE) {
        FlushConsoleInputBuffer(impl_->input);
    }
    const int answer = edit_confirmation([this, cancelled] {
        if (auto* flag = active_cancel_flag.load(std::memory_order_acquire);
            flag && flag->load(std::memory_order_relaxed)) {
            return TerminalEvent{TerminalEventType::interrupt, {}};
        }
        return read_event(cancelled);
    }, display);
    // Type-ahead from this decision must not become the next decision.
    if (impl_->input != INVALID_HANDLE_VALUE) FlushConsoleInputBuffer(impl_->input);
    return answer;
}

bool TerminalSession::native_scrollback() const noexcept { return true; }

void TerminalSession::set_copy_mode(bool) {}

bool TerminalSession::copy_mode() const noexcept { return impl_->copy_mode; }

struct TerminalCancellationMonitor::Impl {
    std::atomic_bool& cancelled;
    std::function<void()> cancel_request;
    std::jthread thread;
    std::mutex input_mutex;
    std::condition_variable input_changed;
    bool input_paused{};

    Impl(std::atomic_bool& flag, std::function<void()> callback)
        : cancelled(flag), cancel_request(std::move(callback)) {
        active_cancel_flag.store(&cancelled, std::memory_order_release);
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input != INVALID_HANDLE_VALUE) FlushConsoleInputBuffer(input);
        interrupt_pending.store(false);
        thread = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                bool cancel = false;
                {
                    // pause_input takes this lock: confirmation is the only
                    // console reader once it returns. Never call a provider
                    // callback while holding the input lock.
                    std::unique_lock lock(input_mutex);
                    input_changed.wait(lock, [&] { return !input_paused || stop.stop_requested(); });
                    if (stop.stop_requested()) return;
                    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
                    if (!input_paused && input != INVALID_HANDLE_VALUE && interrupt_event) {
                        const HANDLE handles[]{input, interrupt_event};
                        const auto ready = WaitForMultipleObjects(2, handles, FALSE, 25);
                        if (ready == WAIT_OBJECT_0) {
                            INPUT_RECORD record{};
                            DWORD count{};
                            if (ReadConsoleInputW(input, &record, 1, &count) && count &&
                                record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
                                const auto& key = record.Event.KeyEvent;
                                cancel = key.wVirtualKeyCode == VK_ESCAPE ||
                                    key.uChar.UnicodeChar == 3 ||
                                    (key.wVirtualKeyCode == 'C' && (key.dwControlKeyState &
                                        (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)));
                            }
                        }
                        cancel = cancel || interrupt_pending.exchange(false);
                    }
                }
                if (cancel || cancelled.load(std::memory_order_relaxed)) {
                    cancelled.store(true, std::memory_order_relaxed);
                    cancel_request();
                    return;
                }
                // Yield input ownership before waiting on the next record.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    ~Impl() {
        thread.request_stop();
        input_changed.notify_all();
        if (thread.joinable()) thread.join();
        active_cancel_flag.store(nullptr, std::memory_order_release);
    }
};

TerminalCancellationMonitor::TerminalCancellationMonitor(
    std::atomic_bool& cancelled, std::function<void()> cancel_request)
    : impl_(std::make_unique<Impl>(cancelled, std::move(cancel_request))) {}

TerminalCancellationMonitor::~TerminalCancellationMonitor() = default;

void TerminalCancellationMonitor::pause_input() {
    std::lock_guard lock(impl_->input_mutex);
    impl_->input_paused = true;
}

void TerminalCancellationMonitor::resume_input() {
    std::lock_guard lock(impl_->input_mutex);
    impl_->input_paused = false;
    impl_->input_changed.notify_one();
}

} // namespace arn
