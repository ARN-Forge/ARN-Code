#include "server.hpp"
#include "api_client.hpp"
#include "confirmation_gate.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
namespace arn {
namespace {
using json = nlohmann::json;
std::mutex output_mutex;
void emit(json event) {
    std::lock_guard lock(output_mutex);
    std::cout << event.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
}
struct Server {
    ApiClient client;
    ToolExecutor tools; // Project root is fixed at process startup.
    Provider provider = Provider::none;
    std::string key, model;
    std::vector<std::string> models;
    std::atomic_bool busy{false}, cancel{false};
    std::thread worker;
    ConfirmationGate gate;
    std::string active_id; // stdin thread only
    ~Server() {
        stop();
        if (worker.joinable())
            worker.join();
    }
    void stop() {
        cancel = true;
        gate.cancel();
        client.cancel_active_request();
    }
    json execute(const json& cmd, const std::string& id) {
        const auto type = cmd.value("type", "");
        if (type == "configure") {
            provider = Provider::none;
            key.clear();
            model.clear();
            models.clear();
            client.reset_session();
            const auto name = cmd.value("provider", "");
            const auto next = provider_from_name(name);
            const auto secret = cmd.value("apiKey", "");
            if (next == Provider::none || secret.empty())
                return {{"type", "error"},
                        {"message", "Choose Gemini or DeepSeek and supply an API key."}};
            auto result = client.list_models(next, secret, &cancel);
            if (!result.ok)
                return {{"type", "error"},
                        {"message", "Provider access check failed. Check credentials, network and "
                                    "provider availability."}};
            if (cancel)
                return {{"type", "cancelled"}};
            if (result.models.empty())
                return {{"type", "error"}, {"message", "Provider returned no supported models."}};
            provider = next;
            key = secret;
            models = std::move(result.models);
            return {{"type", "models_listed"}, {"models", models}};
        }
        if (type == "list_models") {
            if (provider == Provider::none)
                return {{"type", "error"}, {"message", "Verify provider credentials first."}};
            return {{"type", "models_listed"}, {"models", models}};
        }
        if (type == "select_model") {
            const auto selected = cmd.value("model", "");
            if (std::find(models.begin(), models.end(), selected) == models.end())
                return {{"type", "error"},
                        {"message", "Select a model from the verified provider list."}};
            model = selected;
            client.reset_session();
            return {{"type", "configured"}, {"model", model}};
        }
        if (type == "clear_session") {
            client.reset_session();
            return {{"type", "session_cleared"}};
        }
        if (type != "prompt")
            return {{"type", "error"}, {"message", "Unknown command."}};
        if (provider == Provider::none || model.empty())
            return {{"type", "error"}, {"message", "Verify provider and select a model first."}};
        const auto text = cmd.value("text", "");
        if (text.empty())
            return {{"type", "error"}, {"message", "Prompt is empty."}};
        tools = ToolExecutor(tools.project_root(),
                             [&] { emit({{"type", "files_changed"}, {"requestId", id}}); });
        auto confirm = [&](const ToolRequest& request) {
            std::string change_id;
            const bool approved = gate.wait([&](const std::string& operation_id) {
                change_id = operation_id;
                emit({{"type", "confirmation_required"},
                      {"requestId", id},
                      {"id", operation_id},
                      {"operation", request.name},
                      {"path", request.arguments.value("path", "")},
                      {"summary", request.summary},
                      {"before", request.arguments.value("before", "")},
                      {"after", request.arguments.value("after", "")},
                      {"timeoutSeconds", 120}});
            });
            emit({{"type", "confirmation_resolved"},
                  {"requestId", id},
                  {"id", change_id},
                  {"approved", approved}});
            return approved;
        };
        const auto result = client.submit_prompt(
            provider, key, model, text, tools, confirm,
            [&](std::string_view chunk) {
                emit({{"type", "stream"}, {"requestId", id}, {"text", chunk}});
            },
            &cancel,
            [&](std::string_view message) {
                emit({{"type", "progress"}, {"requestId", id}, {"message", message}});
            });
        if (cancel || result.cancelled) {
            client.reset_session();
            return {{"type", "cancelled"}};
        }
        if (!result.ok) {
            client.reset_session();
            // Provider error bodies can echo credentials. Do not relay them to logs/UI.
            return {{"type", "error"},
                    {"message", "ARN request failed. Check provider access, model availability and "
                                "network; conversation was reset."}};
        }
        return {{"type", "complete"}};
    }
    void dispatch(const json& cmd) {
        const auto type = cmd.value("type", "");
        const auto id = cmd.value("id", "");
        if (type == "cancel") {
            if (busy && id == active_id)
                stop();
            return;
        }
        if (type == "confirmation_response") {
            gate.answer(id, cmd.value("approved", false));
            return;
        }
        if (id.empty()) {
            emit({{"type", "error"}, {"message", "Command requires a nonempty id."}});
            return;
        }
        if (busy.exchange(true)) {
            emit({{"type", "error"}, {"requestId", id}, {"message", "ARN is busy."}});
            return;
        }
        if (worker.joinable())
            worker.join();
        active_id = id;
        cancel = false;
        gate.reset();
        worker = std::thread([this, cmd, id] {
            json result;
            try {
                result = execute(cmd, id);
            } catch (...) {
                result = {{"type", "error"},
                          {"message", "Invalid command or ARN execution failure."}};
            }
            result["requestId"] = id;
            busy = false;
            emit(std::move(result));
        });
    }
};
} // namespace
int run_server() {
    Server server;
    emit({{"type", "ready"}, {"protocol", 2}});
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        try {
            server.dispatch(nlohmann::json::parse(line));
        } catch (...) {
            emit({{"type", "error"}, {"message", "Invalid JSON command."}});
        }
    }
    return 0; // Also processes the final line without newline. EOF cancels and joins.
}
} // namespace arn
