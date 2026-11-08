#include "terminal.hpp"
#include "terminal_posix_view.hpp"
#include "terminal_windows_mode.hpp"
#include "confirmation_input.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <cwchar>
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
// The alternate-screen renderer (terminal_windows_mode.hpp) is active.
// Only then does the cancellation monitor keep view keys for the renderer.
std::atomic_bool alternate_active{false};
std::mutex deferred_mutex;
std::vector<arn::TerminalEvent> deferred_view_events;

void defer_view_key(const KEY_EVENT_RECORD& key) {
    arn::TerminalEvent event;
    switch (key.wVirtualKeyCode) {
    case VK_PRIOR: event = {arn::TerminalEventType::page_up, {}, key.wRepeatCount}; break;
    case VK_NEXT: event = {arn::TerminalEventType::page_down, {}, key.wRepeatCount}; break;
    case VK_END: event = {arn::TerminalEventType::end, {}}; break;
    default: return;
    }
    std::lock_guard lock(deferred_mutex);
    if (deferred_view_events.size() < 256) deferred_view_events.push_back(std::move(event));
}

void flush_console_input(HANDLE input) { FlushConsoleInputBuffer(input); }

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

WindowsRendererSetting windows_renderer_setting() {
    WindowsRendererSetting setting;
    const DWORD size = GetEnvironmentVariableW(L"ARN_TERMINAL_RENDERER", nullptr, 0);
    if (size <= 1) return setting; // Unset or empty: the default.
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(L"ARN_TERMINAL_RENDERER", value.data(), size));
    if (_wcsicmp(value.c_str(), L"alternate") == 0) return setting;
    setting.recognized = false;
    setting.removed_native = _wcsicmp(value.c_str(), L"native") == 0;
    setting.value = to_utf8(std::wstring_view(value).substr(0, 64));
    return setting;
}

std::vector<TerminalEvent> take_deferred_view_events() {
    std::lock_guard lock(deferred_mutex);
    return std::exchange(deferred_view_events, {});
}

struct TerminalSession::Impl {
    HANDLE output{INVALID_HANDLE_VALUE};
    HANDLE input{INVALID_HANDLE_VALUE};
    DWORD output_mode{};
    DWORD input_mode{};
    UINT output_code_page{};
    UINT input_code_page{};
    bool output_mode_changed{};
    bool input_mode_changed{};
    bool alternate{}; // The alternate-screen renderer is in use.
    bool copy_mode{};
    wchar_t pending_high_surrogate{};
    WORD pending_surrogate_repeats{};

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

    // The alternate screen needs a console that now accepts VT output and gives
    // console input. Redirected input or output gets plain append-only output
    // (see terminal_windows_mode.hpp); no setting overrides that.
    if (impl_->output_mode_changed && impl_->input_mode_changed) {
        DWORD mode{};
        impl_->alternate = GetConsoleMode(impl_->output, &mode) &&
                           (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
    alternate_active.store(impl_->alternate);

    std::cout << "\x1b[?1000l\x1b[?1006l" << std::flush;
}

TerminalSession::~TerminalSession() {
    alternate_active.store(false);
    (void)take_deferred_view_events();
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
        return {info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1};
    }
    return {};
}

TerminalEvent TerminalSession::read_event() { return read_event(nullptr); }

TerminalEvent TerminalSession::read_event(const std::atomic_bool* cancelled) {
    for (;;) {
        if ((cancelled && cancelled->load()) || interrupt_pending.exchange(false)) {
            impl_->pending_high_surrogate = 0;
            return {TerminalEventType::interrupt, {}};
        }
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

        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) return {TerminalEventType::resize, {}};
        if (event.EventType == MOUSE_EVENT) continue;
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;

        const auto& key = event.Event.KeyEvent;
        if (key.wRepeatCount == 0) continue;
        // Only a low surrogate in the next key record may complete a pending
        // high surrogate; any other key, including a control key, drops it.
        const wchar_t high_surrogate = std::exchange(impl_->pending_high_surrogate, wchar_t{});
        if (key.wVirtualKeyCode == VK_RETURN) return {TerminalEventType::enter, {}};
        if (key.wVirtualKeyCode == 'C' &&
            (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
            return {TerminalEventType::interrupt, {}};
        switch (key.wVirtualKeyCode) {
        case VK_F2: continue;
        case VK_PRIOR: return {TerminalEventType::page_up, {}, key.wRepeatCount};
        case VK_NEXT: return {TerminalEventType::page_down, {}, key.wRepeatCount};
        case VK_END: return {TerminalEventType::end, {}};
        case VK_BACK: return {TerminalEventType::backspace, {}, key.wRepeatCount};
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
            impl_->pending_surrogate_repeats = key.wRepeatCount;
            continue;
        }
        std::wstring characters;
        if (value >= 0xDC00 && value <= 0xDFFF && high_surrogate != 0) {
            // A repeated non-BMP scalar is represented by matching high/low
            // records. An unmatched count is malformed, not permission to
            // invent or retain partial characters for a later input mode.
            if (impl_->pending_surrogate_repeats != key.wRepeatCount) continue;
            characters.push_back(high_surrogate);
        }
        characters.push_back(value);
        return {TerminalEventType::character, to_utf8(characters), key.wRepeatCount};
    }
}

int TerminalSession::read_confirmation(const std::function<void(std::string_view)>& display) {
    return read_confirmation(display, nullptr);
}

int TerminalSession::read_confirmation(const std::function<void(std::string_view)>& display,
                                       const std::atomic_bool* cancelled) {
    impl_->pending_high_surrogate = 0;
    if (impl_->input != INVALID_HANDLE_VALUE) {
        flush_console_input(impl_->input);
    }
    const int answer = edit_confirmation([this, cancelled] {
        if (auto* flag = active_cancel_flag.load(std::memory_order_acquire);
            flag && flag->load(std::memory_order_relaxed)) {
            return TerminalEvent{TerminalEventType::interrupt, {}};
        }
        return read_event(cancelled);
    }, display);
    // Type-ahead from this decision must not become the next decision.
    impl_->pending_high_surrogate = 0;
    if (impl_->input != INVALID_HANDLE_VALUE) flush_console_input(impl_->input);
    return answer;
}

void TerminalSession::discard_pending_input() {
    impl_->pending_high_surrogate = 0;
    if (impl_->input != INVALID_HANDLE_VALUE) flush_console_input(impl_->input);
    interrupt_pending.store(false);
}

// Plain append-only output unless the alternate screen is active.
bool TerminalSession::native_scrollback() const noexcept { return !impl_->alternate; }

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
        (void)take_deferred_view_events(); // Keys from an earlier request are stale.
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input != INVALID_HANDLE_VALUE) flush_console_input(input);
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
                            const bool received = ReadConsoleInputW(input, &record, 1, &count) && count;
                            if (received && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
                                const auto& key = record.Event.KeyEvent;
                                cancel = key.wVirtualKeyCode == VK_ESCAPE ||
                                    key.uChar.UnicodeChar == 3 ||
                                    (key.wVirtualKeyCode == 'C' && (key.dwControlKeyState &
                                        (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)));
                                // Alternate screen: scrolling stays usable while
                                // output arrives. The native renderer drops these.
                                if (!cancel && alternate_active.load()) defer_view_key(key);
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
