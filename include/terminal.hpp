#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace arn {

struct TerminalSize {
    int width{100};
    int height{30};
};

enum class TerminalEventType {
    none,
    character,
    enter,
    backspace,
    escape,
    tab,
    resize,
    wheel_up,
    wheel_down,
    page_up,
    page_down,
    end,
    f2,
    interrupt,
    end_of_input,
};

struct TerminalEvent {
    TerminalEventType type{TerminalEventType::none};
    std::string text;
    // One decoded scalar/editing key, repeated within one host record. Keeping
    // the bounded count avoids a pending replay queue crossing input modes.
    std::uint16_t repeat_count{1};
};

class TerminalSession {
public:
    TerminalSession();
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    [[nodiscard]] TerminalSize size() const;
    [[nodiscard]] TerminalEvent read_event();
    [[nodiscard]] TerminalEvent read_event(const std::atomic_bool* cancelled);
    [[nodiscard]] int read_confirmation(
        const std::function<void(std::string_view)>& display = {});
    [[nodiscard]] int read_confirmation(
        const std::function<void(std::string_view)>& display,
        const std::atomic_bool* cancelled);
    // True: append-only plain output (Windows with redirected input or
    // output). False: the interactive alternate-screen viewport.
    [[nodiscard]] bool native_scrollback() const noexcept;
    // Drop queued keys and any interrupt received before this call.
    void discard_pending_input();

    void set_copy_mode(bool enabled);
    [[nodiscard]] bool copy_mode() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TerminalCancellationMonitor {
public:
    TerminalCancellationMonitor(std::atomic_bool& cancelled,
                                std::function<void()> cancel_request);
    ~TerminalCancellationMonitor();

    TerminalCancellationMonitor(const TerminalCancellationMonitor&) = delete;
    TerminalCancellationMonitor& operator=(const TerminalCancellationMonitor&) = delete;

    void pause_input();
    void resume_input();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arn
