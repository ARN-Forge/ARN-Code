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

INPUT_RECORD key(WORD vk, wchar_t value, bool down = true, DWORD modifiers = 0, WORD repeats = 1) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent = {static_cast<BOOL>(down), repeats, vk, 0, {}, modifiers};
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
    std::string emitted_;
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        DWORD written{};
        if (!WriteFile(output_, data, static_cast<DWORD>(size), &written, nullptr)) return 0;
        emitted_.append(data, written);
        return written;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }
public:
    explicit ConsoleOutput(HANDLE output) : output_(output) {}
    const std::string& emitted() const { return emitted_; }
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
        const auto check_single_banner = [&](const char* stage) {
            // Win32 VT output can scroll the visible region with srWindow.Top
            // unchanged. ReadConsoleOutputCharacterW cannot inspect the host's
            // external scrollback. Count actual writes, even after scrolling;
            // terminal_ui_test separately verifies retained VT history.
            const auto& emitted = writer.emitted();
            const auto banner = emitted.find("ARN AGENT CODE");
            if (banner == std::string::npos)
                throw std::runtime_error(std::string("Real console did not emit startup banner: ") + stage);
            if (emitted.find("ARN AGENT CODE", banner + 1) != std::string::npos)
                throw std::runtime_error(std::string("Real console duplicated startup banner: ") + stage);
        };
        ui.render();
        // Verify backend evidence against an actual Win32 screen buffer. Same
        // geometry/coordinates alone must not certify externally changed cells.
        {
            const auto anchor = terminal.capture_live_anchor(1, 1);
            check(terminal.live_anchor_valid(anchor), "Fresh Win32 anchor is invalid");
            CONSOLE_SCREEN_BUFFER_INFO before{};
            check(GetConsoleScreenBufferInfo(output, &before), "Could not inspect live anchor");
            const auto relocated = COORD{0, static_cast<SHORT>(before.dwCursorPosition.Y - 1)};
            check(SetConsoleCursorPosition(output, relocated), "Could not relocate console cursor");
            check(!terminal.live_anchor_valid(anchor), "Win32 anchor ignored external cursor relocation");
            check(SetConsoleCursorPosition(output, before.dwCursorPosition), "Could not restore test cursor");
            check(terminal.live_anchor_valid(anchor), "Unchanged Win32 region lost its anchor");
            wchar_t original{};
            DWORD count{};
            const COORD changed{0, before.dwCursorPosition.Y};
            check(ReadConsoleOutputCharacterW(output, &original, 1, changed, &count),
                  "Could not inspect live cell");
            check(WriteConsoleOutputCharacterW(output, L"!", 1, changed, &count),
                  "Could not change live cell");
            check(!terminal.live_anchor_valid(anchor), "Win32 anchor ignored changed region contents");
            check(WriteConsoleOutputCharacterW(output, &original, 1, changed, &count),
                  "Could not restore test cell");
        }
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
        check_single_banner("streaming/confirmation");

        // Exercise the production renderer against real console geometry and
        // scrolling. The welcome block must never be appended again.
        for (int turn = 0; turn < 35; ++turn) {
            ui.session("OmniRoute", "fake-model-" + std::to_string(turn), true);
            ui.add(arn::terminal_ui::Tone::user, "offline-turn-" + std::to_string(turn));
            ui.render();
            ui.render();
            check_single_banner(("transcript growth turn " + std::to_string(turn)).c_str());
        }
        for (const auto size : {COORD{70, 24}, COORD{50, 18}, COORD{100, 30}}) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            check(GetConsoleScreenBufferInfo(output, &info), "Could not inspect resize position");
            SMALL_RECT resized{0, info.srWindow.Top, static_cast<SHORT>(size.X - 1),
                               static_cast<SHORT>(info.srWindow.Top + size.Y - 1)};
            check(SetConsoleWindowInfo(output, TRUE, &resized), "Could not resize UI viewport");
            ui.render(); ui.render();
            check_single_banner("viewport resize");
        }
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

void resize_records_without_size_change() {
    // Hosts can repeat a buffer-size record without resizing. That must not
    // invalidate the live region (abandoning its footer); a real change must.
    const auto previous_output = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto output = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);
    check(output != INVALID_HANDLE_VALUE, "Could not allocate resize console buffer");
    ConsoleOutput writer(output);
    auto* previous_writer = std::cout.rdbuf(&writer);
    SetStdHandle(STD_OUTPUT_HANDLE, output);
    const auto restore = [&] {
        std::cout.rdbuf(previous_writer);
        SetStdHandle(STD_OUTPUT_HANDLE, previous_output);
        CloseHandle(output);
    };
    try {
        arn::TerminalSession terminal;
        std::cout << "live-region-row" << std::flush;
        const auto anchor = terminal.capture_live_anchor(0, 0);
        check(terminal.live_anchor_valid(anchor), "Fresh resize-test anchor is invalid");
        CONSOLE_SCREEN_BUFFER_INFO info{};
        check(GetConsoleScreenBufferInfo(output, &info), "Could not inspect buffer size");
        INPUT_RECORD resize{};
        resize.EventType = WINDOW_BUFFER_SIZE_EVENT;
        resize.Event.WindowBufferSizeEvent.dwSize = info.dwSize;
        check(FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)), "Could not clear setup events");
        write({resize, resize});
        for (int record = 0; record < 2; ++record)
            check(terminal.read_event().type == T::resize, "Resize record was not delivered");
        check(terminal.live_anchor_valid(anchor),
              "A resize record with an unchanged buffer size invalidated the live region");
        resize.Event.WindowBufferSizeEvent.dwSize.X = static_cast<SHORT>(info.dwSize.X - 1);
        write({resize});
        check(terminal.read_event().type == T::resize, "Changed resize record was not delivered");
        check(!terminal.live_anchor_valid(anchor), "A buffer-size change did not invalidate the live region");
    } catch (...) {
        restore();
        throw;
    }
    restore();
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
    auto records = [](std::wstring_view text) {
        std::vector<INPUT_RECORD> events;
        for (const wchar_t ch : text) events.push_back(key(0, ch));
        return events;
    };
    auto geometry = [](SHORT width, SHORT height) {
        INPUT_RECORD event{};
        event.EventType = WINDOW_BUFFER_SIZE_EVENT;
        event.Event.WindowBufferSizeEvent.dwSize = {width, height};
        return event;
    };
    auto submitted = [&](std::size_t after) {
        // Transcript's user entry is a single cyan span. Live prompt uses
        // separate cyan/amber spans and cannot match this marker. This checks
        // the actual read_line return value after dispatch, not a live echo.
        const std::string marker = "\x1b[38;5;81m arn › ";
        std::vector<std::string> values;
        auto from = after;
        while ((from = output.find(marker, from)) != std::string::npos) {
            from += marker.size();
            const auto end = output.find("\x1b[0m", from);
            check(end != std::string::npos, "Incomplete submitted input span");
            auto value = output.substr(from, end - from);
            // append_native_text inserts CRLF + a margin at visual wrapping.
            // Windows CRT text-mode stdout can produce CR CR LF in the pipe.
            // read_line does not accept CR/LF inside a submitted logical line.
            value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
            for (auto pos = value.find("\n "); pos != std::string::npos;
                 pos = value.find("\n ")) value.erase(pos, 2);
            values.push_back(std::move(value));
            from = end;
        }
        return values;
    };
    auto reset_input = [&](std::size_t after) {
        wait_for("Input reset required", after);
        const auto boundary = output.size();
        write({key(VK_ESCAPE, 27)});
        wait_for("Input reset complete", boundary);
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
        // Current real editor, with raw console records and pipe output. These
        // resize records test logical state; they do NOT emulate host reflow.
        for (const bool resized : {false, true}) {
            const auto start = output.size();
            auto events = records(L"/pro");
            if (resized) {
                for (const auto size : {COORD{40,16}, {120,45}, {100,30}})
                    events.push_back(geometry(size.X, size.Y));
            }
            events.push_back(key(VK_TAB, L'\t'));
            auto argument = records(L"geminix");
            events.insert(events.end(), argument.begin(), argument.end());
            events.push_back(key(VK_BACK, 8));
            events.push_back(key(VK_RETURN, 0));
            write(events);
            wait_for("Active provider: Gemini", start);
            check(submitted(start) == std::vector<std::string>{"/provider gemini"},
                  "Tab/Backspace/resize changed the real command buffer or replayed submission");
        }
        {
            const auto start = output.size();
            const std::wstring text = L"/unknown_" + std::wstring(150, L'a') + L"_ПривітX";
            auto events = records(text);
            events.push_back(geometry(40, 16));
            events.push_back(key(VK_BACK, 8));
            events.push_back(geometry(120, 45));
            events.push_back(key(VK_RETURN, 0));
            write(events);
            wait_for("Unknown command. Type /help", start);
            const auto actual = submitted(start);
            const auto expected = "/unknown_" + std::string(150, 'a') + "_Привіт";
            check(actual == std::vector<std::string>{expected},
                  "Long Ukrainian input lost data across resize/edit/submit");
        }
        {
            const auto start = output.size();
            // Pipe-output size is 100: current footer splits input at 92
            // characters. Cross that logical tail boundary in both directions.
            auto events = records(L"/unknown" + std::wstring(84, L'q') + L"X");
            events.push_back(key(VK_BACK, 8));
            events.push_back(key(VK_BACK, 8));
            auto suffix = records(L"YZ\r");
            events.insert(events.end(), suffix.begin(), suffix.end());
            write(events);
            wait_for("Unknown command. Type /help", start);
            check(submitted(start) == std::vector<std::string>{
                      "/unknown" + std::string(83, 'q') + "YZ"},
                  "Editing across the logical wrap boundary corrupted submitted input");
        }
        {
            const auto start = output.size();
            // CR/LF records each submit a line; there is no paste transaction.
            auto events = records(L"\r\n/status\r\n/help\r\n");
            write(events);
            wait_for("Credentials:", start);
            check(submitted(start) == std::vector<std::string>{"/status", "/help"},
                  "Multiline paste/empty Enter semantics changed or duplicated commands");
        }
        for (const auto stop : {key(VK_ESCAPE, 27), key('C', 3, true, LEFT_CTRL_PRESSED)}) {
            for (const auto draft : {L"Привіт", L"/provider ki", L"/key-gemini FAKE_CANCELLED"}) {
                const auto start = output.size();
                auto events = records(draft);
                events.push_back(geometry(70, 20));
                events.push_back(stop);
                events.push_back(key(VK_RETURN, 0)); // Cancel must not submit the old draft.
                auto suffix = records(L"/status\r");
                events.insert(events.end(), suffix.begin(), suffix.end());
                write(events);
                wait_for("API key: not set", start);
                check(submitted(start) == std::vector<std::string>{"/status"},
                      "Cancellation after resize submitted a draft or terminated ARN");
                check(output.find("FAKE_CANCELLED", start) == std::string::npos,
                      "Cancelling credential-like draft exposed it");
            }
        }
        {
            const auto start = output.size();
            auto events = records(L"/unknown ");
            events.push_back(key('R', L'R', true, 0, 4));
            events.push_back(key(0, L'К', true, 0, 3));
            events.push_back(key(VK_PACKET, 0xD83D, true, 0, 2));
            events.push_back(key(VK_PACKET, 0xDE42, true, 0, 2));
            events.push_back(key(VK_BACK, 8, true, 0, 2));
            events.push_back(key(VK_RETURN, 0, true, 0, 100));
            auto next_command = records(L"/status\r");
            events.insert(events.end(), next_command.begin(), next_command.end());
            write(events);
            wait_for("API key: not set", start);
            check(submitted(start) == std::vector<std::string>{"/unknown RRRRККК", "/status"},
                  "Repeat count corrupted input/backspace or repeated Enter submission");
        }
        for (const std::wstring canonical : {
                 L"/key-gemini", L"/key-deepseek", L"/key-openrouter", L"/key-omniroute"}) {
            for (const auto spelling : {canonical, canonical.substr(1)}) {
                const auto start = output.size();
                auto events = records(spelling + L" FAKE_INLINE_TOKEN_X");
                events.push_back(geometry(50, 18));
                events.push_back(key(VK_BACK, 8));
                events.push_back(geometry(100, 30));
                events.push_back(key(VK_RETURN, 0));
                write(events);
                wait_for("Inline API key entry is disabled", start);
                check(output.find("FAKE_INLINE_TOKEN", start) == std::string::npos,
                      "Key command/alias leaked plaintext before dispatch");
                const auto safe = submitted(start);
                check(safe.size() == 1 && safe.front().starts_with("/key-") &&
                      safe.front().find(' ') == std::string::npos,
                      "Rejected inline key was retained in transcript");
                reset_input(start);
            }
            for (const auto spelling : {canonical, canonical.substr(1)}) {
                for (const std::wstring delimiter : {L"\r", L"\n", L"\r\n"}) {
                    const auto start = output.size();
                    auto variant = spelling;
                    std::transform(variant.begin(), variant.end(), variant.begin(),
                        [](wchar_t ch) { return ch >= L'a' && ch <= L'z' ? ch - L'a' + L'A' : ch; });
                    auto events = records(L"  " + variant + L" \tFAKE_FIRST" + delimiter +
                                          L"FAKE_CONTINUATION" + delimiter);
                    events.push_back(key('X', L'X', true, 0, 4));
                    write(events);
                    wait_for("Input reset required", start);
                    // Deliver a later chunk: queue flushes/time-based guesses
                    // cannot establish the boundary of this input interaction.
                    write(records(L"FAKE_DELAYED_CHUNK\r\n/status\r"));
                    reset_input(start);
                    check(output.find("FAKE_", start) == std::string::npos,
                          "Multiline credential continuation leaked into visible output");
                    const auto safe = submitted(start);
                    check(safe.size() == 1 && safe.front().starts_with("/key-") &&
                          safe.front().find(' ') == std::string::npos,
                          "Credential continuation became a submitted command");
                    const auto fresh = output.size();
                    type(L"/status");
                    wait_for("API key: not set", fresh);
                    check(submitted(fresh) == std::vector<std::string>{"/status"},
                          "Separate action after explicit reset remained quarantined");
                }
                const auto empty_batch = output.size();
                write(records(spelling + L"\r\nFAKE_AFTER_EMPTY\r\n"));
                wait_for("cancelled or empty", empty_batch);
                reset_input(empty_batch);
                check(output.find("FAKE_AFTER_EMPTY", empty_batch) == std::string::npos &&
                      output.find("Verifying API key", empty_batch) == std::string::npos,
                      "CRLF empty secure prompt exposed or validated its pasted tail");
                const auto leading_tab = output.size();
                write(records(L"\t" + spelling + L" FAKE_TAB_FIRST\r\nFAKE_TAB_TAIL\r\n"));
                reset_input(leading_tab);
                check(output.find("FAKE_TAB", leading_tab) == std::string::npos &&
                      submitted(leading_tab).size() == 1 &&
                      submitted(leading_tab).front().starts_with("/key-"),
                      "Leading paste Tab manufactured a prefix that bypassed key masking");
            }
            const auto start = output.size();
            type(canonical);
            wait_for("API key: type securely below", start);
            auto events = records(L"FAKE_КЛЮЧ_🙂");
            events.push_back(geometry(40, 16));
            events.push_back(key(VK_BACK, 8));
            events.push_back(geometry(100, 30));
            events.push_back(key('X', L'X', true, 0, 4));
            events.push_back(key(VK_BACK, 8, true, 0, 2));
            events.push_back(canonical == L"/key-deepseek"
                ? key('C', 3, true, LEFT_CTRL_PRESSED) : key(VK_ESCAPE, 27));
            write(events);
            wait_for("cancelled", start);
            check(output.find("FAKE", start) == std::string::npos &&
                  output.find("КЛЮЧ", start) == std::string::npos,
                  "Secure paste/resize/backspace/cancellation emitted key text");
            // Pipe stdout is plain output: the masked editor is not echoed at
            // all. Repeat/mask logic is covered by cli_support_test.
            check(output.find('*', start) == std::string::npos,
                  "Plain output echoed the secure editor");
            check(output.find("Verifying API key", start) == std::string::npos,
                  "Offline test must never submit a fake key to a provider");
            reset_input(start);
            const auto empty_start = output.size();
            type(canonical);
            wait_for("API key: type securely below", empty_start);
            write({key(VK_RETURN, 0)});
            wait_for("cancelled or empty", empty_start);
            check(output.find("Verifying API key", empty_start) == std::string::npos,
                  "Empty secure input unexpectedly started provider validation");
            // Empty/cancelled secure entry also must not turn queued trailing
            // text into another ordinary command. No fake key is submitted.
            write(records(L"FAKE_SECURE_TAIL\r\n"));
            reset_input(empty_start);
            check(output.find("FAKE_SECURE_TAIL", empty_start) == std::string::npos,
                  "Secure-input completion exposed its continuation");
        }
        {
            // An Esc queued before the reset prompt is stale: the reset must
            // still wait, so the command after it is discarded, not executed.
            const auto start = output.size();
            auto events = records(L"/key-gemini FAKE_STALE_INLINE\r");
            events.push_back(key(VK_ESCAPE, 27));
            auto tail = records(L"/status\r");
            events.insert(events.end(), tail.begin(), tail.end());
            write(events);
            reset_input(start);
            check(output.find("API key: not set", start) == std::string::npos,
                  "An Esc queued before the reset prompt satisfied the credential reset");
        }
        {
            // reset_input sends Esc the moment the prompt appears. That key is
            // fresh and must complete every reset; discarding stale input
            // after showing the prompt could drop it (an intermittent hang).
            // Repeat to expose that ordering; cli_support_test reproduces it
            // deterministically.
            const auto stress_start = output.size();
            for (int cycle = 0; cycle < 40; ++cycle) {
                const auto start = output.size();
                type(L"/key-gemini FAKE_RACE_INLINE");
                reset_input(start);
            }
            check(output.find("FAKE_RACE", stress_start) == std::string::npos,
                  "Rapid credential resets exposed an inline key");
        }
        auto after = output.size();
        type(L"/provider omniroute");
        wait_for("Active provider: OmniRoute", after);
        after = output.size();
        type(L"/key-omniroute");
        wait_for("API key: type securely below", after);
        write({key('X', L'X'), key('X', L'X'), key(VK_BACK, 8), key(VK_ESCAPE, 27)});
        wait_for("cancelled", after);
        reset_input(after);
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
        // Redirected stdout cannot be read back, so no live input area may be
        // drawn (each one would stack in the byte stream).
        check(output.find("\x1b[38;5;81marn\x1b[38;5;215m › ") == std::string::npos,
              "Redirected output drew a live input area");
    } catch (...) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(reader);
        throw;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(reader);
}

void redirected_stdin_smoke(const char* executable) {
    // The current event reader is a console-only API. Pipe EOF must at least
    // terminate cleanly, rather than spinning or accepting unintended input.
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader{}, writer{};
    check(CreatePipe(&reader, &writer, &attributes, 0), "Could not create EOF pipe");
    CloseHandle(writer);
    const auto sink = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &attributes, OPEN_EXISTING, 0, nullptr);
    if (sink == INVALID_HANDLE_VALUE) {
        CloseHandle(reader);
        throw std::runtime_error("Could not open redirected output sink");
    }
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = reader;
    startup.hStdOutput = startup.hStdError = sink;
    PROCESS_INFORMATION process{};
    std::string command = std::string("\"") + executable + "\"";
    const bool created = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                                       nullptr, nullptr, &startup, &process);
    CloseHandle(reader); CloseHandle(sink);
    check(created, "Could not start pipe-EOF ARN");
    const auto waited = WaitForSingleObject(process.hProcess, 3000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 3000);
    }
    DWORD code{};
    const bool inspected = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    check(waited == WAIT_OBJECT_0 && inspected && code == 0, "Pipe EOF did not exit cleanly");
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
    DWORD original_mode{};
    check(GetConsoleMode(console_input, &original_mode), "Could not record initial console mode");
    const auto original_cp = GetConsoleCP(), original_output_cp = GetConsoleOutputCP();
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

            // Counts are carried atomically within one decoded event, never
            // replayed into a later prompt/permission request.
            for (const auto repeats : {WORD{1}, WORD{4}, WORD{65535}}) {
                write({key('X', L'X', true, 0, repeats), key(0, L'К', true, 0, repeats),
                       key(VK_BACK, 8, true, 0, repeats), key(VK_PRIOR, 0, true, 0, repeats),
                       key(VK_NEXT, 0, true, 0, repeats), key(VK_RETURN, 0, true, 0, repeats),
                       key(VK_TAB, 9, true, 0, repeats), key(VK_END, 0, true, 0, repeats),
                       key(VK_ESCAPE, 27, true, 0, repeats)});
                for (const auto expected : {T::character, T::character, T::backspace, T::page_up, T::page_down}) {
                    const auto decoded = editor_event(terminal);
                    check(decoded.type == expected && decoded.repeat_count == repeats,
                          "Win32 character/edit/navigation repeat was lost");
                }
                for (const auto expected : {T::enter, T::tab, T::end, T::escape}) {
                    const auto decoded = editor_event(terminal);
                    check(decoded.type == expected && decoded.repeat_count == 1,
                          "Control/submission/completion record was unexpectedly repeated");
                }
            }
            write({key('X', L'X', false, 0, 65535), key('X', L'X', true, 0, 0),
                   key(VK_PACKET, 0xD83D, true, 0, 4), key(VK_PACKET, 0xDE42, true, 0, 4),
                   key(VK_RETURN, 0, true, 0, 65535)});
            const auto emoji = editor_event(terminal);
            check(emoji.type == T::character && emoji.text == "🙂" && emoji.repeat_count == 4,
                  "Repeated surrogate-pair Unicode or zero/key-up filtering regressed");
            check(editor_event(terminal).type == T::enter, "Repeated Unicode displaced Enter");
            write({key(VK_PACKET, 0xD83D, true, 0, 4), key(VK_PACKET, 0xDE42, true, 0, 3),
                   key('Z', L'z')});
            const auto after_malformed = editor_event(terminal);
            check(after_malformed.type == T::character && after_malformed.text == "z",
                  "Malformed surrogate counts contaminated later input");
            // Any key between surrogate halves, including an ignored or
            // control key, discards the pending high half.
            for (const auto separator : {key(VK_LEFT, 0), key(VK_F2, 0), key(VK_ESCAPE, 27)}) {
                write({key(VK_PACKET, 0xD83D), separator, key(VK_PACKET, 0xDE42), key('Z', L'z')});
                for (int decoded = 0;; ++decoded) {
                    check(decoded < 4, "Surrogate separator test did not reach its sentinel");
                    const auto event = editor_event(terminal);
                    if (event.type == T::character && event.text == "z") break;
                    check(event.text != "🙂", "A key between surrogate halves was bridged");
                }
            }

            confirmation(terminal, {key('Y', L'y', true, 0, 65535),
                                   key(VK_RETURN, 0, true, 0, 65535)}, 'y', {"", "y"});
            confirmation(terminal, {key('Y', L'y', true, 0, 4), key(VK_BACK, 8, true, 0, 4),
                                   key(VK_RETURN, 0)}, '\n', {"", "y", ""});

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

            // Esc/Ctrl+C received before discard_pending_input (the credential
            // reset boundary) must not be delivered afterwards.
            write({key(VK_ESCAPE, 27), key('Q', L'q')});
            check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "Could not generate stale Ctrl+C");
            std::this_thread::sleep_for(200ms); // The control handler runs on its own thread.
            terminal.discard_pending_input();
            write({key('Z', L'z')});
            const auto fresh = editor_event(terminal);
            check(fresh.type == T::character && fresh.text == "z",
                  "Stale Esc/Ctrl+C survived discard_pending_input");

            for (const auto event : {key(VK_ESCAPE, 27), key('C', 3, true, LEFT_CTRL_PRESSED)}) {
                std::atomic_bool cancelled = false;
                std::atomic_bool callback = false;
                {
                    arn::TerminalCancellationMonitor monitor(cancelled, [&] { callback.store(true); });
                    monitor.pause_input();
                    confirmation(terminal, {key('Y', L'y'), key(VK_RETURN, 0)}, 'y', {"", "y"});
                    INPUT_RECORD resized{};
                    resized.EventType = WINDOW_BUFFER_SIZE_EVENT;
                    resized.Event.WindowBufferSizeEvent.dwSize = {70, 20};
                    confirmation(terminal, {resized, key('Y', L'y'), key(VK_BACK, 8),
                                           key('N', L'n'), key(VK_RETURN, 0)},
                                 'n', {"", "y", "", "n"});
                    check(!cancelled.load() && !callback.load(),
                          "Paused request monitor stole a permission decision");
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
        DWORD restored_mode{};
        check(GetConsoleMode(console_input, &restored_mode) && restored_mode == original_mode,
              "Windows console input mode was not restored");
        check(GetConsoleCP() == original_cp && GetConsoleOutputCP() == original_output_cp,
              "Windows console code pages were not restored");
        check(captured.str().find("1049h") == std::string::npos, "Windows still enters alternate screen");
        resize_records_without_size_change();
        console_ui_smoke();
        if (argc > 1) {
            transcript_smoke(argv[1]);
            redirected_stdin_smoke(argv[1]);
        }
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
