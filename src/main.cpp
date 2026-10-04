#include "agent/coding_prompt.hpp"
#include "agent_workflow.hpp"
#include "cli_support.hpp"
#include "model_provider.hpp"
#include "normal_chat.hpp"
#include "server.hpp"
#include "terminal.hpp"
#include "terminal_ui.hpp"
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

using arn::terminal_ui::Tone;
using arn::terminal_ui::Ui;

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
    ui.input({});
    ui.render_input();
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

int read_confirmation(Ui& ui, arn::TerminalSession& terminal) {
    const int answer = terminal.read_confirmation(
        [&](std::string_view choice) { ui.show_confirmation(choice); });
    ui.finish_confirmation(answer);
    return answer;
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
    ui.status(ui.ready_status());
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
    const auto refresh = [&] {
        ui.session(arn::provider_name(provider), model, context_active);
        ui.render();
    };
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
                const int answer = read_confirmation(ui, terminal);
                if (answer == 3 || answer == 27) cancelled.store(true);
                ui.status("Arnie is working… Esc or Ctrl+C cancels");
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
            watcher.pause_input();
            ui.status(ui.ready_status()); refresh(); continue;
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
                const int answer = read_confirmation(ui, terminal);
                ui.status("Agent workflow running… Esc or Ctrl+C cancels");
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
                const int answer = read_confirmation(ui, terminal);
                ui.status("Agent workflow running… Esc or Ctrl+C cancels");
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
            watcher.pause_input();
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
                ui.status(ui.ready_status());
                refresh();
                continue;
            }
            const auto candidate = read_api_key(ui, terminal, arn::provider_name(selected));
            if (!candidate) {
                ui.add(Tone::warning, "API key entry cancelled or empty. Existing configuration is unchanged.");
                ui.status(ui.ready_status());
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
        ui.status(ui.ready_status()); refresh();
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
