#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace arn::acp {

// Configuration for spawning an ACP-compatible subprocess.
struct SubprocessSpec {
    // Executable path or name resolvable on PATH.
    std::string command;
    // Arguments. The first item is conventionally the ACP subcommand (e.g. "acp").
    std::vector<std::string> args;
    // Optional environment overrides. PATH and TMP are inherited by default.
    std::vector<std::pair<std::string, std::string>> env;
};

// Bidirectional pipe endpoints for the child process.
//
// On Windows, fields carry OS HANDLEs (cast to void* to keep the struct
// platform-portable). On POSIX, they carry int file descriptors. Endpoints
// are described from the CHILD's perspective so they can be passed directly
// to CreateProcessA's STARTUPINFO / dup2 onto STDIN/STDOUT/STDERR:
//   child_stdin  : the handle/FD the child reads standard input from
//                  (the read end of the parent's stdin pipe).
//   child_stdout : the handle/FD the child writes standard output to
//                  (the write end of the parent's stdout pipe).
//   child_stderr : the handle/FD the child writes standard error to
//                  (the write end of the parent's stderr pipe).
struct SubprocessPipes {
#if defined(_WIN32)
    void* child_stdin  = nullptr;
    void* child_stdout = nullptr;
    void* child_stderr = nullptr;
#else
    int child_stdin  = -1;
    int child_stdout = -1;
    int child_stderr = -1;
#endif
};

// Lifecycle owner for a spawned ACP backend. On Windows and POSIX the
// implementation captures the child handle and terminates it on destruction.
// Process-state operations are serialized. Pipe ownership is transferred before
// starting I/O; close_pipes() must run after the reader has stopped.
class Subprocess {
public:
    Subprocess() = default;
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;
    Subprocess(Subprocess&& other) noexcept;
    Subprocess& operator=(Subprocess&& other) noexcept;
    ~Subprocess();

    // Consumes child pipe ends on success only. The caller owns them on failure.
    void spawn(const SubprocessSpec& spec, SubprocessPipes pipes);

    // Transfer ownership of parent pipe ends on either platform.
#if defined(_WIN32)
    void configure_parent_handles(void* parent_stdin_writer,
                                  void* parent_stdout_reader,
                                  void* parent_stderr_reader);
#else
    void configure_parent_handles(int parent_stdin_writer,
                                  int parent_stdout_reader,
                                  int parent_stderr_reader);
#endif

    // Wait up to timeout_ms for the process to exit. Returns the exit code or
    // -1 if still running/unstarted. Repeated checks preserve the cached code.
    int wait(int timeout_ms);

    // Returns true if the process has exited.
    bool exited();

    // Returns true if the process is currently running (or still attached).
    bool running();

    // Returns the exit code if exited() is true, otherwise -1.
    int exit_code() const;

    // Terminate the process (SIGTERM / TerminateProcess). Idempotent.
    void terminate();

    // Close the parent ends of the pipes. Subsequent reads/writes will fail.
    void close_pipes();

    // Read up to buffer_size bytes from stdout. Returns the number of bytes
    // read, 0 on EOF, -1 on error, or -2 if no data is ready yet.
    int read_stdout(char* buffer, std::size_t buffer_size);

    // Write exactly data_size bytes to stdin. Returns true on success.
    bool write_stdin(const char* data, std::size_t data_size);

private:
    void release() noexcept;
    int poll_exit_locked();
    mutable std::mutex state_mutex_;
#if defined(_WIN32)
    void* handle_ = nullptr;     // HANDLE
    void* stdin_write_ = nullptr; // HANDLE
    void* stdout_read_ = nullptr; // HANDLE
    void* stderr_read_ = nullptr; // HANDLE
    int exit_code_ = -1;
    bool exited_ = false;
#else
    pid_t pid_ = -1;
    int stdin_write_fd_ = -1;
    int stdout_read_fd_ = -1;
    int stderr_read_fd_ = -1;
    int exit_code_ = -1;
    bool exited_ = false;
#endif
};

// Returns true when the named executable is discoverable on PATH. The optional
// extra_paths are prepended to PATH for the lookup (useful for tests).
bool executable_on_path(const std::string& name,
                        const std::vector<std::string>& extra_paths = {});

} // namespace arn::acp
