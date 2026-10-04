#define NOMINMAX
#include <windows.h>
#include "terminal.hpp"
#include "cli_support.hpp"
#include "terminal_ui.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using T = arn::TerminalEventType;
using namespace std::chrono_literals;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

INPUT_RECORD key(WORD vk, wchar_t value, bool down = true, DWORD modifiers = 0) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent = {static_cast<BOOL>(down), 1, vk, 0, {}, modifiers};
    record.Event.KeyEvent.uChar.UnicodeChar = value;
    return record;
}

void write(const std::vector<INPUT_RECORD>& events) {
    DWORD count{};
    check(WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), events.data(),
                            static_cast<DWORD>(events.size()), &count) && count == events.size(),
          "WriteConsoleInputW failed");
}

arn::TerminalEvent editor_event(arn::TerminalSession& terminal) {
    // AllocConsole/console mode changes can enqueue resize records. Like the
    // real command editor, handle these independently from keyboard input.
    for (;;) {
        auto event = terminal.read_event();
        if (event.type != T::resize) return event;
    }
}

void confirmation(arn::TerminalSession& terminal, std::vector<INPUT_RECORD> events,
                  int expected, const std::vector<std::string>& expected_display) {
    // Pending records from a previous request must not decide this request.
    write({key('Y', L'y'), key(VK_RETURN, L'\r')});
    bool started = false;
    std::vector<std::string> displayed;
    const auto start = std::chrono::steady_clock::now();
    const int answer = terminal.read_confirmation([&](std::string_view pending) {
        displayed.emplace_back(pending);
        if (!started) { started = true; write(events); }
    });
    check(answer == expected, "Console confirmation decision differs");
    check(displayed == expected_display, "Console confirmation echo differs");
    check(std::chrono::steady_clock::now() - start < 200ms,
          "Console confirmation introduced a submission delay");
}

class ConsoleOutput final : public std::streambuf {
    HANDLE output_;
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        DWORD written{};
        return WriteFile(output_, data, static_cast<DWORD>(size), &written, nullptr) ? written : 0;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }
public:
    explicit ConsoleOutput(HANDLE output) : output_(output) {}
};

void console_ui_smoke() {
    const auto previous_output = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto output = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);
    check(output != INVALID_HANDLE_VALUE, "Could not allocate UI console buffer");
    ConsoleOutput writer(output);
    auto* previous_writer = std::cout.rdbuf(&writer);
    SetStdHandle(STD_OUTPUT_HANDLE, output);
    try {
        SMALL_RECT window{0, 0, 79, 24};
        check(SetConsoleWindowInfo(output, TRUE, &window), "Could not size console viewport");
        check(SetConsoleScreenBufferSize(output, {100, 120}), "Could not size console history");
        window = {0, 0, 99, 29};
        check(SetConsoleWindowInfo(output, TRUE, &window), "Could not size UI viewport");
        arn::TerminalSession terminal;
        arn::terminal_ui::Ui ui(terminal);
        ui.session("Gemini", "fake-model", false);
        ui.status(ui.ready_status());
        const auto line = [&](int row) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            check(GetConsoleScreenBufferInfo(output, &info), "Could not inspect rendered viewport");
            std::wstring text(100, L' ');
            DWORD count{};
            const COORD position{0, static_cast<SHORT>(info.srWindow.Top + row)};
            check(ReadConsoleOutputCharacterW(output, text.data(), 100, position, &count),
                  "Could not read rendered UI");
            return text;
        };
        ui.render();
        check(line(0).find(L'╭') != std::wstring::npos && line(8).find(L'╰') != std::wstring::npos,
              "Real Windows console lost header borders");
        check(line(3).find(L"▄▄          ▄▄") != std::wstring::npos,
              "Real Windows console lost Arny");
        check(line(7).find(L"Model: fake-model") != std::wstring::npos,
              "Real Windows console lost session metadata");
        check(line(27).find(L"arn › ") == 0 && line(28).find(L"Type /help") == 0,
              "Real Windows console misplaced the input area");
        ui.add(arn::terminal_ui::Tone::normal, {});
        ui.append("Вітаю"); ui.render();
        ui.append(" вас!"); ui.render();
        check(line(10).find(L"Вітаю вас!") != std::wstring::npos,
              "Real Windows console broke streamed text");
        ui.status("Allow this tool call? [y/N]");
        ui.show_confirmation("y");
        check(line(27).find(L"arn › y") == 0 && line(28).find(L"[y/N]") != std::wstring::npos,
              "Real Windows console did not display confirmation");
        ui.show_confirmation({});
        check(line(27).find(L"arn ›   ") == 0, "Real Windows console did not erase confirmation");
        ui.finish_confirmation('\n');
        ui.status(ui.ready_status()); ui.render();
        check(line(10).find(L"Вітаю вас!") != std::wstring::npos,
              "Real Windows console confirmation damaged conversation");
    } catch (...) {
        std::cout.rdbuf(previous_writer);
        SetStdHandle(STD_OUTPUT_HANDLE, previous_output);
        CloseHandle(output);
        throw;
    }
    std::cout.rdbuf(previous_writer);
    SetStdHandle(STD_OUTPUT_HANDLE, previous_output);
    CloseHandle(output);
}

void transcript_smoke(const char* executable) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader{}, writer{};
    check(CreatePipe(&reader, &writer, &attributes, 0), "CreatePipe failed");
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = startup.hStdError = writer;
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + executable + "\"";
    check(CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                         nullptr, nullptr, &startup, &process), "Could not start real ARN");
    CloseHandle(writer);
    std::string output;
    auto wait_for = [&](std::string_view expected, std::size_t after = 0) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available{};
            if (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) {
                char buffer[4096];
                DWORD count{};
                if (ReadFile(reader, buffer, sizeof(buffer), &count, nullptr)) output.append(buffer, count);
            }
            if (output.find(expected, after) != std::string::npos) return;
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("Real ARN did not render expected state: " + std::string(expected));
    };
    auto type = [&](std::wstring_view command) {
        std::vector<INPUT_RECORD> events;
        for (wchar_t value : command) events.push_back(key(0, value));
        events.push_back(key(VK_RETURN, 0));
        write(events);
    };
    try {
        wait_for("Type /help for commands");
        check(output.find("ARN AGENT CODE") != std::string::npos,
              "Windows lost the original ARN header");
        check(output.find("▄▀██▄▀██▀▄██▀▄") != std::string::npos,
              "Windows lost the Arny mascot");
        check(output.find("╭") != std::string::npos && output.find("╰") != std::string::npos,
              "Windows lost the header borders");
        check(output.find("Provider: none") != std::string::npos &&
              output.find("Model: not selected") != std::string::npos &&
              output.find("Context: empty") != std::string::npos,
              "Windows lost session information");
        for (int i = 0; i < 3; ++i) {
            const auto after = output.size();
            type(L"/help");
            wait_for("Credentials:", after);
        }
        auto after = output.size();
        type(L"/provider omniroute");
        wait_for("Active provider: OmniRoute", after);
        after = output.size();
        type(L"/key-omniroute");
        wait_for("API key: type securely below", after);
        write({key('X', L'X'), key('X', L'X'), key(VK_BACK, 8), key(VK_ESCAPE, 27)});
        wait_for("cancelled", after);
        after = output.size();
        type(L"Привіт"); // No configured key: this never starts a provider request.
        wait_for("Select a provider first", after);
        check(output.find("Привіт", after) != std::string::npos, "Real ARN lost Ukrainian input");
        type(L"/exit");
        check(WaitForSingleObject(process.hProcess, 3000) == WAIT_OBJECT_0, "ARN did not exit");
        check(output.find("1049h") == std::string::npos && output.find("\x1b[2J") == std::string::npos,
              "Real ARN still replaces terminal history");
        check(output.find("F2 copy mode") == std::string::npos, "Windows still advertises copy mode");
        check(output.find("XX") == std::string::npos, "Secure entry emitted plaintext");
    } catch (...) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(reader);
        throw;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(reader);
}

int main(int argc, char** argv) {
    // CTest generally has redirected stdin; allocate an isolated console.
    // No human interaction or visible window is required.
    FreeConsole();
    check(AllocConsole() != FALSE, "AllocConsole failed");
    if (const auto window = GetConsoleWindow()) ShowWindow(window, SW_HIDE);
    // AllocConsole preserves STARTF_USESTDHANDLES handles supplied by CTest.
    // Explicitly open the newly allocated console rather than its stdin pipe.
    const auto original_input = GetStdHandle(STD_INPUT_HANDLE);
    const auto console_input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(console_input != INVALID_HANDLE_VALUE, "Open CONIN$ failed");
    SetHandleInformation(console_input, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetStdHandle(STD_INPUT_HANDLE, console_input);
    std::ostringstream captured;
    auto* original = std::cout.rdbuf(captured.rdbuf());
    try {
        {
            arn::TerminalSession terminal;
            DWORD mode{};
            check(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode), "No console mode");
            check(!(mode & ENABLE_MOUSE_INPUT), "ARN still owns the mouse");
            check(!(mode & (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)), "Input must be explicitly rendered");
            check(mode & ENABLE_PROCESSED_INPUT, "Selection-aware Ctrl+C must remain processed");
            check(terminal.native_scrollback() && !terminal.copy_mode(), "Native scrollback not active");

            check(FlushConsoleInputBuffer(console_input), "Could not clear console setup events");
            INPUT_RECORD resize{};
            resize.EventType = WINDOW_BUFFER_SIZE_EVENT;
            resize.Event.WindowBufferSizeEvent.dwSize = {100, 30};
            // Reproduce the event ordering that the old text-only assertion
            // incorrectly treated as a key-up failure on a headless runner.
            write({resize, key('Z', L'z', false), key('A', L'a'),
                   key(VK_RETURN, L'\r', false), resize, key(0, L'П'),
                   key(VK_RETURN, 0)});
            const auto typed = editor_event(terminal);
            check(typed.type == T::character && typed.text == "a", "Key-up must not enter the editor");
            const auto ukrainian = editor_event(terminal);
            check(ukrainian.type == T::character && ukrainian.text == "П",
                  "Key-up Enter or resize displaced Ukrainian input");
            check(editor_event(terminal).type == T::enter, "VK_RETURN with empty UnicodeChar must submit");
            write({key(0, L'\n')});
            check(editor_event(terminal).type == T::enter, "LF must submit");

            confirmation(terminal, {key('Y', L'y'), key(VK_RETURN, 0)}, 'y', {"", "y"});
            confirmation(terminal, {key('N', L'n'), key(VK_RETURN, L'\r')}, 'n', {"", "n"});
            confirmation(terminal, {key(VK_RETURN, 0)}, '\n', {""});
            confirmation(terminal, {key('Y', L'y'), key(VK_BACK, 8), key('N', L'n'),
                                   key(VK_RETURN, 0)}, 'n', {"", "y", "", "n"});
            confirmation(terminal, {key(VK_ESCAPE, 27)}, 27, {""});
            confirmation(terminal, {key('C', 3, true, LEFT_CTRL_PRESSED)}, 3, {""});

            // ACP EOF/shutdown cancels a pending decision without requiring a
            // new console event. A typed y without Enter must not grant access.
            {
                std::atomic_bool stopped{false};
                std::jthread transport_stop([&] {
                    std::this_thread::sleep_for(30ms);
                    stopped.store(true);
                });
                std::vector<std::string> displayed;
                const auto start = std::chrono::steady_clock::now();
                const int answer = terminal.read_confirmation([&](std::string_view choice) {
                    displayed.emplace_back(choice);
                    if (displayed.size() == 1) write({key('Y', L'y')});
                }, &stopped);
                check(answer == 3 && displayed == std::vector<std::string>{"", "y"},
                      "Transport cancellation accepted an unfinished confirmation");
                check(std::chrono::steady_clock::now() - start < 250ms,
                      "Transport cancellation did not wake console input");
            }

            arn::SecretInputBuffer secret;
            write({key('X', L'X'), key('X', L'X'), key(VK_BACK, 8), key(VK_RETURN, 0)});
            while (secret.state() == arn::SecretInputState::editing) secret.consume(terminal.read_event());
            check(secret.masked() == "*", "Secure input masking/backspace regressed");
            check(secret.take_submitted_secret() == "X", "Secure input submission regressed");

            // Processed Ctrl+C is delivered by the console control handler,
            // not a KEY_EVENT. This must also wake secure input at the prompt.
            check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "Could not generate Ctrl+C");
            check(terminal.read_event().type == T::interrupt, "Processed Ctrl+C did not wake input");

            for (const auto event : {key(VK_ESCAPE, 27), key('C', 3, true, LEFT_CTRL_PRESSED)}) {
                std::atomic_bool cancelled = false;
                std::atomic_bool callback = false;
                {
                    arn::TerminalCancellationMonitor monitor(cancelled, [&] { callback.store(true); });
                    monitor.pause_input();
                    confirmation(terminal, {key('Y', L'y'), key(VK_RETURN, 0)}, 'y', {"", "y"});
                    monitor.resume_input();
                    write({event});
                    const auto deadline = std::chrono::steady_clock::now() + 1s;
                    while (!callback.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(1ms);
                    check(cancelled.load() && callback.load(), "Request cancellation was not delivered");
                }
            }
            std::atomic_bool cancelled = false;
            std::atomic_bool callback = false;
            {
                arn::TerminalCancellationMonitor monitor(cancelled, [&] { callback.store(true); });
                check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "Could not generate request Ctrl+C");
                const auto deadline = std::chrono::steady_clock::now() + 1s;
                while (!callback.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                check(cancelled.load() && callback.load(), "Processed request Ctrl+C did not cancel");
            }
        }
        check(captured.str().find("1049h") == std::string::npos, "Windows still enters alternate screen");
        console_ui_smoke();
        if (argc > 1) transcript_smoke(argv[1]);
        std::cout.rdbuf(original);
        SetStdHandle(STD_INPUT_HANDLE, original_input);
        CloseHandle(console_input);
        FreeConsole();
        return 0;
    } catch (const std::exception& error) {
        std::cout.rdbuf(original);
        std::cerr << error.what() << '\n';
        SetStdHandle(STD_INPUT_HANDLE, original_input);
        CloseHandle(console_input);
        FreeConsole();
        return 1;
    }
}
