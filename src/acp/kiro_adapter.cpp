#include "acp/kiro_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace arn::acp {

namespace {

std::string trim_copy(const std::string& input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return input.substr(begin, end - begin);
}

} // namespace

namespace {

struct CommandResult {
    std::string output;
    int exit_code = -1;
};

CommandResult read_command_output(const std::filesystem::path& executable,
                                const std::vector<std::string>& args,
                                int timeout_ms) {
    CommandResult result;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) return result;
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

    std::ostringstream cmd;
    cmd << '"' << executable.string() << '"';
    for (const auto& a : args) {
        bool needs_quote = a.find_first_of(" \t\"&|<>^") != std::string::npos;
        if (needs_quote) cmd << " \"" << a << '"';
        else cmd << ' ' << a;
    }
    std::string command_line = cmd.str();
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = child_stdout_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(child_stdout_write);
        CloseHandle(child_stdout_read);
        return result;
    }
    CloseHandle(child_stdout_write);
    std::string& output = result.output;
    constexpr std::size_t buffer_size = 4096;
    std::array<char, buffer_size> buffer{};
    DWORD start = GetTickCount();
    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(child_stdout_read, nullptr, 0, nullptr, &available, nullptr)
            && available > 0) {
            DWORD bytes_read = 0;
            if (!ReadFile(child_stdout_read, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) || bytes_read == 0)
                break;
            output.append(buffer.data(), bytes_read);
        } else {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            if (GetTickCount() - start > static_cast<DWORD>(timeout_ms)) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
            Sleep(20);
        }
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(child_stdout_read);
    result.exit_code = static_cast<int>(exit_code);
    return result;
#else
    // Use the same spawn/FD ownership path as ACP; no fork in a threaded host.
    struct Fds {
        std::array<int, 4> values{-1, -1, -1, -1};
        ~Fds() { for (int fd : values) if (fd >= 0) ::close(fd); }
    } owned;
    owned.values[0] = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    owned.values[1] = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (owned.values[0] < 0 || owned.values[1] < 0 || ::pipe(owned.values.data() + 2)) return result;
    for (int fd : {owned.values[2], owned.values[3]})
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return result;
    Subprocess process;
    try {
        process.spawn({executable.string(), args, {}},
                      {owned.values[0], owned.values[3], owned.values[1]});
        owned.values[0] = owned.values[1] = owned.values[3] = -1;
        process.configure_parent_handles(-1, owned.values[2], -1);
        owned.values[2] = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::array<char, 4096> buffer;
        while (std::chrono::steady_clock::now() < deadline) {
            const int count = process.read_stdout(buffer.data(), buffer.size());
            if (count > 0) result.output.append(buffer.data(), static_cast<std::size_t>(count));
            else if (count != -2) break;
        }
        const int remaining = static_cast<int>(std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()));
        result.exit_code = process.wait(remaining);
        if (result.exit_code < 0) process.terminate();
    } catch (const std::exception&) {}
    return result;

#endif
}

std::filesystem::path resolve_executable(const std::string& override_env,
                                         const std::string& default_name) {
    if (!override_env.empty()) {
        std::error_code ec;
        const auto p = std::filesystem::path(override_env);
        if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec))
            return std::filesystem::absolute(p);
    }
    if (default_name.empty()) return {};
    const std::vector<std::string> extra;
    if (executable_on_path(default_name, extra)) {
        return std::filesystem::path(default_name);
    }
    return {};
}

} // namespace

KiroAvailability probe_kiro_cli() {
    KiroAvailability result;
    const char* override_env = std::getenv("ARN_KIRO_BIN");
    auto resolved = resolve_executable(override_env ? std::string(override_env) : std::string{},
                                       "kiro-cli");
    if (resolved.empty()) {
        result.status = KiroStatus::not_installed;
        result.detail = "Kiro CLI was not found. Install and authenticate Kiro CLI first.";
        return result;
    }
    result.resolved_path = resolved.string();
    // `kiro-cli whoami` is the documented non-interactive probe. A non-zero
    // exit code or a "Not logged in" payload indicates the user has not yet
    // authenticated; either case must NOT be promoted to KiroStatus::ready,
    // otherwise downstream code attempts an ACP handshake against an
    // unauthenticated Kiro CLI and hangs on interactive auth prompts.
    const auto whoami = read_command_output(resolved, {"whoami"}, 8000);
    if (whoami.exit_code != 0 || whoami.output.empty()) {
        result.status = KiroStatus::not_authenticated;
        result.detail = "Kiro CLI is installed but `kiro-cli whoami` reports it is "
                        "not authenticated. Run `kiro-cli login` first.";
        return result;
    }
    const auto trimmed = trim_copy(whoami.output);
    if (trimmed.find("Not logged in") != std::string::npos) {
        result.status = KiroStatus::not_authenticated;
        result.detail = trimmed;
        return result;
    }
    result.status = KiroStatus::ready;
    result.detail = trimmed;
    return result;
}

KiroAvailability KiroAdapter::resolve() { return probe_kiro_cli(); }

SubprocessSpec KiroAdapter::make_spec(const std::filesystem::path& kiro_cli,
                                      const std::optional<std::string>& agent_name,
                                      const std::optional<std::string>& model) {
    SubprocessSpec spec;
    spec.command = kiro_cli.string();
    spec.args = {"acp"};
    if (agent_name && !agent_name->empty()) {
        spec.args.push_back("--agent");
        spec.args.push_back(*agent_name);
    }
    if (model && !model->empty()) {
        spec.args.push_back("--model");
        spec.args.push_back(*model);
    }
    return spec;
}

std::vector<std::string> KiroAdapter::list_models(const std::filesystem::path& kiro_cli) {
    if (kiro_cli.empty()) return {};
    const auto result = read_command_output(kiro_cli,
        {"chat", "--list-models", "-f", "json"}, 8000);
    if (result.exit_code != 0 || result.output.empty()) return {};
    const auto& output = result.output;
    try {
        auto parsed = nlohmann::json::parse(output);
        std::vector<std::string> models;
        auto ingest = [&](const nlohmann::json& entry) {
            if (entry.is_string()) {
                models.push_back(entry.get<std::string>());
            } else if (entry.is_object()) {
                if (entry.contains("modelId") && entry["modelId"].is_string())
                    models.push_back(entry["modelId"].get<std::string>());
                else if (entry.contains("id") && entry["id"].is_string())
                    models.push_back(entry["id"].get<std::string>());
                else if (entry.contains("name") && entry["name"].is_string())
                    models.push_back(entry["name"].get<std::string>());
            }
        };
        if (parsed.is_array()) {
            for (const auto& entry : parsed) ingest(entry);
        } else if (parsed.is_object()) {
            for (const auto& key : {"models", "data", "items", "results"}) {
                if (parsed.contains(key) && parsed[key].is_array()) {
                    for (const auto& entry : parsed[key]) ingest(entry);
                    break;
                }
            }
        }
        return models;
    } catch (...) {
        // Fallback: parse line by line.
        std::vector<std::string> models;
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line)) {
            line = trim_copy(line);
            if (line.empty()) continue;
            if (line.front() == '-') line.erase(0, 1);
            line = trim_copy(line);
            if (!line.empty()) models.push_back(line);
        }
        return models;
    }
}

std::string KiroAdapter::agent_label() { return "kiro-cli"; }

} // namespace arn::acp
