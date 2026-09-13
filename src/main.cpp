#include "api_client.hpp"

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

std::size_t console_width() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info)) {
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    }
    return 100;
}

void panel_line(std::string_view text, std::size_t width, std::string_view color = reset) {
    const auto available = width - 4;
    const auto visible = std::min(text.size(), available);
    std::cout << "  " << cyan << "│ " << color << text.substr(0, visible) << reset
              << std::string(available - visible, ' ') << cyan << " │\n";
}

void print_welcome() {
    // Keep a four-cell safety margin: writing into the last console column can wrap
    // and split a vertical border in Windows Terminal.
    const auto width = std::max<std::size_t>(76, console_width() - 8);
    std::string horizontal;
    for (std::size_t index = 0; index < width - 2; ++index) horizontal += "─";

    std::cout << '\n'
              << "  " << cyan << "╭" << horizontal << "╮\n" << reset;
    panel_line("ARN AGENT CODE", width, cyan);
    panel_line("", width);
    panel_line("       _.--._            Arny · your shell-side armadillo", width, amber);
    panel_line("   _.-'  _  _ '-._       Native CLI · bring your own model", width, amber);
    panel_line("  /  .-'(o)(o)'-.  \\    Keys stay in memory, never on disk", width, amber);
    panel_line(" |  /      __     \\ |   Type / for live command hints", width, amber);
    panel_line("  \\ \\_.-'____'-._/ /    Type /help to see every command", width, amber);
    panel_line("   '-._   /__\\  _.-'     Press Tab to complete a command", width, amber);
    panel_line("       '-.____.-'", width, amber);
    panel_line("", width);
    panel_line("Connect: /key-gemini <key>  or  /key-deepseek <key>", width, dim);
    std::cout << "  " << cyan << "╰" << horizontal << "╯\n" << reset << '\n';
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
              << amber << "  /clear" << reset << "                  Redraw the welcome screen\n"
              << amber << "  /exit" << reset << "                   End the session\n\n"
              << dim << "Write a normal message to ask the selected model. Keys live only until /exit.\n\n" << reset;
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
    for (;;) {
        const wchar_t key = _getwch();
        if (key == L'\r') {
            // The line below the prompt is reserved for live suggestions.
            // Clear it before handing the terminal to the command result.
            std::cout << "\n\x1b[2K\r";
            return to_utf8(buffer);
        }
        if (key == L'\b') {
            if (!buffer.empty()) buffer.pop_back();
            redraw_input(buffer);
            continue;
        }
        if (key == L'\t') {
            const auto matches = matching_commands(buffer);
            if (!matches.empty()) {
                buffer = std::wstring(matches.front().first.begin(), matches.front().first.end());
                buffer += L' ';
            }
            redraw_input(buffer);
            continue;
        }
        if (key == 0 || key == 0xE0) {
            (void)_getwch();
            continue;
        }
        if (key >= L' ') {
            buffer += key;
            redraw_input(buffer);
        }
    }
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
            const auto result = run_with_spinner([&] {
                return client.submit_prompt(provider, api_key, model, input);
            });
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
            std::system("cls");
            print_welcome();
        } else if (command == "/status") {
            std::cout << "Provider: " << arn::provider_name(provider) << "\n"
                      << "Model: " << (model.empty() ? "not selected" : model) << "\n"
                      << "API key: " << (api_key.empty() ? "not set" : "set for this session") << "\n";
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
                std::cout << green << "✓ " << reset << result.message << " Default model: " << amber << model << reset << "\n";
                print_models(models);
                std::cout << "Use /model <name> to choose another model.\n";
            }
        } else if (command == "/model") {
            const auto selected = remove_quotes(argument);
            if (std::ranges::find(models, selected) == models.end()) {
                std::cout << "That model is not in the active provider's list. Use /models.\n";
            } else {
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
