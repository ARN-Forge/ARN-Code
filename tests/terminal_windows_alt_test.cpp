// Real Windows console tests for how ARN presents itself: the alternate
// screen in a console (whatever ARN_TERMINAL_RENDERER says; unsupported
// values, including the removed native, are reported), and plain output for
// redirected input or output. A console is allocated and read back through
// the console API; ARN itself runs as a child on that console.
//
//   terminal_windows_alt_test [arn.exe fake_kiro_cli.exe acp_fake_server.mjs]
//
// Run it in a visible or minimized console. Under a pseudoconsole (for
// example CTest in some environments) the window cannot be resized or read
// as a classic console; the test then reports "not exercised".

#include "terminal.hpp"
#include "terminal_posix_view.hpp"
#include "terminal_ui.hpp"
#include "terminal_windows_mode.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using arn::terminal_ui::Tone;
using arn::terminal_ui::Ui;

int checks = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

// --- Console helpers -------------------------------------------------------

HANDLE open_console(const wchar_t* name, bool inherit = false) {
    SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, inherit ? TRUE : FALSE};
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                       OPEN_EXISTING, 0, nullptr);
}

std::wstring trim(std::wstring text) {
    while (!text.empty() && text.back() == L' ') text.pop_back();
    return text;
}

std::wstring read_row(HANDLE buffer, SHORT y, SHORT width) {
    std::wstring text(static_cast<std::size_t>(width), L' ');
    DWORD read{};
    ReadConsoleOutputCharacterW(buffer, text.data(), static_cast<DWORD>(width), {0, y}, &read);
    text.resize(read);
    return trim(std::move(text));
}

CONSOLE_SCREEN_BUFFER_INFO info_of(HANDLE buffer) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(buffer, &info);
    return info;
}

// The rows of the console window, from the active screen buffer (the
// alternate one while it is in use).
std::vector<std::wstring> visible_rows() {
    const HANDLE active = open_console(L"CONOUT$");
    const auto info = info_of(active);
    std::vector<std::wstring> rows;
    for (SHORT y = info.srWindow.Top; y <= info.srWindow.Bottom; ++y) rows.push_back(read_row(active, y, info.dwSize.X));
    CloseHandle(active);
    return rows;
}

// The console API reads one row at a time while ARN may be painting the next
// frame. Two identical reads in a row are a frame, not a mix of two.
std::vector<std::wstring> stable_rows() {
    auto previous = visible_rows();
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(15ms);
        auto current = visible_rows();
        if (current == previous) return current;
        previous = std::move(current);
    }
    throw std::runtime_error("The screen never held still long enough to be read");
}

std::wstring joined(const std::vector<std::wstring>& rows) {
    std::wstring text;
    for (const auto& row : rows) text += row + L'\n';
    return text;
}

// Width of the console window, from the active screen buffer.
int window_width() {
    const HANDLE active = open_console(L"CONOUT$");
    const auto info = info_of(active);
    CloseHandle(active);
    return info.srWindow.Right - info.srWindow.Left + 1;
}

// Every row of a buffer.
std::wstring buffer_text(HANDLE buffer) {
    const auto info = info_of(buffer);
    std::vector<std::wstring> rows;
    for (SHORT y = 0; y < info.dwSize.Y; ++y) rows.push_back(read_row(buffer, y, info.dwSize.X));
    return joined(rows);
}

std::string narrow(std::wstring_view text) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, count)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

// For this test's own log lines: control characters shown as '?'.
std::string printable(std::wstring_view text) {
    std::wstring copy(text);
    for (auto& unit : copy)
        if (unit < 0x20 || (unit >= 0x7F && unit <= 0x9F)) unit = L'?';
    return narrow(copy);
}

std::wstring widen(std::string_view text) {
    std::wstring wide(text.size(), L'\0');
    wide.resize(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                                             wide.data(), static_cast<int>(wide.size()))));
    return wide;
}

// The console read-back returns each glyph once and characters outside the
// BMP as U+FFFD; it also stores combining marks and wraps lines in its own
// way. Compare text with all of that removed.
std::wstring squeeze(std::wstring_view text) {
    std::wstring result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const wchar_t ch = text[index];
        if (ch >= 0xD800 && ch <= 0xDBFF) { result.push_back(L'\xFFFD'); ++index; continue; }
        if (ch > L' ' && !(ch >= 0x0300 && ch <= 0x036F)) result.push_back(ch);
    }
    return result;
}

std::size_t occurrences(std::wstring_view text, std::wstring_view token) {
    std::size_t found = 0;
    for (auto at = text.find(token); at != std::wstring_view::npos; at = text.find(token, at + token.size())) ++found;
    return found;
}
std::size_t squeezed_occurrences(std::wstring_view text, std::wstring_view token) {
    return occurrences(squeeze(text), squeeze(token));
}

struct ConsoleState {
    DWORD input_mode{}, output_mode{};
    UINT input_cp{}, output_cp{};
    BOOL cursor_visible{};
    bool operator==(const ConsoleState&) const = default;
};

ConsoleState console_state(HANDLE input, HANDLE output) {
    ConsoleState state;
    GetConsoleMode(input, &state.input_mode);
    GetConsoleMode(output, &state.output_mode);
    state.input_cp = GetConsoleCP();
    state.output_cp = GetConsoleOutputCP();
    CONSOLE_CURSOR_INFO cursor{};
    GetConsoleCursorInfo(output, &cursor);
    state.cursor_visible = cursor.bVisible;
    return state;
}

INPUT_RECORD key(WORD vk, wchar_t ch, DWORD control = 0, WORD repeat = 1) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent = {TRUE, repeat, vk, 0, {}, control};
    record.Event.KeyEvent.uChar.UnicodeChar = ch;
    return record;
}
const INPUT_RECORD ctrl_c = key('C', 3, LEFT_CTRL_PRESSED);

// A console mouse record; for the wheel, `delta` is 120 per notch (positive:
// forward, which scrolls up).
INPUT_RECORD mouse(DWORD flags, SHORT delta = 0) {
    INPUT_RECORD record{};
    record.EventType = MOUSE_EVENT;
    record.Event.MouseEvent.dwEventFlags = flags;
    record.Event.MouseEvent.dwButtonState = static_cast<DWORD>(static_cast<WORD>(delta)) << 16;
    record.Event.MouseEvent.dwMousePosition = {10, 12};
    return record;
}
INPUT_RECORD wheel(SHORT delta) { return mouse(MOUSE_WHEELED, delta); }

// The input mode while ARN runs: the alternate screen reads the mouse wheel
// (mouse input on, Quick Edit off); plain output leaves the mouse to the host.
bool reads_wheel(HANDLE input) {
    DWORD mode{};
    return GetConsoleMode(input, &mode) && (mode & ENABLE_MOUSE_INPUT) && !(mode & ENABLE_QUICK_EDIT_MODE);
}
bool host_owns_mouse(HANDLE input) {
    DWORD mode{};
    return GetConsoleMode(input, &mode) && !(mode & ENABLE_MOUSE_INPUT) && (mode & ENABLE_QUICK_EDIT_MODE);
}

void send(HANDLE input, const std::vector<INPUT_RECORD>& records) {
    DWORD written{};
    check(WriteConsoleInputW(input, records.data(), static_cast<DWORD>(records.size()), &written) &&
              written == records.size(),
          "WriteConsoleInputW failed");
}

std::vector<INPUT_RECORD> typed(std::wstring_view text, bool enter = true) {
    std::vector<INPUT_RECORD> records;
    for (const wchar_t ch : text) records.push_back(key(0, ch));
    if (enter) records.push_back(key(VK_RETURN, L'\r'));
    return records;
}

void write_text(HANDLE output, std::string_view text) {
    DWORD written{};
    WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// Writes std::cout to a console handle, keeping a copy of the bytes.
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
    std::string take() { return std::exchange(emitted_, {}); }
};

void set_renderer(const wchar_t* value) { SetEnvironmentVariableW(L"ARN_TERMINAL_RENDERER", value); }

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return condition();
}
bool wait_screen(std::wstring_view token, std::chrono::milliseconds timeout = 10s) {
    return wait_until([&] { return joined(visible_rows()).find(token) != std::wstring::npos; }, timeout);
}
// Rows counted from the bottom of the window: 1 is the last row.
std::wstring bottom_row(int from_bottom) {
    const auto rows = visible_rows();
    return rows.size() >= static_cast<std::size_t>(from_bottom) ? rows[rows.size() - static_cast<std::size_t>(from_bottom)]
                                                                : std::wstring{};
}
bool wait_hint(std::wstring_view prefix, std::chrono::milliseconds timeout = 10s) {
    return wait_until([&] { return bottom_row(2).starts_with(prefix); }, timeout);
}
bool wait_prompt(std::wstring_view prefix, std::chrono::milliseconds timeout = 10s) {
    return wait_until([&] { return bottom_row(3).starts_with(prefix); }, timeout);
}
int prompt_rows() {
    int found = 0;
    for (const auto& row : visible_rows()) found += row.starts_with(L"arn ›");
    return found;
}
// While the alternate screen is active its buffer is exactly the window.
bool alternate_screen_active() {
    const HANDLE active = open_console(L"CONOUT$");
    const auto info = info_of(active);
    CloseHandle(active);
    return info.dwSize.Y == info.srWindow.Bottom - info.srWindow.Top + 1;
}

// A never-shown console window ignores resizes: show it once, off-screen
// and without focus, then hide it again.
void realize_window(HWND window) {
    SetWindowPos(window, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    ShowWindow(window, SW_HIDE);
}
void resize_window(int px, int py) {
    SetWindowPos(GetConsoleWindow(), nullptr, 0, 0, px, py, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// A normal screen of 100x30 with 300 rows of history. False when the host
// cannot provide one (a pseudoconsole).
bool reset_normal_screen(HANDLE output) {
    auto info = info_of(output);
    SMALL_RECT shrunk{0, 0, static_cast<SHORT>(std::min<int>(100, info.srWindow.Right - info.srWindow.Left + 1) - 1),
                      static_cast<SHORT>(std::min<int>(30, info.srWindow.Bottom - info.srWindow.Top + 1) - 1)};
    SetConsoleWindowInfo(output, TRUE, &shrunk);
    if (!SetConsoleScreenBufferSize(output, {100, 300})) return false;
    SMALL_RECT window{0, 0, 99, 29};
    if (!SetConsoleWindowInfo(output, TRUE, &window)) return false;
    info = info_of(output);
    return info.dwSize.Y >= 300 && info.srWindow.Right - info.srWindow.Left + 1 == 100;
}

void clear_screen(HANDLE output, std::string_view marker) {
    write_text(output, "\x1b[2J\x1b[3J\x1b[H");
    write_text(output, marker);
    write_text(output, "\r\n");
}

int count_processes(const wchar_t* name) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof entry};
    int found = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        found += _wcsicmp(entry.szExeFile, name) == 0;
    CloseHandle(snapshot);
    return found;
}

struct Console {
    HANDLE input{}, output{};
};

// --- In-process tests ------------------------------------------------------

void mode_selection(const Console& console) {
    ConsoleOutput sink(console.output); // Session start/stop bytes stay on the console.
    auto* original = std::cout.rdbuf(&sink);
    struct Case {
        const wchar_t* value;
        bool recognized;
        bool removed_native;
    };
    const Case cases[] = {
        {nullptr, true, false},            // Unset.
        {L"", true, false},                // Empty counts as unset.
        {L"alternate", true, false},
        {L"ALTERNATE", true, false},
        // The removed renderer is reported as such.
        {L"native", false, true},
        {L"Native", false, true},
        // Anything else is reported as unsupported.
        {L"alt", false, false},
        {L"nativ", false, false},
        {L"alternate ", false, false},
        {L" native", false, false},
        {L"1", false, false},
        {L"alternate,native", false, false},
    };
    for (const auto& [value, recognized, removed_native] : cases) {
        set_renderer(value);
        const auto setting = arn::windows_renderer_setting();
        const auto label = std::string("ARN_TERMINAL_RENDERER=") + (value ? narrow(value) : "(unset)");
        check(setting.recognized == recognized && setting.removed_native == removed_native,
              label + ": wrong classification");
        check(recognized ? setting.value.empty() : setting.value == narrow(value), label + ": wrong reported value");
        // No value selects a renderer: a console always gets the alternate screen.
        arn::TerminalSession session;
        check(!session.native_scrollback(), label + ": a console did not get the alternate screen");
    }
    set_renderer(std::wstring(100, L'x').c_str());
    check(arn::windows_renderer_setting().value == std::string(64, 'x'), "A long value must be shortened for display");

    // Redirected output or input gets plain output, whatever the variable says.
    for (const wchar_t* value : {static_cast<const wchar_t*>(nullptr), L"alternate", L"native"}) {
        for (const DWORD redirected : {STD_OUTPUT_HANDLE, STD_INPUT_HANDLE}) {
            HANDLE reader{}, writer{};
            SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, FALSE};
            check(CreatePipe(&reader, &writer, &attributes, 1 << 16), "CreatePipe failed");
            SetStdHandle(redirected, redirected == STD_OUTPUT_HANDLE ? writer : reader);
            set_renderer(value);
            {
                arn::TerminalSession session;
                check(session.native_scrollback(), redirected == STD_OUTPUT_HANDLE
                                                       ? "Redirected output must get plain output"
                                                       : "Redirected input must get plain output");
            }
            SetStdHandle(STD_OUTPUT_HANDLE, console.output);
            SetStdHandle(STD_INPUT_HANDLE, console.input);
            CloseHandle(writer);
            CloseHandle(reader);
        }
    }
    set_renderer(nullptr);
    std::cout.rdbuf(original);
    std::puts("console: renderer choice: a console always gets the alternate screen; native and unknown "
              "values are reported; redirected input or output gets plain output");
}

// Renders a conversation in the alternate screen, then leaves it by
// destruction (with the default setting), or by an exception when `fail` is
// set (with ARN_TERMINAL_RENDERER=alternate).
void lifecycle(const Console& console, bool fail) {
    check(reset_normal_screen(console.output), "Could not reset the normal screen");
    clear_screen(console.output, "before-alt-marker");
    SetConsoleCP(437);
    SetConsoleOutputCP(437);
    const auto before = console_state(console.input, console.output);
    ConsoleOutput writer(console.output);
    auto* previous = std::cout.rdbuf(&writer);
    set_renderer(fail ? L"alternate" : nullptr);
    const std::pair<Tone, std::string> entries[] = {
        {Tone::user, "arn › first question"}, {Tone::normal, "reply Привіт 你好 🙂 é"}, {Tone::muted, "• read_file"},
        {Tone::good, "good-entry!"}, {Tone::warning, "warning-entry!"}, {Tone::error, "error-entry!"}};
    try {
        arn::TerminalSession terminal;
        Ui ui(terminal);
        check(!terminal.native_scrollback(), "The alternate screen was not selected");
        // Restored with the other console modes below, on both exit paths.
        check(reads_wheel(console.input), "The alternate screen does not read the mouse wheel");
        ui.session("Gemini", "fake-model", true);
        ui.status(ui.ready_status());
        for (const auto& [tone, text] : entries) ui.add(tone, text);
        ui.render();
        check(alternate_screen_active(), "The renderer did not switch to the alternate screen buffer");
        const auto rows = visible_rows();
        const auto height = rows.size();
        check(rows[0].find(L'╭') != std::wstring::npos && rows[8].find(L'╰') != std::wstring::npos,
              "The header box is missing");
        check(joined(rows).find(L"ARN AGENT CODE") != std::wstring::npos, "The header title is missing");
        check(rows[height - 4].starts_with(L"─") && rows[height - 3] == L"arn ›" &&
                  rows[height - 2] == L"Type /help for commands" && rows[height - 1].empty(),
              "The footer rows are misplaced");
        check(rows[10] == L" arn › first question" && rows[12] == L" • read_file" && rows[15] == L" error-entry!",
              "Transcript rows are misplaced");
        check(squeeze(rows[11]) == squeeze(widen("replyПривіт你好🙂é")), "Unicode text was not drawn intact");
        check(joined(rows).find(L"before-alt-marker") == std::wstring::npos, "The normal screen shows through");
        const auto frame = writer.take();
        for (const auto code : {arn::terminal_ui::cyan, arn::terminal_ui::dim, arn::terminal_ui::green,
                                arn::terminal_ui::amber, arn::terminal_ui::red})
            check(frame.find(std::string(code)) != std::string::npos, "A display style was not used");
        if (fail) throw std::runtime_error("simulated failure");
    } catch (const std::runtime_error& error) {
        check(fail && std::string_view(error.what()) == "simulated failure", "Unexpected exception");
    }
    std::cout.rdbuf(previous);
    set_renderer(nullptr);
    const auto label = std::string(fail ? "exception" : "normal exit");
    check(!alternate_screen_active(), label + ": the alternate screen was not left");
    check(console_state(console.input, console.output) == before,
          label + ": console modes, code pages or cursor visibility were not restored");
    const auto screen = buffer_text(console.output);
    check(occurrences(screen, L"before-alt-marker") == 1, label + ": the normal screen was not restored");
    for (const auto& [tone, text] : entries)
        check(squeezed_occurrences(screen, widen(text)) == 1, label + ": an entry was not printed exactly once");
    for (const auto frame_text : {L"ARN AGENT CODE", L"Type /help for commands", L"───"})
        check(occurrences(screen, frame_text) == 0, label + ": a frame reached the normal screen");
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::printf("console: %s: alternate screen left, conversation printed once, console state restored\n",
                label.c_str());
}

void resize_while_scrolled(const Console& console) {
    check(reset_normal_screen(console.output), "Could not reset the normal screen");
    ConsoleOutput writer(console.output);
    auto* previous = std::cout.rdbuf(&writer);
    set_renderer(nullptr); // The default.
    std::vector<COORD> seen;
    {
        arn::TerminalSession terminal;
        Ui ui(terminal);
        ui.status(ui.ready_status());
        for (int line = 0; line < 150; ++line) ui.add(Tone::normal, "row-" + std::to_string(line) + "!");
        ui.render();
        ui.scroll(60);
        ui.render();
        const auto top = visible_rows()[10];
        check(top == L" row-74!", "Scrolling up did not move to the expected row");
        for (const auto& [px, py] : {std::pair{700, 450}, {1200, 820}, {560, 400}, {1000, 650}, {850, 560}}) {
            resize_window(px, py);
            ui.render();
            const auto rows = visible_rows();
            const COORD size{static_cast<SHORT>(window_width()), static_cast<SHORT>(rows.size())};
            if (seen.empty() || seen.back().X != size.X || seen.back().Y != size.Y) seen.push_back(size);
            const int header = size.X - 4 < 58 ? 5 : size.X - 4 < 76 ? 7 : 9;
            check(prompt_rows() == 1, "More than one prompt after a resize");
            check(occurrences(joined(rows), L"ARN AGENT CODE") == 1, "The header was not shown once after a resize");
            check(rows[rows.size() - 2].starts_with(L"Scrolled up · Wheel down, PgDn or End"),
                  "The scrolled hint was lost");
            check(rows[static_cast<std::size_t>(header) + 1] == top, "A resize lost the reading position");
        }
    }
    std::cout.rdbuf(previous);
    set_renderer(nullptr);
    check(seen.size() >= 3, "Window resizes did not change the alternate screen size");
    std::printf("console: %zu window sizes while scrolled: one prompt, one header, same reading position\n",
                seen.size());
}

// During a request the cancellation monitor owns input. With the alternate
// screen it keeps PgUp/PgDn/End and the mouse wheel for the renderer; with
// plain output (redirected standard output here) it drops them.
void monitor_view_keys(const Console& console) {
    using Type = arn::TerminalEventType;
    for (const bool alternate : {true, false}) {
        set_renderer(nullptr);
        HANDLE reader{}, pipe{};
        SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, FALSE};
        check(CreatePipe(&reader, &pipe, &attributes, 1 << 16), "CreatePipe failed");
        if (!alternate) SetStdHandle(STD_OUTPUT_HANDLE, pipe);
        ConsoleOutput writer(console.output);
        auto* previous = std::cout.rdbuf(&writer);
        {
            arn::TerminalSession terminal;
            check(alternate ? reads_wheel(console.input) : host_owns_mouse(console.input),
                  alternate ? "The alternate screen does not read the mouse wheel"
                            : "Plain output took the mouse from the host");
            std::atomic_bool cancelled{false};
            std::atomic_int requests{0};
            arn::TerminalCancellationMonitor monitor(cancelled, [&] { ++requests; });
            send(console.input, {key(VK_PRIOR, 0, 0, 2), key('X', L'x'), key(VK_NEXT, 0), key(VK_END, 0),
                                 wheel(120), mouse(MOUSE_MOVED), mouse(MOUSE_HWHEELED, 120), wheel(40),
                                 wheel(-240)});
            std::vector<arn::TerminalEvent> events;
            wait_until([&] {
                for (auto& event : arn::take_deferred_view_events()) events.push_back(std::move(event));
                return events.size() >= 5;
            }, alternate ? 3s : 500ms);
            if (alternate) {
                check(events.size() == 5 && events[0].type == Type::page_up && events[0].repeat_count == 2 &&
                          events[1].type == Type::page_down && events[2].type == Type::end &&
                          events[3].type == Type::wheel_up && events[3].repeat_count == 1 &&
                          events[4].type == Type::wheel_down && events[4].repeat_count == 2,
                      "The monitor did not keep the view keys and the wheel in order");
            } else {
                check(terminal.native_scrollback() && events.empty(), "Plain output must not keep view keys");
            }
            check(!cancelled.load() && requests.load() == 0,
                  "A view key, the mouse or typed text cancelled the request");
            send(console.input, {key(VK_ESCAPE, 27)});
            check(wait_until([&] { return cancelled.load(); }, 3s) && requests.load() == 1, "Esc did not cancel");
        }
        std::cout.rdbuf(previous);
        SetStdHandle(STD_OUTPUT_HANDLE, console.output);
        CloseHandle(pipe);
        CloseHandle(reader);
    }
    std::puts("console: request monitor: view keys and the wheel kept for the alternate screen only; Esc cancels");
}

// --- ARN itself on this console ----------------------------------------------

struct Run {
    DWORD exit_code{};
    std::wstring screen; // The normal buffer after ARN exited.
};

Run run_arn(const Console& console, const std::wstring& arn, const wchar_t* renderer, const std::function<void()>& act) {
    check(reset_normal_screen(console.output), "Could not reset the normal screen");
    clear_screen(console.output, "before-arn-marker");
    FlushConsoleInputBuffer(console.input);
    SetConsoleCP(437);
    SetConsoleOutputCP(437);
    const auto before = console_state(console.input, console.output);
    set_renderer(renderer);
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = console.input;
    startup.hStdOutput = startup.hStdError = console.output;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + arn + L"\"";
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) !=
              FALSE,
          "Could not start ARN");
    set_renderer(nullptr);
    std::string failure, failure_screen;
    try {
        act();
    } catch (const std::exception& error) {
        failure = error.what();
        failure_screen = narrow(joined(visible_rows()));
        send(console.input, typed(L"/exit")); // Let ARN end itself if it can.
    }
    const bool exited = WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0;
    if (!exited) TerminateProcess(process.hProcess, 99);
    Run run;
    GetExitCodeProcess(process.hProcess, &run.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!failure.empty()) throw std::runtime_error(failure + "\nScreen at the failure:\n" + failure_screen);
    check(exited, "ARN did not exit");
    check(!alternate_screen_active(), "ARN left the console in the alternate screen");
    check(console_state(console.input, console.output) == before,
          "ARN did not restore console modes, code pages or cursor visibility");
    run.screen = buffer_text(console.output);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    return run;
}

// While a request runs ARN's cancellation monitor owns input and drops keys
// other than Esc/Ctrl+C (and keeps view keys), so typing waits for the prompt.
void type_when_idle(const Console& console, std::wstring_view text) {
    check(wait_until([] {
              const auto hint = bottom_row(2);
              return hint.starts_with(L"Type /help for commands") || hint.starts_with(L"Input reset complete.");
          }, 20s),
          "ARN did not return to the prompt");
    send(console.input, typed(text));
}

void arn_alternate_session(const Console& console, const std::wstring& arn) {
    const auto unicode = widen("Unicode: Привіт ї є ґ, 你好世界, 🙂👍🎉, e\xcc\x81 end.");
    std::wstring long_message = L"__FAKE_ACP_CMD__ plain long-message-start";
    for (int part = 0; part < 12; ++part)
        long_message += L" слово-" + std::to_wstring(part) + L" 中文 word" + std::to_wstring(part);
    long_message += L" long-message-end";
    std::vector<std::wstring> pinned;
    // ARN_TERMINAL_RENDERER unset: the alternate screen is the default.
    const auto run = run_arn(console, arn, nullptr, [&] {
        check(wait_hint(L"Type /help for commands"), "ARN did not start");
        check(alternate_screen_active(), "ARN did not use the alternate screen by default");
        check(joined(visible_rows()).find(L"ARN AGENT CODE") != std::wstring::npos, "The header is missing");
        check(joined(visible_rows()).find(L"before-arn-marker") == std::wstring::npos, "The shell screen shows through");
        // run_arn checks afterwards that ARN restored the original mode.
        check(reads_wheel(console.input), "ARN does not read the mouse wheel on the alternate screen");

        type_when_idle(console, L"/acp kiro");
        check(wait_screen(L"ACP agent ready: Kiro via kiro-cli", 20s), "The ACP backend did not start");

        type_when_idle(console, L"__FAKE_ACP_CMD__ stream-and-finish");
        check(wait_screen(L"hello world") && wait_screen(L"• read_file"), "Streamed text or tool notice missing");

        type_when_idle(console, L"__FAKE_ACP_CMD__ stream-unicode");
        check(wait_screen(L" end."), "The Unicode reply did not finish");
        check(squeezed_occurrences(joined(visible_rows()), unicode) == 1, "Unicode text was not drawn intact");

        // A long message wraps inside the window and stays whole. Rows break
        // at character cells, not words, so markers can be split across rows.
        type_when_idle(console, long_message);
        check(wait_until([&] { return squeezed_occurrences(joined(visible_rows()), long_message) == 1; }, 15s) &&
                  wait_hint(L"Type /help for commands", 15s),
              "The long message was not shown whole");
        {
            const auto rows = stable_rows();
            std::size_t first = rows.size(), last = 0;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (rows[index].find(L"long-message-start") != std::wstring::npos) first = index;
                if (rows[index].find(L"word11") != std::wstring::npos) last = index;
            }
            check(first < rows.size() && last >= first + 2, "A long message must wrap onto several rows");
            check(squeezed_occurrences(joined(rows), long_message) == 1, "Wrapping lost or repeated text");
        }

        type_when_idle(console, L"__FAKE_ACP_CMD__ permission-then-finish");
        check(wait_hint(L"ACP tool: delete important file [y/N]"), "The confirmation question is not in the status row");
        check(bottom_row(3) == L"arn ›", "The confirmation prompt is not empty");
        send(console.input, {key('Y', L'y')});
        check(wait_prompt(L"arn › y"), "The choice is not shown in the prompt row");
        send(console.input, {key(VK_RETURN, L'\r')});
        check(wait_screen(L"choice=allow"), "Allowing did not reach the agent");
        check(wait_screen(L"ACP tool: delete important file [y/N] y"), "The answer was not recorded");

        type_when_idle(console, L"__FAKE_ACP_CMD__ permission-then-finish");
        check(wait_hint(L"ACP tool: delete important file [y/N]"), "The second question is missing");
        send(console.input, {key('N', L'n'), key(VK_RETURN, L'\r')});
        check(wait_screen(L"choice=reject"), "Declining did not reach the agent");

        type_when_idle(console, L"__FAKE_ACP_CMD__ wait-for-cancel");
        check(wait_hint(L"Arnie is working") && wait_screen(L"waiting"), "The request did not start");
        send(console.input, {key(VK_ESCAPE, 27)});
        check(wait_screen(L"Request cancelled."), "Esc did not cancel");
        type_when_idle(console, L"__FAKE_ACP_CMD__ wait-for-cancel");
        check(wait_hint(L"Arnie is working"), "The second request did not start");
        std::this_thread::sleep_for(300ms);
        send(console.input, {ctrl_c});
        check(wait_until([] { return occurrences(joined(visible_rows()), L"Request cancelled.") == 2; }),
              "Ctrl+C did not cancel");

        // A real console Ctrl+C (a control event, as the keyboard produces,
        // not a key record) cancels a request, and at the prompt clears the
        // draft without ending ARN.
        type_when_idle(console, L"__FAKE_ACP_CMD__ wait-for-cancel");
        check(wait_hint(L"Arnie is working"), "The third request did not start");
        std::this_thread::sleep_for(300ms);
        check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != FALSE, "GenerateConsoleCtrlEvent failed");
        check(wait_until([] { return occurrences(joined(visible_rows()), L"Request cancelled.") == 3; }),
              "A console Ctrl+C did not cancel the request");
        check(wait_hint(L"Type /help for commands"), "ARN did not return to the prompt after Ctrl+C");
        send(console.input, typed(L"draft-to-clear", false));
        check(wait_prompt(L"arn › draft-to-clear"), "The draft is not shown");
        check(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != FALSE, "GenerateConsoleCtrlEvent failed");
        check(wait_until([] { return bottom_row(3) == L"arn ›"; }, 3s), "A console Ctrl+C did not clear the draft");
        check(alternate_screen_active(), "A console Ctrl+C at the prompt ended ARN");

        // Scrolling while output arrives: the rows being read stay put.
        type_when_idle(console, L"__FAKE_ACP_CMD__ stream-lines");
        check(wait_screen(L"line 12 "), "Streaming did not start");
        send(console.input, {key(VK_PRIOR, 0)});
        check(wait_hint(L"Scrolled up · Wheel down, PgDn or End"), "PgUp during streaming did not scroll");
        const auto transcript = [] {
            auto rows = stable_rows();
            return std::vector<std::wstring>(rows.begin() + 10, rows.end() - 4);
        };
        pinned = transcript();
        check(std::ranges::count_if(pinned, [](const auto& row) { return row.starts_with(L" line "); }) >= 3,
              "The pinned view does not show streamed lines");
        std::this_thread::sleep_for(700ms);
        check(transcript() == pinned, "New output moved the rows being read");
        check(joined(visible_rows()).find(L"line 60 ") == std::wstring::npos, "The view did not stay scrolled");
        send(console.input, {key(VK_END, 0)});
        check(wait_screen(L"line 60 ", 15s), "End did not return to the newest output");
        check(wait_hint(L"Type /help for commands", 15s), "The streamed reply did not finish");
        send(console.input, {key(VK_PRIOR, 0)});
        check(wait_hint(L"Scrolled up"), "PgUp at the prompt did not scroll");
        send(console.input, {key(VK_NEXT, 0, 0, 20)});
        check(wait_hint(L"Type /help for commands"), "PgDn did not return to the newest output");

        // The mouse wheel does the same: wheel up while output arrives pins the
        // rows being read, a move or the horizontal wheel changes nothing, End
        // follows again; at the prompt wheel down returns to the newest output.
        type_when_idle(console, L"__FAKE_ACP_CMD__ stream-lines");
        check(wait_screen(L"line 12 "), "The wheel stream did not start");
        send(console.input, {wheel(120)});
        check(wait_hint(L"Scrolled up · Wheel down"), "The wheel during streaming did not scroll");
        pinned = transcript();
        std::this_thread::sleep_for(700ms);
        check(transcript() == pinned, "New output moved the rows read after a wheel scroll");
        send(console.input, {mouse(MOUSE_MOVED), mouse(MOUSE_HWHEELED, -120), mouse(MOUSE_MOVED)});
        std::this_thread::sleep_for(300ms);
        check(bottom_row(2).starts_with(L"Scrolled up") && transcript() == pinned,
              "A mouse move or the horizontal wheel moved the view");
        send(console.input, {key(VK_END, 0)});
        check(wait_screen(L"line 60 ", 15s), "End did not return to the newest output after a wheel scroll");
        check(wait_hint(L"Type /help for commands", 15s), "The wheel-scrolled reply did not finish");
        send(console.input, {wheel(240)});
        check(wait_hint(L"Scrolled up"), "The wheel at the prompt did not scroll");
        send(console.input, {wheel(-120 * 40)});
        check(wait_hint(L"Type /help for commands"), "Wheel down did not return to the newest output");

        // Window resizes while output arrives.
        type_when_idle(console, L"__FAKE_ACP_CMD__ stream-lines");
        check(wait_screen(L"line 3 "), "The second stream did not start");
        int resizes = 0;
        for (const auto& [px, py] : {std::pair{700, 450}, {1200, 820}, {560, 400}, {1000, 650}, {850, 560},
                                    {640, 420}, {1100, 700}, {900, 600}}) {
            resize_window(px, py);
            ++resizes;
            std::this_thread::sleep_for(150ms);
        }
        check(wait_hint(L"Type /help for commands", 20s), "The resized stream did not finish");
        check(prompt_rows() == 1, "More than one prompt after resizing while streaming");
        check(occurrences(joined(visible_rows()), L"ARN AGENT CODE") == 1, "The header is not shown once");
        resize_window(860, 560); // At the prompt.
        check(wait_until([] { return prompt_rows() == 1 && bottom_row(2).starts_with(L"Type /help"); }, 3s),
              "Resizing at the prompt broke the layout");

        // Secure API key input.
        type_when_idle(console, L"/key-gemini");
        check(wait_hint(L"Enter submits · Esc or Ctrl+C cancels"), "The secure key prompt is missing");
        send(console.input, typed(L"FAKE_SECRET_123", false));
        check(wait_prompt(L"arn › ***************"), "The key was not masked");
        check(joined(visible_rows()).find(L"FAKE_SECRET") == std::wstring::npos, "The key was drawn");
        send(console.input, {key(VK_ESCAPE, 27)});
        check(wait_screen(L"API key entry cancelled") && wait_hint(L"Input reset required"), "Cancelling the key failed");
        send(console.input, {key(VK_ESCAPE, 27)});
        check(wait_hint(L"Input reset complete."), "The input reset did not finish");
        send(console.input, typed(L"/key-gemini FAKE_INLINE_456", false));
        std::this_thread::sleep_for(300ms);
        check(joined(visible_rows()).find(L"FAKE_INLINE") == std::wstring::npos, "An inline key was drawn");
        send(console.input, {key(VK_RETURN, L'\r')});
        check(wait_screen(L"Inline API key entry is disabled") && wait_hint(L"Input reset required"),
              "The inline key was not rejected");
        send(console.input, {key(VK_ESCAPE, 27)});
        check(wait_hint(L"Input reset complete."), "The second input reset did not finish");

        type_when_idle(console, L"/exit");
    });
    check(run.exit_code == 0, "ARN did not exit with code 0");
    const auto& screen = run.screen;
    check(occurrences(screen, L"before-arn-marker") == 1, "The shell screen was not restored");
    for (const auto frame_text : {L"ARN AGENT CODE", L"Type /help for commands", L"Scrolled up", L"Arnie is working",
                                  L"───", L"Enter submits"})
        check(occurrences(screen, frame_text) == 0, "A frame reached the normal screen");
    for (const auto& [token, expected] :
         {std::pair<std::wstring_view, std::size_t>{L"arn › __FAKE_ACP_CMD__ stream-and-finish", 1},
          {L"hello world", 1}, {L"choice=allow", 1}, {L"choice=reject", 1}, {L"Request cancelled.", 3},
          {L"ACP tool: delete important file [y/N] y", 1}, {L"line 37 Привіт 你好", 3}, {L"arn › /exit", 1}})
        check(squeezed_occurrences(screen, token) == expected, "The printed conversation is wrong: " +
              std::to_string(squeezed_occurrences(screen, token)) + " copies of a token");
    check(squeezed_occurrences(screen, unicode) == 1, "The printed Unicode reply is not intact");
    check(squeezed_occurrences(screen, long_message) == 1, "The printed long message is not whole");
    check(occurrences(screen, L"draft-to-clear") == 0, "A draft cleared by Ctrl+C reached the printed conversation");
    check(occurrences(screen, L"FAKE_SECRET") == 0 && occurrences(screen, L"FAKE_INLINE") == 0,
          "A key reached the printed conversation");
    check(wait_until([] { return count_processes(L"fake_kiro_cli.exe") == 0; }, 5s), "The ACP backend outlived ARN");
    std::puts("console: ARN default (no ARN_TERMINAL_RENDERER) uses the alternate screen: streaming, Unicode, a long "
              "wrapped message, tools, y/N, Esc, Ctrl+C keys and console Ctrl+C events (request and prompt), "
              "PgUp/PgDn/End and the mouse wheel while streaming, 8 resizes while streaming, secure key input, "
              "/exit printed once, "
              "console restored");
}

// ARN_TERMINAL_RENDERER=alternate, chosen explicitly.
void arn_explicit_alternate(const Console& console, const std::wstring& arn) {
    const auto run = run_arn(console, arn, L"alternate", [&] {
        check(wait_hint(L"Type /help for commands"), "ARN did not start");
        check(alternate_screen_active(), "ARN_TERMINAL_RENDERER=alternate did not use the alternate screen");
        type_when_idle(console, L"/status");
        check(wait_screen(L"Context: empty"), "/status did not answer");
        type_when_idle(console, L"/exit");
    });
    check(run.exit_code == 0, "ARN did not exit with code 0");
    check(occurrences(run.screen, L"ARN AGENT CODE") == 0 && squeezed_occurrences(run.screen, L"arn › /status") == 1,
          "The alternate screen did not print the conversation once without frames");
    std::puts("console: ARN_TERMINAL_RENDERER=alternate uses the alternate screen");
}

// ARN_TERMINAL_RENDERER=native (the removed renderer) or any other value ARN
// does not support: a warning, and the alternate screen in a console.
void arn_unsupported_value(const Console& console, const std::wstring& arn, const wchar_t* value) {
    const bool removed_native = _wcsicmp(value, L"native") == 0;
    const std::wstring warning = L"ARN_TERMINAL_RENDERER=\"" + std::wstring(value) +
        (removed_native ? L"\" is no longer supported: the native renderer was removed."
                        : L"\" is not supported and has no effect.");
    const auto run = run_arn(console, arn, value, [&] {
        check(wait_hint(L"Type /help for commands"), "ARN did not start");
        check(alternate_screen_active(), "An unsupported value kept ARN off the alternate screen");
        check(wait_until([&] { return squeezed_occurrences(joined(visible_rows()), warning) == 1; }),
              "The warning was not shown");
        type_when_idle(console, L"/status");
        check(wait_screen(L"Context: empty"), "/status did not answer");
        type_when_idle(console, L"/exit");
    });
    check(run.exit_code == 0, "ARN did not exit with code 0");
    check(squeezed_occurrences(run.screen, warning) == 1 && squeezed_occurrences(run.screen, L"arn › /status") == 1,
          "The conversation with the warning was not printed once");
    check(occurrences(run.screen, L"ARN AGENT CODE") == 0, "A frame reached the normal screen");
    std::printf("console: ARN_TERMINAL_RENDERER=%s is reported; the alternate screen is used\n", narrow(value).c_str());
}

// Standard input is a pipe, standard output the console: plain output, which
// ends at end of input.
void arn_redirected_input(const Console& console, const std::wstring& arn) {
    check(reset_normal_screen(console.output), "Could not reset the normal screen");
    clear_screen(console.output, "before-arn-marker");
    const auto before = console_state(console.input, console.output);
    HANDLE reader{}, writer{};
    SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, TRUE};
    check(CreatePipe(&reader, &writer, &attributes, 0), "CreatePipe failed");
    SetHandleInformation(writer, HANDLE_FLAG_INHERIT, 0);
    CloseHandle(writer); // Nothing to read: ARN sees end of input.
    set_renderer(nullptr);
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = reader;
    startup.hStdOutput = startup.hStdError = console.output;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + arn + L"\"";
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) !=
              FALSE,
          "Could not start ARN with redirected input");
    CloseHandle(reader);
    const bool exited = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
    if (!exited) TerminateProcess(process.hProcess, 99);
    DWORD code{};
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    check(exited && code == 0, "ARN with redirected input did not end at end of input");
    check(!alternate_screen_active(), "Redirected input left the alternate screen active");
    check(console_state(console.input, console.output) == before, "Redirected input changed the console state");
    // Plain output prints the banner as lines below what was there (the
    // alternate screen would have printed only the conversation), then the
    // prompt line, and never a live input area or footer rule.
    const auto screen = buffer_text(console.output);
    std::vector<std::wstring> rows;
    for (std::size_t start = 0, end; (end = screen.find(L'\n', start)) != std::wstring::npos; start = end + 1)
        rows.push_back(screen.substr(start, end - start));
    check(rows.size() > 12 && rows[0] == L"before-arn-marker" && rows[1].starts_with(L" ╭") &&
              rows[2].find(L"ARN AGENT CODE") != std::wstring::npos && rows[9].starts_with(L" ╰"),
          "Redirected input did not print the banner as plain lines");
    check(occurrences(screen, L"ARN AGENT CODE") == 1 && occurrences(screen, L"Type /help for commands") == 1,
          "Redirected input printed the banner or the prompt more than once");
    for (const auto& row : rows)
        check(!row.starts_with(L"arn ›") && row.find(std::wstring(98, L'─')) == std::wstring::npos,
              "Redirected input drew a live input area or footer rule");
    std::puts("console: ARN with redirected input (console output) prints plain output and ends cleanly");
}

// `shown`, when given, is text the output must contain.
void arn_redirected_output(const Console& console, const std::wstring& arn, const wchar_t* value,
                           std::string_view shown = {}) {
    HANDLE reader{}, writer{};
    SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, TRUE};
    check(CreatePipe(&reader, &writer, &attributes, 0), "CreatePipe failed");
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    set_renderer(value);
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = console.input;
    startup.hStdOutput = startup.hStdError = writer;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + arn + L"\"";
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) !=
              FALSE,
          "Could not start ARN with redirected output");
    set_renderer(nullptr);
    CloseHandle(writer);
    std::string output;
    const auto read_until = [&](std::string_view token) {
        return wait_until([&] {
            DWORD available{};
            while (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) {
                char buffer[4096];
                DWORD count{};
                if (!ReadFile(reader, buffer, sizeof buffer, &count, nullptr) || count == 0) break;
                output.append(buffer, count);
            }
            return output.find(token) != std::string::npos;
        });
    };
    const bool started = read_until("Type /help for commands");
    const bool host_mouse = host_owns_mouse(console.input); // While ARN runs with plain output.
    send(console.input, typed(L"/exit"));
    const bool exited = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
    if (!exited) TerminateProcess(process.hProcess, 99);
    DWORD code{};
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    for (char buffer[4096]; ;) { // Drain what is left; the pipe breaks once ARN has exited.
        DWORD count{};
        if (!ReadFile(reader, buffer, sizeof buffer, &count, nullptr) || count == 0) break;
        output.append(buffer, count);
    }
    CloseHandle(reader);
    check(started && exited && code == 0, "ARN with redirected output did not run and exit");
    check(host_mouse, "ARN with redirected output took the mouse from the host");
    check(output.find("1049h") == std::string::npos, "Redirected output must not use the alternate screen");
    // Colour only, apart from the session's mouse-mode and cursor resets.
    for (auto at = output.find('\x1b'); at != std::string::npos; at = output.find('\x1b', at + 1)) {
        auto end = at + 2;
        while (end < output.size() && !(output[end] >= '@' && output[end] <= '~')) ++end;
        const auto sequence = output.substr(at, end + 1 - at);
        check(output.compare(at, 2, "\x1b[") == 0 && end < output.size() &&
                  (output[end] == 'm' || sequence == "\x1b[?1000l" || sequence == "\x1b[?1006l" ||
                   sequence == "\x1b[?25h"),
              "Redirected output contained a non-colour sequence: " + sequence.substr(1));
    }
    check(output.find("ARN AGENT CODE") != std::string::npos && output.find("Type /help for commands") != std::string::npos,
          "Redirected output lost the banner or the prompt line");
    if (value && std::wstring_view(value) == L"native")
        check(output.find("is no longer supported: the native renderer was removed") != std::string::npos,
              "Plain output did not report the removed renderer");
    if (!shown.empty())
        check(output.find(shown) != std::string::npos, "Redirected output did not show: " + std::string(shown));
    std::printf("console: ARN with redirected output prints plain output (ARN_TERMINAL_RENDERER=%s)\n",
                value ? printable(value).c_str() : "(unset)");
}

} // namespace

// The test sends real Ctrl+C events to everything on its console; it must
// survive them itself. (A handler, unlike ignoring Ctrl+C, is not inherited
// by ARN.)
BOOL WINAPI survive_ctrl_c(DWORD type) { return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT; }

int main(int argc, char** argv) {
    FreeConsole();
    check(AllocConsole() != FALSE, "AllocConsole failed");
    SetConsoleCtrlHandler(survive_ctrl_c, TRUE);
    const HWND window = GetConsoleWindow();
    if (window) {
        ShowWindow(window, SW_HIDE);
        realize_window(window);
    }
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    Console console{open_console(L"CONIN$", true), open_console(L"CONOUT$", true)};
    check(console.input != INVALID_HANDLE_VALUE && console.output != INVALID_HANDLE_VALUE, "Could not open the console");
    SetStdHandle(STD_INPUT_HANDLE, console.input);
    SetStdHandle(STD_OUTPUT_HANDLE, console.output);
    DWORD mode{};
    GetConsoleMode(console.output, &mode);
    SetConsoleMode(console.output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    set_renderer(nullptr);
    int result = 0;
    try {
        if (!window || !reset_normal_screen(console.output)) {
            std::puts("console: the host is a pseudoconsole without a resizable classic console; not exercised");
        } else {
            mode_selection(console);
            lifecycle(console, false);
            lifecycle(console, true);
            resize_while_scrolled(console);
            monitor_view_keys(console);
            if (argc > 3) {
                std::wstring arn = widen(argv[1]);
                SetEnvironmentVariableW(L"ARN_KIRO_BIN", widen(argv[2]).c_str());
                SetEnvironmentVariableW(L"ARN_FAKE_ACP_SERVER", widen(argv[3]).c_str());
                arn_unsupported_value(console, arn, L"native");
                arn_unsupported_value(console, arn, L"alt");
                arn_explicit_alternate(console, arn);
                arn_alternate_session(console, arn);
                arn_redirected_output(console, arn, nullptr);
                arn_redirected_output(console, arn, L"alternate");
                arn_redirected_output(console, arn, L"native");
                // Regression: the warning quotes the variable's value, and
                // plain output wrote it unchanged, so a window-title OSC, a
                // clear-screen CSI, BEL, CR and a C1 CSI reached the pipe.
                arn_redirected_output(console, arn, L"x\x1b]0;ARN_TITLE\x07y\x1b[2Jz\r\x9b!",
                                      "ARN_TERMINAL_RENDERER=\"x]0;ARN_TITLEy[2Jz!\" is not supported");
                arn_redirected_input(console, arn);
            } else {
                std::puts("console: no ARN path given; ARN process tests not exercised");
            }
            std::printf("terminal_windows_alt_test: %d checks passed\n", checks);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "terminal_windows_alt_test FAILED after %d checks: %s\n", checks, error.what());
        result = 1;
    }
    FreeConsole();
    return result;
}
