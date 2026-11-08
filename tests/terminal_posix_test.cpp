// POSIX terminal session tests (no terminal needed), plus a probe the PTY
// suite runs on a real pseudo-terminal:
//
//   terminal_posix_test                 unit tests
//   terminal_posix_test --probe-throw   render a conversation, then throw

#include "terminal.hpp"
#include "terminal_posix_view.hpp"
#include "terminal_ui.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Type = arn::TerminalEventType;

int checks = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

// Distinct handlers installed before a session, to see that it restores them.
void previous_terminate(int) {}
void previous_hangup(int) {}
void previous_interrupt(int) {}
void previous_resize(int) {}

void install(int signal, void (*handler)(int)) {
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
}

void (*current(int signal))(int) {
    struct sigaction action{};
    sigaction(signal, nullptr, &action);
    return action.sa_handler;
}

// stdin becomes the read end of a pipe the test writes key bytes into.
struct KeyPipe {
    int ends[2]{-1, -1};
    int saved{dup(STDIN_FILENO)};
    KeyPipe() {
        check(pipe(ends) == 0, "pipe() failed");
        dup2(ends[0], STDIN_FILENO);
    }
    ~KeyPipe() {
        dup2(saved, STDIN_FILENO);
        close(saved);
        close(ends[0]);
        close(ends[1]);
    }
    void send(std::string_view bytes) const {
        check(write(ends[1], bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write() failed");
    }
};

// Session output (mouse-tracking switches) stays out of the test log.
struct QuietOutput {
    std::ostringstream sink;
    std::streambuf* original{std::cout.rdbuf(sink.rdbuf())};
    ~QuietOutput() { std::cout.rdbuf(original); }
};

void session_restores_signal_handlers() {
    QuietOutput quiet;
    install(SIGTERM, previous_terminate);
    install(SIGHUP, previous_hangup);
    install(SIGINT, previous_interrupt);
    install(SIGWINCH, previous_resize);
    {
        arn::TerminalSession session;
        for (const int signal : {SIGTERM, SIGHUP, SIGINT, SIGWINCH})
            check(current(signal) != SIG_DFL && current(signal) != SIG_IGN, "The session did not install a handler");
        check(current(SIGTERM) != previous_terminate && current(SIGHUP) != previous_hangup,
              "SIGTERM/SIGHUP are not handled during the session");
    }
    check(current(SIGTERM) == previous_terminate && current(SIGHUP) == previous_hangup &&
              current(SIGINT) == previous_interrupt && current(SIGWINCH) == previous_resize,
          "The session did not restore the previous signal handlers");
    for (const int signal : {SIGTERM, SIGHUP, SIGINT, SIGWINCH}) install(signal, SIG_DFL);
}

// A signal may be delivered to any thread; the waiting reader must still
// wake. Here it is sent to another thread while the main thread waits.
void terminate_signal_ends_input(int signal) {
    QuietOutput quiet;
    KeyPipe keys;
    arn::TerminalSession session;
    std::thread sender([signal] {
        std::this_thread::sleep_for(100ms);
        pthread_kill(pthread_self(), signal);
    });
    const auto start = std::chrono::steady_clock::now();
    const auto event = session.read_event();
    sender.join();
    const auto name = std::string(signal == SIGTERM ? "SIGTERM" : "SIGHUP");
    check(event.type == Type::end_of_input, name + " did not end input");
    check(std::chrono::steady_clock::now() - start < 3s, name + " sent to another thread did not wake the reader");
    check(session.read_event().type == Type::end_of_input, name + ": input must stay ended");
    check(session.read_event(nullptr).type == Type::end_of_input, name + ": input must stay ended");
    // A second signal is not caught: it stops the process at once.
    check(current(signal) == SIG_DFL, name + ": the handler must be reset after the first signal");
}

void interrupt_and_resize_still_work() {
    QuietOutput quiet;
    KeyPipe keys;
    arn::TerminalSession session;
    raise(SIGWINCH);
    check(session.read_event().type == Type::resize, "SIGWINCH did not report a resize");
    raise(SIGINT);
    check(session.read_event().type == Type::interrupt, "SIGINT did not report an interrupt");
    keys.send("a");
    const auto event = session.read_event();
    check(event.type == Type::character && event.text == "a", "Keys after signals were lost");
    keys.send("\x1b[5~");
    check(session.read_event().type == Type::page_up, "PgUp at the prompt was not read");
}

std::vector<arn::TerminalEvent> wait_for_deferred(std::size_t count) {
    std::vector<arn::TerminalEvent> events;
    for (int attempt = 0; attempt < 200 && events.size() < count; ++attempt) {
        for (auto& event : arn::take_deferred_view_events()) events.push_back(std::move(event));
        if (events.size() < count) std::this_thread::sleep_for(10ms);
    }
    return events;
}

// During a request the monitor owns input: view keys are kept for the
// rendering thread, other keys are ignored, Esc cancels.
void monitor_keeps_view_keys() {
    QuietOutput quiet;
    KeyPipe keys;
    arn::TerminalSession session;
    std::atomic_bool cancelled{false};
    std::atomic_int requests{0};
    {
        arn::TerminalCancellationMonitor monitor(cancelled, [&] { ++requests; });
        keys.send("\x1b[5~");
        std::this_thread::sleep_for(60ms);
        keys.send("\x1b[<64;10;5M");
        std::this_thread::sleep_for(60ms);
        keys.send("typed text");
        std::this_thread::sleep_for(60ms);
        keys.send("\x1b[6~");
        std::this_thread::sleep_for(60ms);
        keys.send("\x1b[F");
        const auto events = wait_for_deferred(4);
        check(events.size() == 4 && events[0].type == Type::page_up && events[1].type == Type::wheel_up &&
                  events[2].type == Type::page_down && events[3].type == Type::end,
              "The monitor did not keep the view keys in order");
        check(!cancelled.load() && requests.load() == 0, "A view key or typed text cancelled the request");
        keys.send("\x1b");
        for (int attempt = 0; attempt < 200 && !cancelled.load(); ++attempt) std::this_thread::sleep_for(10ms);
        check(cancelled.load() && requests.load() == 1, "Esc did not cancel the request");
    }
    // Keys left from one request are not applied to the next.
    cancelled = false;
    {
        arn::TerminalCancellationMonitor monitor(cancelled, [] {});
        keys.send("\x1b[5~");
        for (int attempt = 0; attempt < 100; ++attempt) std::this_thread::sleep_for(5ms);
    }
    {
        arn::TerminalCancellationMonitor monitor(cancelled, [] {});
        check(arn::take_deferred_view_events().empty(), "A new request inherited stale view keys");
    }
}

void monitor_cancels_on_terminate() {
    QuietOutput quiet;
    KeyPipe keys;
    arn::TerminalSession session;
    std::atomic_bool cancelled{false};
    std::atomic_int requests{0};
    arn::TerminalCancellationMonitor monitor(cancelled, [&] { ++requests; });
    raise(SIGTERM);
    for (int attempt = 0; attempt < 200 && !cancelled.load(); ++attempt) std::this_thread::sleep_for(10ms);
    check(cancelled.load() && requests.load() == 1, "SIGTERM did not cancel the running request");
    check(session.read_event().type == Type::end_of_input, "SIGTERM did not end input after the request");
}

int probe_throw() {
    using arn::terminal_ui::Tone;
    try {
        arn::TerminalSession terminal;
        arn::terminal_ui::Ui ui(terminal);
        ui.status(ui.ready_status());
        ui.add(Tone::user, "arn › probe question");
        for (int entry = 0; entry < 3; ++entry) ui.add(Tone::normal, "probe-entry-" + std::to_string(entry) + "!");
        ui.render();
        std::this_thread::sleep_for(800ms); // Long enough to see the alternate screen.
        throw std::runtime_error("probe failure");
    } catch (const std::exception& error) {
        std::cerr << "probe caught: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--probe-throw") return probe_throw();
    const std::pair<const char*, void (*)()> cases[] = {
        {"session_restores_signal_handlers", session_restores_signal_handlers},
        {"sigterm_ends_input", +[] { terminate_signal_ends_input(SIGTERM); }},
        {"sighup_ends_input", +[] { terminate_signal_ends_input(SIGHUP); }},
        {"interrupt_and_resize_still_work", interrupt_and_resize_still_work},
        {"monitor_keeps_view_keys", monitor_keeps_view_keys},
        {"monitor_cancels_on_terminate", monitor_cancels_on_terminate},
    };
    int failures = 0;
    for (const auto& [name, test] : cases) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << checks << " checks\n";
    return failures ? 1 : 0;
}
