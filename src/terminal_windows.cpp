#include "terminal.hpp"
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
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view reset = "\x1b[0m";
std::atomic<std::atomic_bool*> active_cancel_flag = nullptr;
std::atomic_bool interrupt_pending = false;
std::atomic<std::uint64_t> console_geometry_events{1};
// Last buffer size reported by the host, packed as width << 16 | height.
std::atomic<std::uint32_t> last_buffer_size{0};
HANDLE interrupt_event = nullptr;

std::uint32_t packed_size(COORD size) {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(size.X)) << 16 |
           static_cast<std::uint16_t>(size.Y);
}

// Hosts can repeat a buffer-size record without resizing (for example after
// a console mode change). Only a different size invalidates live regions.
void note_buffer_size_event(COORD size) {
    const auto packed = packed_size(size);
    if (last_buffer_size.exchange(packed) != packed) ++console_geometry_events;
}

// Flushing must not hide a pending resize from live-region validation.
void flush_console_input(HANDLE input) {
    DWORD pending{};
    if (GetNumberOfConsoleInputEvents(input, &pending) && pending) {
        std::vector<INPUT_RECORD> records(pending);
        DWORD count{};
        if (PeekConsoleInputW(input, records.data(), pending, &count)) {
            for (DWORD index = 0; index < count; ++index)
                if (records[index].EventType == WINDOW_BUFFER_SIZE_EVENT)
                    note_buffer_size_event(records[index].Event.WindowBufferSizeEvent.dwSize);
        }
    }
    FlushConsoleInputBuffer(input);
}

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
    WORD pending_surrogate_repeats{};
    std::atomic<std::uint64_t> geometry_generation{1};
    TerminalSize last_geometry{};

    TerminalSize observe_geometry(const CONSOLE_SCREEN_BUFFER_INFO& info) {
        const TerminalSize next{info.srWindow.Right - info.srWindow.Left + 1,
                                info.srWindow.Bottom - info.srWindow.Top + 1};
        if (next.width != last_geometry.width || next.height != last_geometry.height) {
            ++geometry_generation;
            last_geometry = next;
        }
        return next;
    }

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
    if (CONSOLE_SCREEN_BUFFER_INFO info{}; GetConsoleScreenBufferInfo(impl_->output, &info))
        last_buffer_size.store(packed_size(info.dwSize));

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
        return impl_->observe_geometry(info);
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

        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            note_buffer_size_event(event.Event.WindowBufferSizeEvent.dwSize);
            return {TerminalEventType::resize, {}};
        }
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

struct TerminalLiveAnchor::State {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    SMALL_RECT region{};
    std::vector<CHAR_INFO> cells;
    std::uint64_t generation{};
    std::uint64_t event_generation{};
    int rows_before{}, rows_after{};
};

namespace {
bool same_position(const CONSOLE_SCREEN_BUFFER_INFO& a, const CONSOLE_SCREEN_BUFFER_INFO& b) {
    return a.dwSize.X == b.dwSize.X && a.dwSize.Y == b.dwSize.Y &&
           a.dwCursorPosition.X == b.dwCursorPosition.X && a.dwCursorPosition.Y == b.dwCursorPosition.Y &&
           a.srWindow.Left == b.srWindow.Left && a.srWindow.Right == b.srWindow.Right &&
           a.srWindow.Top == b.srWindow.Top && a.srWindow.Bottom == b.srWindow.Bottom;
}
}

TerminalLiveAnchor TerminalSession::capture_live_anchor(int rows_before, int rows_after) const {
    if (rows_before < 0 || rows_after < 0) return {};
    std::cout.flush();
    auto state = std::make_shared<TerminalLiveAnchor::State>();
    if (!GetConsoleScreenBufferInfo(impl_->output, &state->info)) return {};
    impl_->observe_geometry(state->info);
    state->generation = impl_->geometry_generation.load();
    state->event_generation = console_geometry_events.load();
    state->rows_before = rows_before;
    state->rows_after = rows_after;
    const auto& info = state->info;
    if (info.dwCursorPosition.Y < info.srWindow.Top || info.dwCursorPosition.Y > info.srWindow.Bottom)
        return {};
    state->region = {info.srWindow.Left,
        static_cast<SHORT>(std::max<int>(info.srWindow.Top, info.dwCursorPosition.Y - rows_before)),
        info.srWindow.Right,
        static_cast<SHORT>(std::min<int>(info.srWindow.Bottom, info.dwCursorPosition.Y + rows_after))};
    const COORD extent{static_cast<SHORT>(state->region.Right - state->region.Left + 1),
                       static_cast<SHORT>(state->region.Bottom - state->region.Top + 1)};
    state->cells.resize(static_cast<std::size_t>(extent.X) * extent.Y);
    auto read_region = state->region;
    if (!ReadConsoleOutputW(impl_->output, state->cells.data(), extent, {0, 0}, &read_region) ||
        read_region.Left != state->region.Left || read_region.Right != state->region.Right ||
        read_region.Top != state->region.Top || read_region.Bottom != state->region.Bottom)
        return {};
    CONSOLE_SCREEN_BUFFER_INFO after{};
    if (!GetConsoleScreenBufferInfo(impl_->output, &after) || !same_position(info, after) ||
        state->generation != impl_->geometry_generation.load() ||
        state->event_generation != console_geometry_events.load()) return {};
    return {std::move(state)};
}

bool TerminalSession::live_anchor_valid(const TerminalLiveAnchor& anchor) const {
    if (!anchor.state) return false;
    const auto current = capture_live_anchor(anchor.state->rows_before, anchor.state->rows_after);
    if (!current.state) return false;
    const auto& a = *anchor.state;
    const auto& b = *current.state;
    if (a.generation != b.generation || a.event_generation != b.event_generation ||
        !same_position(a.info, b.info) ||
        a.region.Top != b.region.Top || a.region.Bottom != b.region.Bottom ||
        a.cells.size() != b.cells.size()) return false;
    for (std::size_t i = 0; i < a.cells.size(); ++i)
        if (a.cells[i].Char.UnicodeChar != b.cells[i].Char.UnicodeChar ||
            a.cells[i].Attributes != b.cells[i].Attributes) return false;
    return true;
}

bool TerminalSession::begin_output_region() const {
    std::cout.flush();
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(impl_->output, &info)) {
        // Redirected stdout is a byte sink, not an editable screen buffer.
        // Preserve CLI output by appending; never seek with guessed geometry.
        const auto type = GetFileType(impl_->output);
        if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) return false;
        std::cout << "\r\n" << std::flush;
        return true;
    }
    const auto geometry = impl_->observe_geometry(info);
    // Allocate a blank row below the viewport. Old rows enter native history
    // without erasure, even if the host relocated the previous cursor.
    std::cout << "\x1b[" << geometry.height << ";1H\r\n" << std::flush;
    return true;
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
                            if (received &&
                                record.EventType == WINDOW_BUFFER_SIZE_EVENT)
                                note_buffer_size_event(record.Event.WindowBufferSizeEvent.dwSize);
                            if (received && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
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
