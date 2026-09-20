#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

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
};

class TerminalSession {
public:
    TerminalSession();
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    [[nodiscard]] TerminalSize size() const;
    [[nodiscard]] TerminalEvent read_event();
    [[nodiscard]] int read_confirmation();

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
