#include "server.hpp"
#include "api_client.hpp"
#include "tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace arn {

using json = nlohmann::json;

namespace {

// Global state for the server session
struct ServerState {
    Provider provider = Provider::none;
    std::string api_key;
    std::string model;
    ApiClient client;
    ToolExecutor tools;
    std::atomic_bool request_active{false};
    std::string active_request_id;
    std::mutex state_mutex;
};

// Emit a JSON event to stdout with newline
void emit_event(const json& event) {
    std::cout << event.dump() << '\n' << std::flush;
}

// Log diagnostic message to stderr
void log_error(std::string_view msg) {
    std::cerr << "[arn-server] " << msg << '\n' << std::flush;
}

// Handle "configure" command: set provider, api_key, and model
void handle_configure(ServerState& state, const json& cmd) {
    try {
        auto provider_name = cmd.value("provider", "");
        auto key = cmd.value("apiKey", "");
        auto model_name = cmd.value("model", "");

        auto new_provider = provider_name == "gemini" ? Provider::gemini
                          : provider_name == "deepseek" ? Provider::deepseek
                          : Provider::none;

        if (new_provider == Provider::none) {
            emit_event(json{{"type", "error"}, {"message", "Unknown provider. Use 'gemini' or 'deepseek'."}});
            return;
        }

        if (key.empty()) {
            emit_event(json{{"type", "error"}, {"message", "API key is required."}});
            return;
        }

        if (model_name.empty()) {
            emit_event(json{{"type", "error"}, {"message", "Model name is required."}});
            return;
        }

        std::lock_guard<std::mutex> lock(state.state_mutex);
        state.provider = new_provider;
        state.api_key = key;
        state.model = model_name;
        state.client.reset_session();

        emit_event(json{{"type", "configured"},
                       {"provider", provider_name},
                       {"model", model_name}});
    } catch (const std::exception& e) {
        log_error(std::string("configure handler error: ") + e.what());
        emit_event(json{{"type", "error"}, {"message", "Configure failed."}});
    }
}

// Handle "prompt" command: submit a prompt and stream responses
void handle_prompt(ServerState& state, const json& cmd) {
    try {
        auto request_id = cmd.value("id", "");
        auto text = cmd.value("text", "");

        if (request_id.empty() || text.empty()) {
            emit_event(json{{"type", "error"}, {"message", "Request requires 'id' and 'text'."}});
            return;
        }

        std::lock_guard<std::mutex> lock(state.state_mutex);
        if (state.provider == Provider::none) {
            emit_event(json{{"type", "error"},
                           {"requestId", request_id},
                           {"message", "No provider configured."}});
            return;
        }

        if (state.api_key.empty()) {
            emit_event(json{{"type", "error"},
                           {"requestId", request_id},
                           {"message", "No API key set."}});
            return;
        }

        state.request_active = true;
        state.active_request_id = request_id;

        // Confirmation callback: send confirmation_required event and wait for response
        auto confirm = [&state, request_id](const ToolRequest& tool_req) {
            json event{
                {"type", "confirmation_required"},
                {"id", "change-" + std::to_string(std::hash<std::string>{}(tool_req.summary) % 1000000)},
                {"requestId", request_id},
                {"operation", tool_req.name},
                {"path", tool_req.arguments.value("path", "")},
                {"summary", tool_req.summary}
            };
            if (tool_req.arguments.contains("diff")) {
                event["diff"] = tool_req.arguments["diff"];
            }
            emit_event(event);

            // In headless mode, default to deny for safety
            return false;
        };

        // Stream callback: emit streaming text
        auto on_text = [request_id](std::string_view text) {
            emit_event(json{{"type", "stream"},
                           {"requestId", request_id},
                           {"text", std::string(text)}});
        };

        auto result = state.client.submit_prompt(state.provider, state.api_key, state.model,
                                                  text, state.tools, confirm, on_text, nullptr);

        if (result.cancelled) {
            emit_event(json{{"type", "cancelled"}, {"requestId", request_id}});
        } else if (!result.ok) {
            emit_event(json{{"type", "error"},
                           {"requestId", request_id},
                           {"message", result.message}});
        } else {
            emit_event(json{{"type", "complete"}, {"requestId", request_id}});
        }

        state.request_active = false;
        state.active_request_id.clear();
    } catch (const std::exception& e) {
        log_error(std::string("prompt handler error: ") + e.what());
        emit_event(json{{"type", "error"}, {"message", "Prompt submission failed."}});
        std::lock_guard<std::mutex> lock(state.state_mutex);
        state.request_active = false;
    }
}

// Handle "cancel" command: cancel active request
void handle_cancel(ServerState& state, const json& cmd) {
    try {
        auto request_id = cmd.value("id", "");
        std::lock_guard<std::mutex> lock(state.state_mutex);
        if (state.request_active && state.active_request_id == request_id) {
            state.client.cancel_active_request();
            emit_event(json{{"type", "cancelled"}, {"requestId", request_id}});
        }
    } catch (const std::exception& e) {
        log_error(std::string("cancel handler error: ") + e.what());
    }
}

// Handle "clear_session" command: reset conversation context
void handle_clear_session(ServerState& state, const json& cmd) {
    try {
        std::lock_guard<std::mutex> lock(state.state_mutex);
        state.client.reset_session();
        emit_event(json{{"type", "session_cleared"}});
    } catch (const std::exception& e) {
        log_error(std::string("clear_session handler error: ") + e.what());
    }
}

// Handle "list_models" command: retrieve available models for current provider
void handle_list_models(ServerState& state, const json& cmd) {
    try {
        std::lock_guard<std::mutex> lock(state.state_mutex);
        if (state.provider == Provider::none) {
            emit_event(json{{"type", "error"}, {"message", "No provider configured."}});
            return;
        }
        if (state.api_key.empty()) {
            emit_event(json{{"type", "error"}, {"message", "No API key set."}});
            return;
        }

        auto result = state.client.list_models(state.provider, state.api_key);
        if (!result.ok) {
            emit_event(json{{"type", "error"}, {"message", result.message}});
        } else {
            emit_event(json{{"type", "models_listed"}, {"models", result.models}});
        }
    } catch (const std::exception& e) {
        log_error(std::string("list_models handler error: ") + e.what());
        emit_event(json{{"type", "error"}, {"message", "Model listing failed."}});
    }
}

// Parse and dispatch a command from stdin
void process_command(ServerState& state, const std::string& line) {
    if (line.empty()) return;

    try {
        auto cmd = json::parse(line);
        auto cmd_type = cmd.value("type", "");

        if (cmd_type == "configure") {
            handle_configure(state, cmd);
        } else if (cmd_type == "prompt") {
            handle_prompt(state, cmd);
        } else if (cmd_type == "cancel") {
            handle_cancel(state, cmd);
        } else if (cmd_type == "clear_session") {
            handle_clear_session(state, cmd);
        } else if (cmd_type == "list_models") {
            handle_list_models(state, cmd);
        } else {
            emit_event(json{{"type", "error"}, {"message", "Unknown command type."}});
        }
    } catch (const json::parse_error& e) {
        log_error(std::string("JSON parse error: ") + e.what());
        emit_event(json{{"type", "error"}, {"message", "Invalid JSON."}});
    } catch (const std::exception& e) {
        log_error(std::string("Unexpected error: ") + e.what());
        emit_event(json{{"type", "error"}, {"message", "Internal error."}});
    }
}

} // namespace

int run_server() {
    emit_event(json{{"type", "ready"}});

    ServerState state;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (std::cin.eof()) break;
        process_command(state, line);
    }

    return 0;
}

} // namespace arn
