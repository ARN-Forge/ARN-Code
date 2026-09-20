#include "api_client.hpp"
#include "tool_executor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view reset = "\x1b[0m", cyan = "\x1b[38;5;81m", amber = "\x1b[38;5;215m";
constexpr std::string_view orange = "\x1b[38;5;208m", green = "\x1b[38;5;114m";
constexpr std::string_view dim = "\x1b[90m", red = "\x1b[38;5;203m";

std::atomic<std::atomic_bool*> active_cancel_flag = nullptr;

BOOL WINAPI handle_console_control(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (auto* flag = active_cancel_flag.load(std::memory_order_acquire)) {
        flag->store(true, std::memory_order_relaxed);
    }
    // Windows Terminal handles Ctrl+C itself while text is selected. If the
    // event reaches ARN, consume it so copying (or an accidental Ctrl+C at the
    // prompt) never terminates the whole session. During a request it still
    // acts as cancellation through active_cancel_flag.
    return TRUE;
}

class AlternateScreen {
public:
    AlternateScreen() {
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output_ != INVALID_HANDLE_VALUE && GetConsoleMode(output_, &mode_)) {
            SetConsoleMode(output_, mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        if (input_ != INVALID_HANDLE_VALUE && GetConsoleMode(input_, &input_mode_)) {
            input_mode_changed_ = true;
            set_copy_mode(false);
        }
        std::cout << "\x1b[?1049h\x1b[2J\x1b[H" << std::flush;
    }
    ~AlternateScreen() {
        std::cout << reset << "\x1b[?25h\x1b[?1049l" << std::flush;
        if (output_ != INVALID_HANDLE_VALUE) SetConsoleMode(output_, mode_);
        if (input_mode_changed_) SetConsoleMode(input_, input_mode_);
    }
    void set_copy_mode(bool enabled) {
        if (!input_mode_changed_) return;
        DWORD interactive_mode = input_mode_ | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
        if (enabled) {
            interactive_mode |= ENABLE_QUICK_EDIT_MODE;
            interactive_mode &= ~ENABLE_MOUSE_INPUT;
        } else {
            interactive_mode |= ENABLE_MOUSE_INPUT;
            interactive_mode &= ~ENABLE_QUICK_EDIT_MODE;
        }
        SetConsoleMode(input_, interactive_mode);
        copy_mode_ = enabled;
    }
    [[nodiscard]] bool copy_mode() const { return copy_mode_; }
private:
    HANDLE output_{INVALID_HANDLE_VALUE};
    HANDLE input_{INVALID_HANDLE_VALUE};
    DWORD mode_{};
    DWORD input_mode_{};
    bool input_mode_changed_{};
    bool copy_mode_{};
};

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string lower_ascii(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return text;
}

std::string remove_quotes(std::string text) {
    text = trim(std::move(text));
    return text.size() >= 2 && text.front() == '"' && text.back() == '"' ? text.substr(1, text.size() - 2) : text;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::size_t char_bytes(unsigned char byte) {
    if ((byte & 0x80) == 0) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;
}

std::size_t display_columns(std::string_view text) {
    std::size_t columns = 0;
    for (std::size_t index = 0; index < text.size();) {
        index += std::min(char_bytes(static_cast<unsigned char>(text[index])), text.size() - index);
        ++columns;
    }
    return columns;
}

std::vector<std::string> wrap(std::string_view text, std::size_t width) {
    std::vector<std::string> output;
    std::string line;
    std::size_t columns = 0;
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\n') { output.push_back(std::move(line)); line.clear(); columns = 0; ++index; continue; }
        const auto count = std::min(char_bytes(static_cast<unsigned char>(text[index])), text.size() - index);
        if (columns == width) { output.push_back(std::move(line)); line.clear(); columns = 0; }
        line.append(text.substr(index, count));
        index += count;
        ++columns;
    }
    if (!line.empty() || output.empty()) output.push_back(std::move(line));
    return output;
}

std::string horizontal_rule(std::size_t cells) {
    std::string result;
    result.reserve(cells * 3);
    for (std::size_t index = 0; index < cells; ++index) result += "─";
    return result;
}

enum class Tone { normal, user, muted, good, warning, error };
struct Entry { Tone tone; std::string text; };

class Ui {
public:
    void add(Tone tone, std::string text) {
        log_.push_back({tone, std::move(text)});
        if (log_.size() > 180) log_.erase(log_.begin(), log_.begin() + 20);
        scroll_offset_ = 0;
    }
    void append(std::string_view text) { if (log_.empty()) add(Tone::normal, {}); log_.back().text.append(text); scroll_offset_ = 0; }
    void clear() { log_.clear(); scroll_offset_ = 0; }
    void input(std::string value) { input_ = std::move(value); }
    void hint(std::string value) { hint_ = std::move(value); }
    void status(std::string value) { status_ = std::move(value); }
    void copy_mode(bool enabled) { copy_mode_ = enabled; }
    void session(arn::Provider provider, const std::string& model, bool context) {
        provider_ = arn::provider_name(provider); model_ = model; context_ = context;
    }

    void scroll(int amount) {
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) return;
        const int inner = width - 4;
        const int header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        const int visible_rows = std::max(0, height - 3 - (header + 2));
        std::size_t total_rows = 0;
        for (const auto& entry : log_) total_rows += wrap(entry.text, static_cast<std::size_t>(inner - 2)).size();
        const auto maximum = total_rows > static_cast<std::size_t>(visible_rows)
            ? total_rows - static_cast<std::size_t>(visible_rows) : 0;
        if (amount > 0) scroll_offset_ = std::min(maximum, scroll_offset_ + static_cast<std::size_t>(amount));
        else scroll_offset_ = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(scroll_offset_) + amount));
    }

    void scroll_to_bottom() { scroll_offset_ = 0; }

    void render() const {
        const auto [width, height] = dimensions();
        std::cout << "\x1b[?2026h\x1b[?25l\x1b[2J\x1b[H";
        if (width < 38 || height < 13) {
            std::cout << cyan << "ARN" << reset << " — enlarge terminal\x1b[?25h\x1b[?2026l" << std::flush;
            return;
        }
        const int left = 2, inner = width - 4, header = inner < 58 ? 5 : inner < 76 ? 7 : 9;
        header_box(left, inner, header);
        const int content_top = header + 2, footer = height - 3, rows = std::max(0, footer - content_top);
        std::vector<Entry> lines;
        for (const auto& entry : log_) for (auto line : wrap(entry.text, static_cast<std::size_t>(inner - 2))) lines.push_back({entry.tone, std::move(line)});
        const std::size_t bottom = lines.size() > static_cast<std::size_t>(rows) ? lines.size() - static_cast<std::size_t>(rows) : 0;
        const std::size_t start = bottom > scroll_offset_ ? bottom - scroll_offset_ : 0;
        for (int row = 0; row < rows; ++row) if (start + row < lines.size()) at(content_top + row, left, color(lines[start + row].tone) + lines[start + row].text + std::string(reset));
        at(footer, 1, std::string(dim) + horizontal_rule(static_cast<std::size_t>(width - 2)) + std::string(reset));
        at(footer + 1, 1, std::string(cyan) + "arn" + std::string(amber) + " › " + std::string(reset) + input_);
        const auto footer_hint = current_hint();
        at(footer + 2, 1, std::string(dim) + footer_hint + std::string(reset));
        place_cursor(footer, width);
        std::cout << "\x1b[?25h\x1b[?2026l" << std::flush;
    }

    void render_input() const {
        const auto [width, height] = dimensions();
        if (width < 38 || height < 13) { render(); return; }
        const int footer = height - 3;
        const auto footer_hint = current_hint();
        std::cout << "\x1b[?2026h\x1b[?25l";
        clear_line(footer + 1);
        std::cout << cyan << "arn" << amber << " › " << reset << input_;
        clear_line(footer + 2);
        std::cout << dim << footer_hint << reset;
        place_cursor(footer, width);
        std::cout << "\x1b[?25h\x1b[?2026l" << std::flush;
    }

private:
    static std::pair<int, int> dimensions() {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return {info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1};
        return {100, 30};
    }
    // render() clears the screen once before drawing. Clearing each individual
    // cell would erase content that was drawn earlier on the same row.
    static void at(int row, int column, const std::string& text) { std::cout << "\x1b[" << row << ';' << column << "H" << text; }
    static void clear_line(int row) { std::cout << "\x1b[" << row << ";1H\x1b[2K"; }
    std::string current_hint() const {
        if (copy_mode_) return "COPY MODE · Drag to select, then Ctrl+C · F2 returns to scrolling";
        if (scroll_offset_ != 0) return "Scrolled up · Wheel down or End returns to the latest message · Shift+drag selects text";
        return hint_.empty() ? status_ : hint_;
    }
    void place_cursor(int footer, int width) const {
        std::cout << "\x1b[" << footer + 1 << ';'
                  << std::min(width - 1, 7 + static_cast<int>(display_columns(input_))) << 'H';
    }
    static std::string color(Tone tone) {
        switch (tone) {
        case Tone::user: return std::string(cyan);
        case Tone::muted: return std::string(dim);
        case Tone::good: return std::string(green);
        case Tone::warning: return std::string(amber);
        case Tone::error: return std::string(red);
        default: return std::string(reset);
        }
    }
    void header_box(int left, int width, int height) const {
        const std::string rule = horizontal_rule(static_cast<std::size_t>(width - 2));
        at(1, left, std::string(cyan) + "╭" + rule + "╮" + std::string(reset));
        for (int row = 2; row < height; ++row) { at(row, left, std::string(cyan) + "│" + std::string(reset)); at(row, left + width - 1, std::string(cyan) + "│" + std::string(reset)); }
        at(height, left, std::string(cyan) + "╰" + rule + "╯" + std::string(reset));
        at(2, left + 2, std::string(cyan) + "ARN AGENT CODE" + std::string(reset));
        if (height == 5) {
            at(3, left + 2, std::string(amber) + "Arny · native coding companion" + std::string(reset));
        } else if (height == 7) {
            at(3, left + 2, std::string(amber) + "Arny · native coding companion" + std::string(reset));
            at(4, left + 2, std::string(dim) + "Gemini / DeepSeek · streaming · safe file tools" + std::string(reset));
            at(5, left + 2, std::string(dim) + "Esc cancels · keys and context stay in memory" + std::string(reset));
        } else {
            at(4, left + 3, std::string(orange) + "▄▄          ▄▄" + std::string(reset));
            at(5, left + 3, std::string(orange) + "▄▀██▄▀██▀▄██▀▄" + std::string(reset));
            at(6, left + 3, std::string(orange) + "   ▀██████▀" + std::string(reset));
            at(7, left + 3, std::string(orange) + "    ▄▀▀▀▀▄" + std::string(reset));
            at(4, left + 30, std::string(amber) + "Arny · your coding companion" + std::string(reset));
            at(5, left + 30, "Native CLI · bring your own model");
            at(6, left + 30, "Streaming replies · safe local file tools");
            at(7, left + 30, std::string(dim) + "Esc cancels · keys and context stay in memory" + std::string(reset));
        }
        if (height > 5) {
            at(height - 1, left + 2, std::string(dim) + "Provider: " + provider_ + " · Model: " + (model_.empty() ? "not selected" : model_) + " · Context: " + (context_ ? "active" : "empty") + std::string(reset));
        }
    }
    std::vector<Entry> log_;
    std::string input_, hint_, status_{"Type /help for commands · F2 copy mode"}, provider_{"none"}, model_;
    std::size_t scroll_offset_{};
    bool copy_mode_{};
    bool context_{};
};

class CancellationWatcher {
public:
    CancellationWatcher(std::atomic_bool& cancelled, arn::ApiClient& client) : cancelled_(cancelled), client_(client) {
        active_cancel_flag.store(&cancelled_, std::memory_order_release);
        thread_ = std::jthread([this](std::stop_token stop) {
            bool previous = false;
            while (!stop.stop_requested()) {
                const bool escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if ((escape && !previous) || cancelled_.load(std::memory_order_relaxed)) { cancelled_.store(true, std::memory_order_relaxed); client_.cancel_active_request(); return; }
                previous = escape;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }
    ~CancellationWatcher() { thread_.request_stop(); if (thread_.joinable()) thread_.join(); active_cancel_flag.store(nullptr, std::memory_order_release); }
private:
    std::atomic_bool& cancelled_; arn::ApiClient& client_; std::jthread thread_;
};

constexpr std::array command_hints{"/key-gemini", "/key-deepseek", "/model", "/models", "/provider", "/status", "/clear-session", "/clear", "/help", "/exit"};

std::string input_hint(std::string_view input) {
    if (!input.starts_with('/') || input.find_first_of(" \t") != std::string_view::npos) {
        return "Type /help for commands · Esc cancels a request · F2 copy mode";
    }
    std::string result;
    for (const auto command : command_hints) {
        if (!std::string_view(command).starts_with(input)) continue;
        if (!result.empty()) result += "  ·  ";
        result += command;
        if (result.size() > 96) { result += "  …"; break; }
    }
    return result.empty() ? "Unknown command · Type /help" : result + "   Tab completes";
}

std::string normalize_command(std::string input) {
    if (input.empty() || input.front() == '/') return input;
    const auto separator = input.find_first_of(" \t");
    const auto first_word = lower_ascii(input.substr(0, separator));
    for (const auto command : command_hints) {
        if (first_word == std::string_view(command).substr(1)) return '/' + input;
    }
    if (first_word == "quit") return '/' + input;
    return input;
}

std::string preferred_model(arn::Provider provider, const std::vector<std::string>& models) {
    const std::vector<std::string_view> preferred = provider == arn::Provider::gemini
        ? std::vector<std::string_view>{"gemini-flash-latest", "gemini-3.5-flash-lite", "gemini-3.5-flash", "gemini-3.1-flash-lite", "gemini-3.1-flash", "gemini-2.5-flash"}
        : std::vector<std::string_view>{"deepseek-chat", "deepseek-reasoner"};
    for (const auto candidate : preferred) {
        if (std::ranges::find(models, candidate) != models.end()) return std::string(candidate);
    }
    if (provider == arn::Provider::gemini) {
        for (const auto& candidate : models) {
            const auto name = lower_ascii(candidate);
            if (name.find("gemini") != std::string::npos && name.find("flash") != std::string::npos
                && name.find("image") == std::string::npos && name.find("tts") == std::string::npos
                && name.find("transcribe") == std::string::npos) return candidate;
        }
    }
    return models.empty() ? std::string{} : models.front();
}

std::string read_line(Ui& ui, AlternateScreen& screen) {
    std::wstring buffer;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        INPUT_RECORD event{}; DWORD count{};
        if (input == INVALID_HANDLE_VALUE || !ReadConsoleInputW(input, &event, 1, &count)) return {};
        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) { ui.render(); continue; }
        if (event.EventType == MOUSE_EVENT) {
            const auto& mouse = event.Event.MouseEvent;
            if (mouse.dwEventFlags == MOUSE_WHEELED) {
                const auto delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
                ui.scroll(delta > 0 ? 3 : -3);
                ui.render();
            }
            continue;
        }
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
        const auto& key = event.Event.KeyEvent;
        if (key.wVirtualKeyCode == VK_F2) {
            screen.set_copy_mode(!screen.copy_mode());
            ui.copy_mode(screen.copy_mode());
            ui.render_input();
            continue;
        }
        if (key.wVirtualKeyCode == VK_PRIOR) { ui.scroll(8); ui.render(); continue; }
        if (key.wVirtualKeyCode == VK_NEXT) { ui.scroll(-8); ui.render(); continue; }
        if (key.wVirtualKeyCode == VK_END) { ui.scroll_to_bottom(); ui.render(); continue; }
        if (key.uChar.UnicodeChar == L'\r') { ui.input({}); ui.hint({}); return to_utf8(buffer); }
        if (key.uChar.UnicodeChar == 3) continue;
        if (screen.copy_mode() && key.uChar.UnicodeChar >= L' ') {
            screen.set_copy_mode(false);
            ui.copy_mode(false);
        }
        if (key.wVirtualKeyCode == VK_BACK) { if (!buffer.empty()) buffer.pop_back(); }
        else if (key.wVirtualKeyCode == VK_TAB) {
            const auto prefix = to_utf8(buffer);
            for (const auto hint : command_hints) if (std::string_view(hint).starts_with(prefix)) { buffer.assign(hint, hint + std::char_traits<char>::length(hint)); buffer += L' '; break; }
        } else if (key.uChar.UnicodeChar >= L' ') buffer += key.uChar.UnicodeChar;
        const auto current_input = to_utf8(buffer);
        ui.input(current_input);
        ui.hint(input_hint(current_input));
        ui.render_input();
    }
}

arn::Provider provider_from_name(const std::string& value) { return value == "gemini" ? arn::Provider::gemini : value == "deepseek" ? arn::Provider::deepseek : arn::Provider::none; }

int run() {
    AlternateScreen screen;
    Ui ui;
    arn::ApiClient client;
    const arn::ToolExecutor tools;
    arn::Provider provider = arn::Provider::none;
    std::string key, model;
    std::vector<std::string> models;
    const auto refresh = [&] { ui.session(provider, model, client.session_entries() != 0); ui.render(); };
    const auto confirm = [&](const arn::ToolRequest& request) {
        ui.status("Allow file change? " + request.summary + " [y/N]"); refresh();
        const int answer = _getch(); ui.status("Type /help for commands · F2 copy mode"); return answer == 'y' || answer == 'Y';
    };
    refresh();
    for (;;) {
        const auto input = normalize_command(trim(read_line(ui, screen)));
        if (input.empty()) { refresh(); continue; }
        ui.add(Tone::user, "arn › " + input);
        if (input.front() != '/') {
            ui.add(Tone::normal, {}); ui.status("Arnie is working… Esc or Ctrl+C cancels"); refresh();
            std::atomic_bool cancelled = false;
            CancellationWatcher watcher(cancelled, client);
            const auto result = client.submit_prompt(provider, key, model, input, tools, confirm, [&](std::string_view text) { ui.append(text); refresh(); }, &cancelled);
            if (result.cancelled) ui.add(Tone::warning, "Request cancelled.");
            else if (!result.ok) ui.add(Tone::error, "Error: " + result.message);
            else if (result.message.empty()) ui.add(Tone::muted, "Done.");
            ui.status("Type /help for commands · F2 copy mode"); refresh(); continue;
        }
        const auto separator = input.find_first_of(" \t");
        const auto command = lower_ascii(input.substr(0, separator));
        const auto argument = separator == std::string::npos ? "" : trim(input.substr(separator + 1));
        if (command == "/exit" || command == "/quit") return 0;
        if (command == "/clear") ui.clear();
        else if (command == "/help") ui.add(Tone::muted, "Commands: /key-gemini <key>, /key-deepseek <key>, /model <name>, /models, /provider <name>, /status, /clear-session, /clear, /exit");
        else if (command == "/status") ui.add(Tone::normal, "Provider: " + arn::provider_name(provider) + " | Model: " + (model.empty() ? "not selected" : model) + " | API key: " + (key.empty() ? "not set" : "set") + " | Context: " + (client.session_entries() ? "active" : "empty"));
        else if (command == "/clear-session") { client.reset_session(); ui.add(Tone::good, "Chat context cleared. Key and model are unchanged."); }
        else if (command == "/models") { if (models.empty()) ui.add(Tone::warning, "No verified API key is active."); else for (const auto& name : models) ui.add(Tone::normal, "• " + name); }
        else if (command == "/provider") {
            const auto selected = provider_from_name(lower_ascii(argument));
            if (selected == arn::Provider::none) ui.add(Tone::warning, "Supported providers: gemini, deepseek");
            else { provider = selected; key.clear(); model.clear(); models.clear(); client.reset_session(); ui.add(Tone::good, "Active provider: " + arn::provider_name(provider)); }
        } else if (command == "/key-gemini" || command == "/key-deepseek") {
            const auto selected = command == "/key-gemini" ? arn::Provider::gemini : arn::Provider::deepseek;
            ui.status("Verifying API key…"); refresh();
            const auto result = client.list_models(selected, remove_quotes(argument));
            if (!result.ok || result.models.empty()) ui.add(Tone::error, "Key was not saved: " + (result.ok ? "no text-generation models are available." : result.message));
            else { provider = selected; key = remove_quotes(argument); models = result.models; model = preferred_model(provider, models); client.reset_session(); ui.add(Tone::good, "Connected to " + arn::provider_name(provider) + ". Default model: " + model); }
        } else if (command == "/model") {
            const auto selected = remove_quotes(argument);
            if (std::ranges::find(models, selected) == models.end()) ui.add(Tone::warning, "That model is not in the active provider list. Use /models.");
            else { model = selected; client.reset_session(); ui.add(Tone::good, "Active model: " + model); }
        } else ui.add(Tone::error, "Unknown command. Type /help for commands.");
        ui.status("Type /help for commands · F2 copy mode"); refresh();
    }
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8); SetConsoleCtrlHandler(handle_console_control, TRUE);
    return run();
}
