#include "acp/acp_subprocess.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using arn::acp::Subprocess;

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Pipes {
#ifdef _WIN32
    std::array<HANDLE, 6> fds{};
    ~Pipes() { for (auto fd : fds) if (fd) CloseHandle(fd); }
#else
    std::array<int, 6> fds{-1, -1, -1, -1, -1, -1};
    ~Pipes() { for (int fd : fds) if (fd >= 0) ::close(fd); }
#endif
    Pipes() {
        for (int i = 0; i < 6; i += 2) {
#ifdef _WIN32
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            check(CreatePipe(&fds[i], &fds[i + 1], &sa, 0), "CreatePipe");
            check(SetHandleInformation(fds[i == 0 ? i + 1 : i], HANDLE_FLAG_INHERIT, 0), "inheritance");
#else
            check(::pipe(fds.data() + i) == 0, "pipe");
            for (int index : {i, i + 1}) check(::fcntl(fds[index], F_SETFD, FD_CLOEXEC) == 0, "CLOEXEC");
#endif
        }
    }
    void spawn(Subprocess& child, const arn::acp::SubprocessSpec& spec) {
        child.spawn(spec, {fds[0], fds[3], fds[5]});
#ifdef _WIN32
        fds[0] = fds[3] = fds[5] = nullptr;
        child.configure_parent_handles(fds[1], fds[2], fds[4]);
        fds[1] = fds[2] = fds[4] = nullptr;
#else
        fds[0] = fds[3] = fds[5] = -1;
        child.configure_parent_handles(fds[1], fds[2], fds[4]);
        fds[1] = fds[2] = fds[4] = -1;
#endif
    }
};

std::string read_until(Subprocess& child, bool eof) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::string output;
    std::array<char, 4096> buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        const int count = child.read_stdout(buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), count);
        else if (count == 0) {
#ifdef _WIN32
            // The C++ child uses the CRT's text-mode stdout (CRLF).
            output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
#endif
            return output;
        }
        else check(count == -2, "Pipe read error");
        if (!eof && output.find('\n') != std::string::npos) return output;
    }
    throw std::runtime_error("Pipe read/EOF deadline");
}

unsigned resource_count() {
#ifdef _WIN32
    DWORD count{};
    check(GetProcessHandleCount(GetCurrentProcess(), &count), "Handle count");
    return count;
#else
    rlimit limit{};
    check(getrlimit(RLIMIT_NOFILE, &limit) == 0, "FD limit");
    unsigned count{};
    for (int fd = 0; fd < static_cast<int>(std::min<rlim_t>(limit.rlim_cur, 4096)); ++fd)
        if (::fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
#endif
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--echo") {
        std::string line;
        std::getline(std::cin, line);
        // More than a pipe buffer: the host must drain stderr while reading stdout.
        std::cerr << std::string(128 * 1024, 'x') << std::flush;
        const auto* value = std::getenv("ARN_ACP_TEST_VALUE");
        std::cout << line << ':' << (value ? value : "missing") << '\n' << std::flush;
        return 23;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--exit") return 7;
    if (argc > 1 && std::string_view(argv[1]) == "--sleep") {
#ifndef _WIN32
        std::signal(SIGTERM, SIG_IGN);
        std::cout << ::getpid() << '\n' << std::flush;
#else
        std::cout << "ready\n" << std::flush;
#endif
        std::this_thread::sleep_for(10s);
        return 0;
    }
    try {
        const auto executable = std::filesystem::absolute(argv[0]).string();
        // Windows lazily initializes process-launch/runtime handles on first use.
        // Measure repeated ownership after one complete, cleaned-up launch.
        {
            Subprocess child;
            Pipes pipes;
            pipes.spawn(child, {executable, {"--exit"}, {}});
            check(read_until(child, true).empty() && child.wait(3000) == 7, "Warm-up child");
        }
        const unsigned baseline = resource_count();
        for (int round = 0; round < 10; ++round) {
            Subprocess child;
            check(child.exited() && child.wait(0) == -1, "Unstarted state");
            Pipes pipes;
            pipes.spawn(child, {executable, {"--echo"}, {{"ARN_ACP_TEST_VALUE", "fake-value"}}});
            check(child.write_stdin("hello\n", 6), "Parent stdin FD must be wired");
            check(read_until(child, true) == "hello:fake-value\n", "stdout/environment/stdio mapping");
            check(child.wait(3000) == 23, "Exit code");
            check(child.exited() && !child.running() && child.exit_code() == 23, "Exit poll state");
            check(child.wait(0) == 23 && child.wait(-1) == 23, "Repeated wait must retain code");
            child.terminate(); // Must never signal a reaped/reused PID.
            check(child.wait(0) == 23, "Termination after exit changes cached code");
            child.close_pipes(); child.close_pipes();
        }
        check(resource_count() == baseline, "Round trips leaked resources");
        for (int round = 0; round < 10; ++round) {
            Subprocess child;
            Pipes pipes;
            bool failed = false;
            try { pipes.spawn(child, {"__arn_missing_test_executable__", {}, {}}); }
            catch (const std::runtime_error&) { failed = true; }
            check(failed && !child.running(), "Failed spawn ownership/state");
        }
        check(resource_count() == baseline, "Failed spawns leaked resources");
        {
            Subprocess child;
            Pipes pipes;
            pipes.spawn(child, {executable, {"--exit"}, {}});
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!child.exited() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(2ms);
            check(child.exited() && child.wait(0) == 7, "Polling must cache reaped exit code");
            check(!child.write_stdin("ignored", 7), "Closed child stdin must fail without SIGPIPE");
            check(read_until(child, true).empty(), "Unused parent write ends must not delay EOF");
        }
        const auto start = std::chrono::steady_clock::now();
#ifndef _WIN32
        pid_t sleeping_pid{};
#endif
        {
            Subprocess child;
            Pipes pipes;
            pipes.spawn(child, {executable, {"--sleep"}, {}});
            const auto ready = read_until(child, false);
#ifndef _WIN32
            sleeping_pid = static_cast<pid_t>(std::stoi(ready));
#endif
            Subprocess moved(std::move(child));
            check(!child.running() && moved.running(), "Move ownership");
            Subprocess assigned;
            assigned = std::move(moved);
            check(!moved.running() && assigned.running(), "Move assignment ownership");
        }
        check(std::chrono::steady_clock::now() - start < 3s, "Destructor must terminate an unresponsive child");
#ifndef _WIN32
        int status{};
        check(waitpid(sleeping_pid, &status, WNOHANG) == -1 && errno == ECHILD, "Destructor must reap child");
        // Closed embedding stdin may cause pipe() to allocate descriptor zero.
        const int saved_stdin = dup(STDIN_FILENO);
        close(STDIN_FILENO);
        try {
            Subprocess child;
            Pipes pipes;
            pipes.spawn(child, {executable, {"--echo"}, {{"ARN_ACP_TEST_VALUE", "fake-value"}}});
            check(child.write_stdin("hello\n", 6), "Low FD mapping");
            check(read_until(child, true) == "hello:fake-value\n", "Low FD dup2 ordering");
            check(child.wait(3000) == 23, "Low FD child exit");
        } catch (...) {
            if (saved_stdin >= 0) { dup2(saved_stdin, STDIN_FILENO); close(saved_stdin); }
            throw;
        }
        if (saved_stdin >= 0) { dup2(saved_stdin, STDIN_FILENO); close(saved_stdin); }
#endif
        check(resource_count() == baseline, "Lifecycle checks leaked resources");
        std::cout << "ACP subprocess ownership tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
