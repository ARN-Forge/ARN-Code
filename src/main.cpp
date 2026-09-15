#include "api_client.hpp"
#include "tool_executor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view reset = "\x1b[0m";
constexpr std::string_view cyan = "\x1b[38;5;81m";
constexpr std::string_view amber = "\x1b[38;5;215m";
constexpr std::string_view crab_orange = "\x1b[38;5;208m";
constexpr std::string_view green = "\x1b[38;5;114m";
constexpr std::string_view dim = "\x1b[90m";
constexpr std::string_view red = "\x1b[38;5;203m";

void enable_ansi_colors() {
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

void clear_console_viewport() {
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(handle, &info)) {
        return;
    }

    // Do not use ANSI clear/home here. Windows Terminal can deliver resize
    // events while it is reflowing scrollback, which may leave the old card
    // visible. Clearing through the Console API is deterministic.
    DWORD written = 0;
    const DWORD cell_count = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
    FillConsoleOutputCharacterW(handle, L' ', cell_count, {0, 0}, &written);
    FillConsoleOutputAttribute(handle, info.wAttributes, cell_count, {0, 0}, &written);
    SetConsoleCursorPosition(handle, {0, 0});
}

std::size_t console_width() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info)) {
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    }
    return 100;
}

void write_console_at(HANDLE handle, COORD position, std::wstring_view text, WORD color) {
    SetConsoleCursorPosition(handle, position);
    SetConsoleTextAttribute(handle, color);
    DWORD written = 0;
    WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void write_orange_console_at(HANDLE handle, COORD position, std::wstring_view text) {
    SetConsoleCursorPosition(handle, position);
    // The 16-colour Windows Console palette has no real orange. Use an ANSI
    // 256-colour escape only for Arny, with the normal console palette as the
    // fallback for older terminals.
    std::cout << crab_orange << std::flush;
    DWORD written = 0;
    WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    std::cout << reset << std::flush;
}

void write_panel_text(HANDLE handle, SHORT left, SHORT right, SHORT row, SHORT column,
                      std::wstring_view text, WORD color) {
    const SHORT x = static_cast<SHORT>(left + column);
    if (x >= right) return;
    const auto capacity = static_cast<std::size_t>(right - x);
    write_console_at(handle, {x, row}, text.substr(0, capacity), color);
}

void print_welcome() {
    std::cout.flush();
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(handle, &info)) {
        std::cout << "ARN AGENT CODE — type /help for commands\n";
        return;
    }

    // Measure the visible viewport, not the underlying scrollback buffer.
    // Leaving two columns on each side prevents Windows Terminal from wrapping
    // the final border cell.
    const SHORT left = static_cast<SHORT>(info.srWindow.Left + 2);
    const SHORT top = static_cast<SHORT>(info.dwCursorPosition.Y + 1);
    const SHORT right = static_cast<SHORT>(info.srWindow.Right - 2);
    const SHORT width = static_cast<SHORT>(right - left + 1);
    if (width < 34) {
        std::cout << "ARN AGENT CODE — type /help for commands\n";
        return;
    }
    const bool compact = width < 78;
    const SHORT height = compact ? 8 : 10;
    const WORD cyan_color = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    const WORD amber_color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const WORD dim_color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    const WORD normal_color = info.wAttributes;
    const std::wstring horizontal(static_cast<std::size_t>(width - 2), L'─');

    // Clear the full card first. This prevents stale terminal glyphs from
    // remaining inside the border after a resize or a redraw.
    for (SHORT row = 0; row < height; ++row) {
        DWORD written = 0;
        FillConsoleOutputCharacterW(handle, L' ', width, {left, static_cast<SHORT>(top + row)}, &written);
    }

    write_console_at(handle, {left, top}, L"╭" + horizontal + L"╮", cyan_color);
    write_console_at(handle, {left, static_cast<SHORT>(top + height - 1)}, L"╰" + horizontal + L"╯", cyan_color);
    for (SHORT row = 1; row < height - 1; ++row) {
        write_console_at(handle, {left, static_cast<SHORT>(top + row)}, L"│", cyan_color);
        write_console_at(handle, {right, static_cast<SHORT>(top + row)}, L"│", cyan_color);
    }

    write_panel_text(handle, left, right, static_cast<SHORT>(top + 1), 2, L"ARN AGENT CODE", cyan_color);
    if (compact) {
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 2), 2, L"Arny · native CLI coding companion", amber_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 3), 2, L"Type / for hints · /help for all commands", dim_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 4), 2, L"Tab completes commands · keys stay in memory", dim_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 6), 2, L"Connect: /key-gemini <key> or /key-deepseek <key>", dim_color);
    } else {
        // Arny is a compact orange crab. Adjacent block elements form one
        // continuous pixel shape, without the gaps caused by emoji squares.
        write_orange_console_at(handle, {static_cast<SHORT>(left + 3), static_cast<SHORT>(top + 3)},
                                L"▄▄          ▄▄");
        write_orange_console_at(handle, {static_cast<SHORT>(left + 3), static_cast<SHORT>(top + 4)},
                                L"▄▀██▄▀██▀▄██▀▄");
        write_orange_console_at(handle, {static_cast<SHORT>(left + 3), static_cast<SHORT>(top + 5)},
                                L"   ▀██████▀");
        write_orange_console_at(handle, {static_cast<SHORT>(left + 3), static_cast<SHORT>(top + 6)},
                                L"    ▄▀▀▀▀▄");
        constexpr SHORT text_column = 30;
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 3), text_column, L"Arny · your coding companion", amber_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 4), text_column, L"Native CLI · bring your own model", normal_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 5), text_column, L"Keys stay in memory, never on disk", normal_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 6), text_column, L"Type / for live hints · /help for all commands", dim_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 7), text_column, L"Press Tab to complete a command", dim_color);
        write_panel_text(handle, left, right, static_cast<SHORT>(top + 8), 2,
                         L"Connect: /key-gemini <key>  or  /key-deepseek <key>", dim_color);
    }

    SetConsoleTextAttribute(handle, normal_color);
    SetConsoleCursorPosition(handle, {0, static_cast<SHORT>(top + height + 1)});
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string lower_ascii(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

std::string remove_quotes(std::string text) {
    text = trim(std::move(text));
    return text.size() >= 2 && text.front() == '"' && text.back() == '"'
        ? text.substr(1, text.size() - 2) : text;
}

void print_models(const std::vector<std::string>& models) {
    std::cout << cyan << "Available models" << reset << "\n";
    for (const auto& model : models) std::cout << "  " << amber << "•" << reset << " " << model << '\n';
}

void print_help() {
    std::cout << '\n' << cyan << "Commands" << reset << "\n"
              << amber << "  /key-gemini <key>" << reset << "       Connect Gemini and show available models\n"
              << amber << "  /key-deepseek <key>" << reset << "     Connect DeepSeek and show available models\n"
              << amber << "  /model <name>" << reset << "           Select an available model\n"
              << amber << "  /models" << reset << "                 Show models for the active key\n"
              << amber << "  /provider <name>" << reset << "        Switch provider and reset this session\n"
              << amber << "  /status" << reset << "                 Show provider, model, and key status\n"
              << amber << "  /clear-session" << reset << "          Forget this chat's context\n"
              << amber << "  /clear" << reset << "                  Redraw the welcome screen\n"
              << amber << "  /exit" << reset << "                   End the session\n\n"
              << dim << "Write a normal message to ask the selected model. ARN can read project files automatically; "
              << "file changes always ask first. Keys live only until /exit.\n\n" << reset;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const auto length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

constexpr std::array command_hints{
    std::pair{"/key-gemini", "connect Gemini"},
    std::pair{"/key-deepseek", "connect DeepSeek"},
    std::pair{"/model", "choose a model"},
    std::pair{"/models", "list available models"},
    std::pair{"/provider", "switch provider"},
    std::pair{"/status", "show session status"},
    std::pair{"/clear-session", "forget chat context"},
    std::pair{"/help", "show all commands"},
    std::pair{"/clear", "redraw welcome"},
    std::pair{"/exit", "leave arn"},
};

std::vector<std::pair<std::string_view, std::string_view>> matching_commands(const std::wstring& buffer) {
    const auto typed = to_utf8(buffer);
    if (!typed.starts_with('/') || typed.find_first_of(" \t") != std::string::npos) return {};
    std::vector<std::pair<std::string_view, std::string_view>> matches;
    for (const auto& command : command_hints) {
        if (std::string_view(command.first).starts_with(typed)) {
            matches.emplace_back(command.first, command.second);
        }
    }
    return matches;
}

void draw_suggestions(const std::wstring& buffer) {
    const auto matches = matching_commands(buffer);
    // Reserve exactly one line under the prompt. Explicit cursor motion works
    // reliably in Windows Terminal, unlike the save/restore cursor sequences.
    std::cout << "\n\x1b[2K\r";
    if (!matches.empty()) {
        std::cout << dim << "  " << reset;
        for (std::size_t index = 0; index < std::min<std::size_t>(matches.size(), 3); ++index) {
            if (index != 0) std::cout << dim << "  ·  " << reset;
            std::cout << amber << matches[index].first << reset << dim << " " << matches[index].second << reset;
        }
        if (matches.size() > 3) std::cout << dim << "  …" << reset;
    }
    std::cout << "\x1b[1A\r\x1b[2K" << cyan << "arn" << amber << " › " << reset
              << to_utf8(buffer) << std::flush;
}

void redraw_input(const std::wstring& buffer) {
    std::cout << "\r\x1b[2K" << cyan << "arn" << amber << " › " << reset << to_utf8(buffer);
    draw_suggestions(buffer);
}

std::string read_command_line() {
    std::wstring buffer;
    const auto input_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD input_mode = 0;
    if (input_handle != INVALID_HANDLE_VALUE && GetConsoleMode(input_handle, &input_mode)) {
        SetConsoleMode(input_handle, input_mode | ENABLE_WINDOW_INPUT);
    }

    for (;;) {
        INPUT_RECORD record{};
        DWORD events_read = 0;
        if (input_handle == INVALID_HANDLE_VALUE ||
            !ReadConsoleInputW(input_handle, &record, 1, &events_read)) {
            return {};
        }

        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            // Terminal reflow cannot preserve an absolute-position card. Redraw
            // the small UI from scratch using the new viewport dimensions.
            clear_console_viewport();
            print_welcome();
            redraw_input(buffer);
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        const auto& key_event = record.Event.KeyEvent;
        const wchar_t key = key_event.uChar.UnicodeChar;
        if (key == L'\r') {
            // The line below the prompt is reserved for live suggestions.
            // Clear it before handing the terminal to the command result.
            std::cout << "\n\x1b[2K\r";
            return to_utf8(buffer);
        }
        if (key == L'\b' || key_event.wVirtualKeyCode == VK_BACK) {
            if (!buffer.empty()) buffer.pop_back();
            redraw_input(buffer);
            continue;
        }
        if (key == L'\t' || key_event.wVirtualKeyCode == VK_TAB) {
            const auto matches = matching_commands(buffer);
            if (!matches.empty()) {
                buffer = std::wstring(matches.front().first.begin(), matches.front().first.end());
                buffer += L' ';
            }
            redraw_input(buffer);
            continue;
        }
        if (key >= L' ') {
            buffer += key;
            redraw_input(buffer);
        }
    }
}

bool confirm_tool_change(const arn::ToolRequest& request) {
    std::cout << '\n' << amber << "Arny wants to change files: " << reset << request.summary << "\n"
              << dim << "Allow this action? [y/N]: " << reset << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    answer = lower_ascii(trim(std::move(answer)));
    return answer == "y" || answer == "yes";
}

arn::Provider provider_from_name(const std::string& name) {
    if (name == "deepseek") return arn::Provider::deepseek;
    if (name == "gemini") return arn::Provider::gemini;
    return arn::Provider::none;
}

template <typename Task>
auto run_with_spinner(Task&& task) {
    using namespace std::chrono_literals;

    constexpr std::array frames{'|', '/', '-', '\\'};
    auto result = std::async(std::launch::async, std::forward<Task>(task));
    std::size_t frame = 0;

    while (result.wait_for(100ms) != std::future_status::ready) {
        std::cout << "\r" << cyan << "Arnie is thinking " << amber << frames[frame] << reset << std::flush;
        frame = (frame + 1) % frames.size();
    }

    std::cout << "\r                          \r" << std::flush;
    return result.get();
}

int run_interactive() {
    arn::ApiClient client;
    const arn::ToolExecutor tools;
    arn::Provider provider = arn::Provider::none;
    std::string api_key;
    std::vector<std::string> models;
    std::string model;

    print_welcome();
    for (std::string input; ; ) {
        std::cout << cyan << "arn" << amber << " › " << reset;
        input = read_command_line();
        input = trim(std::move(input));
        if (input.empty()) continue;

        if (input.front() != '/') {
            std::cout << dim << "Arnie is working…" << reset << std::flush;
            const auto result = client.submit_prompt(provider, api_key, model, input, tools, confirm_tool_change);
            std::cout << "\r\x1b[2K" << std::flush;
            if (result.ok) {
                std::cout << '\n';
            } else {
                std::cout << red << "Error: " << reset;
            }
            std::cout << result.message << "\n\n";
            continue;
        }

        const auto separator = input.find_first_of(" \t");
        const auto command = lower_ascii(input.substr(0, separator));
        const auto argument = separator == std::string::npos ? "" : trim(input.substr(separator + 1));

        if (command == "/help") {
            print_help();
        } else if (command == "/exit" || command == "/quit") {
            return 0;
        } else if (command == "/clear") {
            clear_console_viewport();
            print_welcome();
        } else if (command == "/status") {
            std::cout << "Provider: " << arn::provider_name(provider) << "\n"
                      << "Model: " << (model.empty() ? "not selected" : model) << "\n"
                      << "API key: " << (api_key.empty() ? "not set" : "set for this session") << "\n"
                      << "Chat context: " << (client.session_entries() == 0 ? "empty" : "active") << "\n";
        } else if (command == "/clear-session") {
            client.reset_session();
            std::cout << green << "✓ " << reset << "Chat context cleared. Your key and model are unchanged.\n";
        } else if (command == "/models") {
            if (models.empty()) std::cout << "No verified API key is active.\n";
            else print_models(models);
        } else if (command == "/provider") {
            const auto selected = provider_from_name(lower_ascii(argument));
            if (selected == arn::Provider::none) {
                std::cout << "Supported providers: deepseek, gemini\n";
            } else {
                provider = selected;
                api_key.clear();
                models.clear();
                model.clear();
                client.reset_session();
                std::cout << green << "✓ " << reset << "Active provider: " << arn::provider_name(provider) << "\n";
            }
        } else if (command == "/key-deepseek" || command == "/key-gemini") {
            const auto selected = command == "/key-deepseek" ? arn::Provider::deepseek : arn::Provider::gemini;
            const auto candidate = remove_quotes(argument);
            const auto result = run_with_spinner([&] {
                return client.list_models(selected, candidate);
            });
            if (!result.ok) {
                std::cout << red << "✗ " << reset << "Key was not saved: " << result.message << "\n";
            } else if (result.models.empty()) {
                std::cout << "Key was not saved: no text-generation models are available.\n";
            } else {
                provider = selected;
                api_key = candidate;
                models = result.models;
                model = models.front();
                client.reset_session();
                std::cout << green << "✓ " << reset << result.message << " Default model: " << amber << model << reset << "\n";
                print_models(models);
                std::cout << "Use /model <name> to choose another model.\n";
            }
        } else if (command == "/model") {
            const auto selected = remove_quotes(argument);
            if (std::ranges::find(models, selected) == models.end()) {
                std::cout << "That model is not in the active provider's list. Use /models.\n";
            } else {
                client.reset_session();
                model = selected;
                std::cout << green << "✓ " << reset << "Active model: " << amber << model << reset << "\n";
            }
        } else {
            std::cout << "Unknown command. Type /help for commands.\n";
        }
    }
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    enable_ansi_colors();
    return run_interactive();
}
