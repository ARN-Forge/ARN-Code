#include "acp/acp_subprocess.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace arn::acp {

namespace {

bool is_path_separator(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool has_path_separators(const std::string& s) {
    return std::find_if(s.begin(), s.end(), is_path_separator) != s.end();
}

std::string quote_argument(const std::string& value) {
#if defined(_WIN32)
    if (value.empty()) return "\"\"";
    bool needs_quotes = false;
    for (char c : value) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return value;

    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < value.size() && value[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == value.size()) {
            out.append(backslashes * 2, '\\');
            break;
        } else if (value[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(value[i]);
        }
    }
    out.push_back('"');
    return out;
#else
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
#endif
}

std::string build_command_line(const SubprocessSpec& spec) {
    std::ostringstream cmd;
    cmd << quote_argument(spec.command);
    for (const auto& arg : spec.args) {
        cmd << ' ' << quote_argument(arg);
    }
    return cmd.str();
}

std::string build_env_block(const SubprocessSpec& spec) {
#if defined(_WIN32)
    if (spec.env.empty()) return {};
    struct CaseInsensitive {
        bool operator()(const std::string& a, const std::string& b) const {
            return _stricmp(a.c_str(), b.c_str()) < 0;
        }
    };
    std::map<std::string, std::string, CaseInsensitive> environment;
    const auto inherited = GetEnvironmentStringsA();
    if (!inherited) throw std::runtime_error("Could not read subprocess environment.");
    for (const char* entry = inherited; *entry; entry += std::strlen(entry) + 1) {
        const std::string value(entry);
        const auto split = value.find('=', value.starts_with('=') ? 1 : 0);
        if (split != std::string::npos) environment[value.substr(0, split)] = value.substr(split + 1);
    }
    FreeEnvironmentStringsA(inherited);
    for (const auto& [name, value] : spec.env) environment[name] = value;
    std::string block;
    for (const auto& [name, value] : environment) {
        block += name + "=" + value;
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
#else
    (void)spec;
    return {};
#endif
}

} // namespace


Subprocess::Subprocess(Subprocess&& other) noexcept { *this = std::move(other); }
Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
    if (this != &other) {
        release();
        std::scoped_lock lock(state_mutex_, other.state_mutex_);
#if defined(_WIN32)
        handle_ = std::exchange(other.handle_, nullptr);
        stdin_write_ = std::exchange(other.stdin_write_, nullptr);
        stdout_read_ = std::exchange(other.stdout_read_, nullptr);
        stderr_read_ = std::exchange(other.stderr_read_, nullptr);
#else
        pid_ = std::exchange(other.pid_, -1);
        stdin_write_fd_ = std::exchange(other.stdin_write_fd_, -1);
        stdout_read_fd_ = std::exchange(other.stdout_read_fd_, -1);
        stderr_read_fd_ = std::exchange(other.stderr_read_fd_, -1);
#endif
        exit_code_ = std::exchange(other.exit_code_, -1);
        exited_ = std::exchange(other.exited_, false);
    }
    return *this;
}

Subprocess::~Subprocess() { release(); }

void Subprocess::release() noexcept {
    try {
        terminate();
        if (wait(200) == -1) {
#if !defined(_WIN32)
            std::lock_guard lock(state_mutex_);
            if (pid_ > 0 && poll_exit_locked() == -1) ::kill(pid_, SIGKILL);
#endif
        }
        (void)wait(-1);
    } catch (...) {}
    close_pipes();
}

#if defined(_WIN32)
void Subprocess::spawn(const SubprocessSpec& spec, SubprocessPipes pipes) {
    std::lock_guard lock(state_mutex_);
    if (handle_) throw std::runtime_error("Subprocess already spawned.");
    auto command_line = build_command_line(spec);
    auto env_block = build_env_block(spec);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = static_cast<HANDLE>(pipes.child_stdin);
    si.hStdOutput = static_cast<HANDLE>(pipes.child_stdout);
    si.hStdError = static_cast<HANDLE>(pipes.child_stderr);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, env_block.empty() ? nullptr : env_block.data(),
                        nullptr, &si, &pi))
        throw std::runtime_error("CreateProcessA failed (error " + std::to_string(GetLastError()) + ")");
    handle_ = pi.hProcess;
    exited_ = false;
    exit_code_ = -1;
    CloseHandle(pi.hThread);
    CloseHandle(static_cast<HANDLE>(pipes.child_stdin));
    CloseHandle(static_cast<HANDLE>(pipes.child_stdout));
    CloseHandle(static_cast<HANDLE>(pipes.child_stderr));
}

void Subprocess::configure_parent_handles(void* input, void* output, void* error) {
    stdin_write_ = input;
    stdout_read_ = output;
    stderr_read_ = error;
}

int Subprocess::poll_exit_locked() {
    if (exited_) return exit_code_;
    if (!handle_ || WaitForSingleObject(handle_, 0) != WAIT_OBJECT_0) return -1;
    DWORD code{};
    if (!GetExitCodeProcess(handle_, &code)) return -1;
    exit_code_ = static_cast<int>(code);
    exited_ = true;
    CloseHandle(handle_);
    handle_ = nullptr;
    return exit_code_;
}
#else
void Subprocess::spawn(const SubprocessSpec& spec, SubprocessPipes pipes) {
    std::lock_guard lock(state_mutex_);
    if (pid_ > 0) throw std::runtime_error("Subprocess already spawned.");
    // Duplicate above stdio before creating actions: this also handles pipe ends
    // numbered 0/1/2 when the embedding application's stdio was closed.
    std::array<int, 3> copies{-1, -1, -1};
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) throw std::runtime_error("posix_spawn actions: " + std::string(std::strerror(rc)));
    const auto cleanup = [&] {
        posix_spawn_file_actions_destroy(&actions);
        for (int fd : copies) if (fd >= 0) ::close(fd);
    };
    try {
        const std::array<int, 3> originals{pipes.child_stdin, pipes.child_stdout, pipes.child_stderr};
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copies[i] = ::fcntl(originals[i], F_DUPFD_CLOEXEC, 3);
            if (copies[i] < 0) throw std::runtime_error("Could not duplicate ACP pipe.");
            rc = posix_spawn_file_actions_adddup2(&actions, copies[i], static_cast<int>(i));
            if (rc) throw std::runtime_error("Could not map ACP stdio.");
        }
        // dup2 must precede close. All unused parent ends are CLOEXEC.
        for (int fd : copies) {
            if (posix_spawn_file_actions_addclose(&actions, fd))
                throw std::runtime_error("Could not close child ACP pipe.");
        }
        for (int fd : originals) {
            if (fd > STDERR_FILENO && posix_spawn_file_actions_addclose(&actions, fd))
                throw std::runtime_error("Could not close original child ACP pipe.");
        }
        std::vector<char*> argv{const_cast<char*>(spec.command.c_str())};
        for (const auto& arg : spec.args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        // Own strings until posix_spawnp returns; overrides replace inherited values.
        std::map<std::string, std::string> environment;
        for (char** entry = environ; entry && *entry; ++entry) {
            const std::string value(*entry);
            const auto split = value.find('=');
            if (split != std::string::npos) environment[value.substr(0, split)] = value.substr(split + 1);
        }
        for (const auto& [name, value] : spec.env) environment[name] = value;
        std::vector<std::string> storage;
        for (const auto& [name, value] : environment) storage.push_back(name + "=" + value);
        std::vector<char*> envp;
        for (auto& value : storage) envp.push_back(value.data());
        envp.push_back(nullptr);
        pid_t child = -1;
        rc = posix_spawnp(&child, spec.command.c_str(), &actions, nullptr, argv.data(), envp.data());
        if (rc) throw std::runtime_error("posix_spawnp failed: " + std::string(std::strerror(rc)));
        pid_ = child;
        exited_ = false;
        exit_code_ = -1;
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    ::close(pipes.child_stdin);
    ::close(pipes.child_stdout);
    ::close(pipes.child_stderr);
}

void Subprocess::configure_parent_handles(int input, int output, int error) {
    stdin_write_fd_ = input;
    stdout_read_fd_ = output;
    stderr_read_fd_ = error;
}

int Subprocess::poll_exit_locked() {
    if (exited_) return exit_code_;
    if (pid_ <= 0) return -1;
    int status{};
    pid_t result;
    do { result = ::waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == pid_) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
            : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        exited_ = true;
        pid_ = -1; // Reaped PIDs must never be signalled again.
    } else if (result < 0 && errno == ECHILD) {
        pid_ = -1;
        exited_ = true;
    }
    return exited_ ? exit_code_ : -1;
}
#endif

int Subprocess::wait(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    for (;;) {
        {
            std::lock_guard lock(state_mutex_);
            const int code = poll_exit_locked();
            if (exited_) return code;
#if defined(_WIN32)
            if (!handle_) return -1;
#else
            if (pid_ <= 0) return -1;
#endif
        }
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool Subprocess::exited() {
    std::lock_guard lock(state_mutex_);
    (void)poll_exit_locked();
#if defined(_WIN32)
    return exited_ || !handle_;
#else
    return exited_ || pid_ <= 0;
#endif
}
bool Subprocess::running() { return !exited(); }
int Subprocess::exit_code() const {
    std::lock_guard lock(state_mutex_);
    return exited_ ? exit_code_ : -1;
}

void Subprocess::terminate() {
    std::lock_guard lock(state_mutex_);
    (void)poll_exit_locked();
#if defined(_WIN32)
    if (handle_) TerminateProcess(handle_, 1);
#else
    if (pid_ > 0) ::kill(pid_, SIGTERM);
#endif
}

void Subprocess::close_pipes() {
#if defined(_WIN32)
    for (auto* pipe : {&stdin_write_, &stdout_read_, &stderr_read_}) {
        if (*pipe) CloseHandle(std::exchange(*pipe, nullptr));
    }
#else
    for (auto* pipe : {&stdin_write_fd_, &stdout_read_fd_, &stderr_read_fd_}) {
        if (*pipe >= 0) ::close(std::exchange(*pipe, -1));
    }
#endif
}

int Subprocess::read_stdout(char* buffer, std::size_t size) {
#if defined(_WIN32)
    if (!stdout_read_) return -1;
    // Drain stderr as well: an unread diagnostic pipe can block the child.
    DWORD count{}, available{};
    if (stderr_read_ && PeekNamedPipe(stderr_read_, nullptr, 0, nullptr, &available, nullptr) && available) {
        char discard[4096];
        ReadFile(stderr_read_, discard, std::min<DWORD>(available, sizeof(discard)), &count, nullptr);
    }
    if (!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); return -2; }
    if (!ReadFile(stdout_read_, buffer, static_cast<DWORD>(std::min<std::size_t>(available, size)), &count, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return static_cast<int>(count);
#else
    if (stdout_read_fd_ < 0) return -1;
    pollfd pipes[]{{stdout_read_fd_, POLLIN, 0}, {stderr_read_fd_, POLLIN, 0}};
    int ready;
    do { ready = ::poll(pipes, 2, 100); } while (ready < 0 && errno == EINTR);
    if (ready < 0) return -1;
    if (ready == 0) return -2;
    if (pipes[1].revents & POLLIN) {
        char discard[4096];
        const auto discarded = ::read(stderr_read_fd_, discard, sizeof(discard));
        if (discarded == 0 || (discarded < 0 && errno != EINTR))
            ::close(std::exchange(stderr_read_fd_, -1));
    } else if (pipes[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        ::close(std::exchange(stderr_read_fd_, -1));
    }
    if (!(pipes[0].revents & (POLLIN | POLLHUP | POLLERR))) return -2;
    ssize_t count;
    do { count = ::read(stdout_read_fd_, buffer, size); } while (count < 0 && errno == EINTR);
    return static_cast<int>(count);
#endif
}

bool Subprocess::write_stdin(const char* data, std::size_t size) {
#if defined(_WIN32)
    if (!stdin_write_) return false;
    std::size_t offset{};
    while (offset < size) {
        DWORD count{};
        if (!WriteFile(stdin_write_, data + offset, static_cast<DWORD>(std::min<std::size_t>(size - offset, MAXDWORD)), &count, nullptr) || !count)
            return false;
        offset += count;
    }
    return true;
#else
    if (stdin_write_fd_ < 0) return false;
    // A dead backend must return an I/O error, not terminate ARN via SIGPIPE.
    // Block only this thread, and consume only a signal produced by this write.
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous)) return false;
    sigpending(&pending);
    const bool already_pending = sigismember(&pending, SIGPIPE);
    bool ok = true;
    std::size_t offset{};
    while (offset < size) {
        const auto count = ::write(stdin_write_fd_, data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(count);
    }
    sigpending(&pending);
    if (!already_pending && sigismember(&pending, SIGPIPE)) {
        int signal{};
        (void)sigwait(&blocked, &signal);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return ok;
#endif
}

bool executable_on_path(const std::string& name,
                        const std::vector<std::string>& extra_paths) {
    if (name.empty()) return false;
    if (has_path_separators(name)) {
        std::error_code ec;
        return std::filesystem::exists(name, ec) &&
               std::filesystem::is_regular_file(name, ec)
#if !defined(_WIN32)
               && ::access(name.c_str(), X_OK) == 0
#endif
               ;
    }
    const char* path_env = std::getenv("PATH");
    std::string path_value = path_env ? path_env : "";
    for (const auto& extra : extra_paths) {
        if (!path_value.empty()) path_value +=
#if defined(_WIN32)
            ';'
#else
            ':'
#endif
            ;
        path_value += extra;
    }
#if defined(_WIN32)
    const char sep = ';';
    const std::array<const char*, 4> suffixes = {"", ".exe", ".cmd", ".bat"};
#else
    const char sep = ':';
    const std::array<const char*, 1> suffixes = {""};
#endif
    std::size_t cursor = 0;
    while (cursor <= path_value.size()) {
        const std::size_t end = path_value.find(sep, cursor);
        const std::string dir = path_value.substr(cursor,
            end == std::string::npos ? std::string::npos : end - cursor);
        cursor = (end == std::string::npos) ? path_value.size() + 1 : end + 1;
        if (dir.empty()) continue;
        for (const char* suffix : suffixes) {
            std::error_code ec;
            const std::filesystem::path candidate =
                std::filesystem::path(dir) / (name + suffix);
            if (std::filesystem::exists(candidate, ec) &&
                std::filesystem::is_regular_file(candidate, ec)
#if !defined(_WIN32)
                && ::access(candidate.c_str(), X_OK) == 0
#endif
                ) {
                return true;
            }
        }
    }
    return false;
}

} // namespace arn::acp
