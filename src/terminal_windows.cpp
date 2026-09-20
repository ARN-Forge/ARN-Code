#include "terminal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view reset = "\x1b[0m";
std::atomic<std::atomic_bool*> active_cancel_flag = nullptr;

BOOL WINAPI handle_console_control(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (auto* flag = active_cancel_flag.load(std::memory_order_acquire)) {
        flag->store(true, std::memory_order_relaxed);
    }
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

    void apply_copy_mode(bool enabled) {
        if (!input_mode_changed) return;
        DWORD mode = input_mode | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
        if (enabled) {
            mode |= ENABLE_QUICK_EDIT_MODE;
            mode &= ~ENABLE_MOUSE_INPUT;
        } else {
            mode |= ENABLE_MOUSE_INPUT;
            mode &= ~ENABLE_QUICK_EDIT_MODE;
        }
        SetConsoleMode(input, mode);
        copy_mode = enabled;
    }
};

TerminalSession::TerminalSession() : impl_(std::make_unique<Impl>()) {
    impl_->output_code_page = GetConsoleOutputCP();
    impl_->input_code_page = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(handle_console_control, TRUE);

    impl_->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (impl_->output != INVALID_HANDLE_VALUE && GetConsoleMode(impl_->output, &impl_->output_mode)) {
        impl_->output_mode_changed = true;
        SetConsoleMode(impl_->output, impl_->output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    impl_->input = GetStdHandle(STD_INPUT_HANDLE);
    if (impl_->input != INVALID_HANDLE_VALUE && GetConsoleMode(impl_->input, &impl_->input_mode)) {
        impl_->input_mode_changed = true;
        impl_->apply_copy_mode(false);
    }

    std::cout << "\x1b[?1049h\x1b[2J\x1b[H" << std::flush;
}

TerminalSession::~TerminalSession() {
    std::cout << reset << "\x1b[?25h\x1b[?1049l" << std::flush;
    if (impl_->output_mode_changed) SetConsoleMode(impl_->output, impl_->output_mode);
    if (impl_->input_mode_changed) SetConsoleMode(impl_->input, impl_->input_mode);
    if (impl_->output_code_page != 0) SetConsoleOutputCP(impl_->output_code_page);
    if (impl_->input_code_page != 0) SetConsoleCP(impl_->input_code_page);
    SetConsoleCtrlHandler(handle_console_control, FALSE);
}

TerminalSize TerminalSession::size() const {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (impl_->output != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(impl_->output, &info)) {
        return {info.srWindow.Right - info.srWindow.Left + 1,
                info.srWindow.Bottom - info.srWindow.Top + 1};
    }
    return {};
}

TerminalEvent TerminalSession::read_event() {
    for (;;) {
        INPUT_RECORD event{};
        DWORD count{};
        if (impl_->input == INVALID_HANDLE_VALUE ||
            !ReadConsoleInputW(impl_->input, &event, 1, &count)) {
            return {TerminalEventType::end_of_input, {}};
        }

        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            return {TerminalEventType::resize, {}};
        }
        if (event.EventType == MOUSE_EVENT) {
            const auto& mouse = event.Event.MouseEvent;
            if (mouse.dwEventFlags == MOUSE_WHEELED) {
                const auto delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
                return {delta > 0 ? TerminalEventType::wheel_up : TerminalEventType::wheel_down, {}};
            }
            continue;
        }
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;

        const auto& key = event.Event.KeyEvent;
        switch (key.wVirtualKeyCode) {
        case VK_F2: return {TerminalEventType::f2, {}};
        case VK_PRIOR: return {TerminalEventType::page_up, {}};
        case VK_NEXT: return {TerminalEventType::page_down, {}};
        case VK_END: return {TerminalEventType::end, {}};
        case VK_BACK: return {TerminalEventType::backspace, {}};
        case VK_TAB: return {TerminalEventType::tab, {}};
        default: break;
        }

        const wchar_t value = key.uChar.UnicodeChar;
        if (value == L'\r') return {TerminalEventType::enter, {}};
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

int TerminalSession::read_confirmation() {
    for (;;) {
        const int value = _getch();
        if (value == 0 || value == 0xE0) {
            (void)_getch();
            continue;
        }
        return value;
    }
}

void TerminalSession::set_copy_mode(bool enabled) { impl_->apply_copy_mode(enabled); }

bool TerminalSession::copy_mode() const noexcept { return impl_->copy_mode; }

struct TerminalCancellationMonitor::Impl {
    std::atomic_bool& cancelled;
    std::function<void()> cancel_request;
    std::jthread thread;

    Impl(std::atomic_bool& flag, std::function<void()> callback)
        : cancelled(flag), cancel_request(std::move(callback)) {
        active_cancel_flag.store(&cancelled, std::memory_order_release);
        thread = std::jthread([this](std::stop_token stop) {
            bool previous_escape = false;
            while (!stop.stop_requested()) {
                const bool escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if ((escape && !previous_escape) || cancelled.load(std::memory_order_relaxed)) {
                    cancelled.store(true, std::memory_order_relaxed);
                    cancel_request();
                    return;
                }
                previous_escape = escape;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }

    ~Impl() {
        thread.request_stop();
        if (thread.joinable()) thread.join();
        active_cancel_flag.store(nullptr, std::memory_order_release);
    }
};

TerminalCancellationMonitor::TerminalCancellationMonitor(
    std::atomic_bool& cancelled, std::function<void()> cancel_request)
    : impl_(std::make_unique<Impl>(cancelled, std::move(cancel_request))) {}

TerminalCancellationMonitor::~TerminalCancellationMonitor() = default;

void TerminalCancellationMonitor::pause_input() {}

void TerminalCancellationMonitor::resume_input() {}

} // namespace arn
