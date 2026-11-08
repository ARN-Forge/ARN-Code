// A stand-in for `kiro-cli` on Windows, for the real-console tests: an
// authenticated account, one model, and `acp` runs tests/acp_fake_server.mjs
// (path in ARN_FAKE_ACP_SERVER) with node. The server runs in a job object
// that is closed with this process, so stopping the backend stops node too.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const std::string_view command = argv[1];
    if (command == "whoami") {
        std::puts("console-test@example.invalid");
        return 0;
    }
    if (command == "chat") {
        std::puts("[\"fake-model-a\"]");
        return 0;
    }
    if (command != "acp") return 1;

    char server[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("ARN_FAKE_ACP_SERVER", server, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 2;

    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) return 3;

    STARTUPINFOA startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::string line = std::string("node \"") + server + "\"";
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return 4;
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 5;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    return static_cast<int>(code);
}
