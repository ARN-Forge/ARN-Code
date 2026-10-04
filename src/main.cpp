#include "agent/coding_prompt.hpp"
#include "agent_workflow.hpp"
#include "cli_support.hpp"
#include "model_provider.hpp"
#include "normal_chat.hpp"
#include "server.hpp"
#include "terminal.hpp"
#include "tools/coding_tools.hpp"
#include <arn/core/agent/agent_session.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view reset = "\x1b[0m", cyan = "\x1b[38;5;81m", amber = "\x1b[38;5;215m";
constexpr std::string_view orange = "\x1b[38;5;208m", green = "\x1b[38;5;114m";
constexpr std::string_view dim = "\x1b[90m", red = "\x1b[38;5;203m";
#if defined(__APPLE__)
constexpr std::string_view copy_shortcut = "Command+C";
#elif defined(_WIN32)
constexpr std::string_view copy_shortcut = "Ctrl+C";
#else
constexpr std::string_view copy_shortcut = "Ctrl+Shift+C";
#endif

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
    explicit Ui(const arn::TerminalSession& terminal) : terminal_(terminal) {}

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
    [[nodiscard]] std::pair<int, int> dimensions() const {
        const auto dimensions = terminal_.size();
        return {dimensions.width, dimensions.height};
    }
    // render() clears the screen once before drawing. Clearing each individual
    // cell would erase content that was drawn earlier on the same row.
    static void at(int row, int column, const std::string& text) { std::cout << "\x1b[" << row << ';' << column << "H" << text; }
    static void clear_line(int row) { std::cout << "\x1b[" << row << ";1H\x1b[2K"; }
    std::string current_hint() const {
        if (copy_mode_) return "COPY MODE · Drag to select, then " + std::string(copy_shortcut) + " · F2 returns to scrolling";
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
    const arn::TerminalSession& terminal_;
    std::string input_, hint_, status_{"Type /help for commands · F2 copy mode"}, provider_{"none"}, model_;
    std::size_t scroll_offset_{};
    bool copy_mode_{};
    bool context_{};
};

constexpr std::array command_hints{"/agent", "/key-gemini", "/key-deepseek", "/key-openrouter", "/key-omniroute", "/model", "/models", "/provider", "/status", "/clear-session", "/clear", "/help", "/exit"};

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

void erase_last_utf8_character(std::string& text) {
    if (text.empty()) return;
    auto position = text.size() - 1;
    while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80) --position;
    text.erase(position);
}

std::string read_line(Ui& ui, arn::TerminalSession& terminal) {
    std::string buffer;
    for (;;) {
        const auto event = terminal.read_event();
        if (event.type == arn::TerminalEventType::resize) { ui.render(); continue; }
        if (event.type == arn::TerminalEventType::wheel_up) { ui.scroll(3); ui.render(); continue; }
        if (event.type == arn::TerminalEventType::wheel_down) { ui.scroll(-3); ui.render(); continue; }
        if (event.type == arn::TerminalEventType::f2) {
            terminal.set_copy_mode(!terminal.copy_mode());
            ui.copy_mode(terminal.copy_mode());
            ui.render_input();
            continue;
        }
        if (event.type == arn::TerminalEventType::page_up) { ui.scroll(8); ui.render(); continue; }
        if (event.type == arn::TerminalEventType::page_down) { ui.scroll(-8); ui.render(); continue; }
        if (event.type == arn::TerminalEventType::end) { ui.scroll_to_bottom(); ui.render(); continue; }
        if (event.type == arn::TerminalEventType::enter) { ui.input({}); ui.hint({}); return buffer; }
        if (event.type == arn::TerminalEventType::end_of_input) return "/exit";
        if (event.type == arn::TerminalEventType::interrupt || event.type == arn::TerminalEventType::none) continue;
        if (terminal.copy_mode() && event.type == arn::TerminalEventType::character) {
            terminal.set_copy_mode(false);
            ui.copy_mode(false);
        }
        if (event.type == arn::TerminalEventType::backspace) erase_last_utf8_character(buffer);
        else if (event.type == arn::TerminalEventType::tab) {
            for (const auto hint : command_hints) {
                if (std::string_view(hint).starts_with(buffer)) { buffer = hint; buffer += ' '; break; }
            }
        } else if (event.type == arn::TerminalEventType::character) buffer += event.text;
        ui.input(arn::safe_command_display(buffer));
        const auto pending_key = arn::parse_api_key_command(buffer);
        ui.hint(pending_key
            ? (pending_key->has_inline_value
                ? "Inline key input is disabled. Remove the masked text, then press Enter."
                : "Press Enter to open secure API-key input, then type or paste your key.")
            : input_hint(buffer));
        ui.render_input();
    }
}

std::optional<std::string> read_api_key(Ui& ui, arn::TerminalSession& terminal,
                                        std::string_view provider_name) {
    arn::SecretInputBuffer input;
    ui.status(std::string(provider_name) + " API key: type securely below");
    ui.hint("Enter submits · Esc or Ctrl+C cancels · key is kept in memory only");
    ui.input({});
    ui.render();
    while (input.state() == arn::SecretInputState::editing) {
        const auto event = terminal.read_event();
        if (event.type == arn::TerminalEventType::resize) {
            ui.render();
            continue;
        }
        input.consume(event);
        ui.input(input.masked());
        ui.render_input();
    }
    ui.input({});
    ui.hint({});
    return input.take_submitted_secret();
}

int run() {
    arn::TerminalSession terminal;
    Ui ui(terminal);
    arn::core::AgentSession session;
    session.set_system_instruction(arn::coding_prompt());
    const arn::ToolExecutor tools;
    session.set_tools(tools.registry_ptr());
    arn::Provider provider = arn::Provider::none;
    std::string key, model;
    std::vector<std::string> models;
    bool context_active = false;
    // Never query AgentSession from a streaming or confirmation callback:
    // prompt() owns the session mutex until the request finishes.
    const auto refresh = [&] { ui.session(provider, model, context_active); ui.render(); };
    refresh();
    for (;;) {
        auto input = normalize_command(trim(read_line(ui, terminal)));
        if (input.empty()) { refresh(); continue; }
        const auto key_command = arn::parse_api_key_command(input);
        const auto separator = input.find_first_of(" \t");
        const auto command = input.front() == '/'
            ? lower_ascii(input.substr(0, separator)) : std::string{};
        ui.add(Tone::user, "arn › " + arn::safe_command_echo(input));
        if (key_command && key_command->has_inline_value) {
            std::ranges::fill(input, '\0');
            input = command;
        }
        const auto argument = separator == std::string::npos || key_command
            ? std::string{} : trim(input.substr(separator + 1));
        if (input.front() != '/') {
            ui.add(Tone::normal, {}); ui.status("Arnie is working… Esc or Ctrl+C cancels"); refresh();
            std::atomic_bool cancelled = false;
            arn::TerminalCancellationMonitor watcher(cancelled, [&session] { session.cancel_active_request(); });
            const auto confirm = [&](const arn::core::ConfirmationRequest& request) {
                watcher.pause_input();
                ui.status("Allow file change? " + request.summary + " [y/N]");
                refresh();
                const int answer = terminal.read_confirmation();
                ui.status("Type /help for commands · F2 copy mode");
                watcher.resume_input();
                return answer == 'y' || answer == 'Y';
            };
            session.set_confirmation_handler(confirm);
            const auto chat = arn::run_normal_chat(
                session, input,
                {
                    .append_text = [&](std::string_view text) { ui.append(text); },
                    .render = refresh,
                },
                &cancelled);
            context_active = chat.context_active;
            const auto& result = chat.api;
            if (result.cancelled) ui.add(Tone::warning, "Request cancelled.");
            else if (!result.ok) ui.add(Tone::error, "Error: " + result.message);
            else if (result.message.empty()) ui.add(Tone::muted, "Done.");
            ui.status("Type /help for commands · F2 copy mode"); refresh(); continue;
        }
        if (command == "/exit" || command == "/quit") return 0;
        if (command == "/clear") ui.clear();
        else if (command == "/help") {
            for (const auto& line : arn::help_lines()) ui.add(Tone::muted, line);
        }
        else if (command == "/status") {
            if (provider == arn::Provider::omniroute) {
                const auto* active = session.provider();
                auto configured = active && active->kind() == provider && !key.empty()
                    ? std::unique_ptr<arn::IModelProvider>{} : arn::make_provider(provider);
                const auto endpoint = arn::provider_endpoint(configured ? *configured : *active);
                const std::string base_url = endpoint.empty() ? "invalid endpoint configuration" : endpoint;
                ui.add(Tone::normal, "Provider: " + arn::provider_name(provider) + " | Model: " + (model.empty() ? "not selected" : model) + " | API key: " + (key.empty() ? "not set" : "set") + " | Endpoint: " + base_url + " | Context: " + (context_active ? "active" : "empty"));
            } else {
                ui.add(Tone::normal, "Provider: " + arn::provider_name(provider) + " | Model: " + (model.empty() ? "not selected" : model) + " | API key: " + (key.empty() ? "not set" : "set") + " | Context: " + (context_active ? "active" : "empty"));
            }
        }
        else if (command == "/clear-session") { session.reset_session(); context_active = false; ui.add(Tone::good, "Chat context cleared. Key and model are unchanged."); }
        else if (command == "/models") { if (models.empty()) ui.add(Tone::warning, "No verified API key is active."); else for (const auto& name : models) ui.add(Tone::normal, "• " + name); }
        else if (const auto agent_command = arn::parse_agent_command(input); agent_command) {
            auto workflow = arn::create_agent_orchestrator({
                .provider = provider,
                .api_key = key,
                .model = model,
                .project_root = tools.project_root(),
            });
            std::atomic_bool cancelled = false;
            arn::TerminalCancellationMonitor watcher(
                cancelled, [&workflow] { workflow->cancel_active_execution(); });
            const auto confirm = [&](const arn::core::ConfirmationRequest& request) {
                watcher.pause_input();
                ui.status("Allow file change? " + request.summary + " [y/N]");
                refresh();
                const int answer = terminal.read_confirmation();
                ui.status("Type /help for commands · F2 copy mode");
                watcher.resume_input();
                if (cancelled.load() || answer == 3 || answer == 27) {
                    cancelled.store(true);
                    workflow->cancel_active_execution();
                    return false;
                }
                return answer == 'y' || answer == 'Y';
            };
            const auto output = [&](arn::AgentOutputLevel level, std::string text) {
                Tone tone = Tone::normal;
                if (level == arn::AgentOutputLevel::good) tone = Tone::good;
                else if (level == arn::AgentOutputLevel::warning) tone = Tone::warning;
                else if (level == arn::AgentOutputLevel::error) tone = Tone::error;
                ui.add(tone, std::move(text));
                refresh();
            };
            const auto decide_plan = [&](const arn::core::ContextArtifact&) {
                watcher.pause_input();
                ui.status("Proceed with Coder? [y/N]");
                refresh();
                const int answer = terminal.read_confirmation();
                ui.status("Type /help for commands · F2 copy mode");
                watcher.resume_input();
                if (cancelled.load() || answer == 3 || answer == 27) {
                    cancelled.store(true);
                    workflow->cancel_active_execution();
                    return arn::core::ContinuationDecision::cancel;
                }
                return answer == 'y' || answer == 'Y'
                    ? arn::core::ContinuationDecision::proceed
                    : arn::core::ContinuationDecision::decline;
            };
            ui.status(agent_command->mode == arn::AgentCommandMode::direct
                          ? "Direct agent running… Esc or Ctrl+C cancels"
                          : "Multi-agent workflow running… Esc or Ctrl+C cancels");
            refresh();
            (void)arn::run_agent_command(*agent_command, tools.project_root(), *workflow,
                                         {.output = output, .decide_plan = decide_plan}, confirm);
        } else if (command == "/provider") {
            const auto selected = arn::provider_from_name(lower_ascii(argument));
            if (selected == arn::Provider::none) ui.add(Tone::warning, "Supported providers: gemini, deepseek, openrouter, omniroute");
            else { provider = selected; key.clear(); model.clear(); models.clear(); session.reset_session(); context_active = false; ui.add(Tone::good, "Active provider: " + arn::provider_name(provider)); }
        } else if (key_command) {
            const auto selected = [&] {
                switch (key_command->command) {
                case arn::ApiKeyCommand::gemini: return arn::Provider::gemini;
                case arn::ApiKeyCommand::deepseek: return arn::Provider::deepseek;
                case arn::ApiKeyCommand::openrouter: return arn::Provider::openrouter;
                case arn::ApiKeyCommand::omniroute: return arn::Provider::omniroute;
                }
                return arn::Provider::none;
            }();
            if (key_command->has_inline_value) {
                ui.add(Tone::warning, "Inline API key entry is disabled. Run " + command +
                                      " without an argument to enter it securely.");
                ui.status("Type /help for commands · F2 copy mode");
                refresh();
                continue;
            }
            const auto candidate = read_api_key(ui, terminal, arn::provider_name(selected));
            if (!candidate) {
                ui.add(Tone::warning, "API key entry cancelled or empty. Existing configuration is unchanged.");
                ui.status("Type /help for commands · F2 copy mode");
                refresh();
                continue;
            }
            ui.status("Verifying API key…"); refresh();
            auto prov = arn::make_provider(selected);
            if (!prov) {
                ui.add(Tone::error, "Provider not available.");
            } else {
                const auto result = session.configure_provider(std::move(prov), *candidate);
                if (!result.ok || session.available_models().empty()) {
                    ui.add(Tone::error, "Key was not saved: " + (result.ok ? "no text-generation models are available." : result.message));
                } else {
                    provider = selected;
                    key = *candidate;
                    models = session.available_models();
                    model = session.preferred_model();
                    session.select_model(model);
                    session.reset_session();
                    context_active = false;
                    ui.add(Tone::good, "Connected to " + arn::provider_name(provider) + ". Default model: " + model);
                }
            }
        } else if (command == "/model") {
            const auto selected = remove_quotes(argument);
            if (std::ranges::find(models, selected) == models.end()) ui.add(Tone::warning, "That model is not in the active provider list. Use /models.");
            else { model = selected; session.select_model(model); session.reset_session(); context_active = false; ui.add(Tone::good, "Active model: " + model); }
        } else ui.add(Tone::error, "Unknown command. Type /help for commands.");
        ui.status("Type /help for commands · F2 copy mode"); refresh();
    }
}

} // namespace

#ifndef ARN_VERSION
#define ARN_VERSION "dev"
#endif

int main(int argc, char** argv) {
    if (argc > 1) {
        const auto arg = std::string_view(argv[1]);
        if (arg == "--version" || arg == "-v") {
            std::cout << "arn " << ARN_VERSION << '\n';
            return 0;
        }
        if (arg == "--server") {
            return arn::run_server();
        }
    }
    return run();
}
