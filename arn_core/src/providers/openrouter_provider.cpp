#include "arn/core/provider/openrouter_provider.hpp"
#include "arn/core/net/http_client.hpp"
#include "arn/core/net/sse_decoder.hpp"
#include "provider_utils.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <ranges>
#include <utility>

namespace arn::core {

OpenRouterProvider::OpenRouterProvider(OpenRouterConfig config)
    : config_(std::move(config)) {
    init_client();
}

OpenRouterProvider::OpenRouterProvider(std::string endpoint, std::string api_path_prefix)
    : config_{.endpoint = std::move(endpoint), .api_path_prefix = std::move(api_path_prefix)} {
    init_client();
}

OpenRouterProvider::~OpenRouterProvider() = default;

OpenRouterProvider::OpenRouterProvider(OpenRouterProvider&&) noexcept = default;
OpenRouterProvider& OpenRouterProvider::operator=(OpenRouterProvider&&) noexcept = default;

void OpenRouterProvider::init_client() {
    client_ = std::make_unique<httplib::Client>(config_.endpoint);
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(90, 0);
    client_->set_keep_alive(true);
}

void OpenRouterProvider::set_config(OpenRouterConfig config) {
    config_ = std::move(config);
    init_client();
}

std::string OpenRouterProvider::preferred_model(const std::vector<std::string>& models) const {
    static constexpr std::array preferred_candidates = {
        "anthropic/claude-3.7-sonnet",
        "anthropic/claude-3.5-sonnet",
        "openai/gpt-4o",
        "deepseek/deepseek-chat",
        "openai/gpt-4o-mini",
        "google/gemini-2.0-flash-001",
        "meta-llama/llama-3.3-70b-instruct",
        "meta-llama/llama-3.1-70b-instruct"
    };
    for (const auto candidate : preferred_candidates) {
        if (std::ranges::find(models, candidate) != models.end())
            return candidate;
    }
    for (const auto candidate : {"claude-3.5-sonnet", "gpt-4o", "deepseek-chat"}) {
        for (const auto& model : models) {
            if (model.find(candidate) != std::string::npos)
                return model;
        }
    }
    return models.empty() ? std::string{} : models.front();
}

ModelCapabilities OpenRouterProvider::model_capabilities(std::string_view model) const {
    const auto it = model_capabilities_.find(model);
    if (it != model_capabilities_.end())
        return it->second;
    return ModelCapabilities{};
}

ApiResult OpenRouterProvider::list_models(const std::string& api_key,
                                          const std::atomic_bool* cancel_requested) {
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    if (!client_)
        return {false, "Client not initialized."};
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(30, 0);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key}
    };
    if (!config_.http_referer.empty()) {
        headers.emplace("HTTP-Referer", config_.http_referer);
    }
    if (!config_.app_title.empty()) {
        headers.emplace("X-Title", config_.app_title);
    }

    const std::string models_path = detail::join_api_path(config_.api_path_prefix, "/models");
    const auto response = net::execute_with_retry(
        [&] { return client_->Get(models_path.c_str(), headers); },
        cancel_requested);
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    if (!response)
        return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300)
        return detail::parse_error(response->status, "OpenRouter", response->body);
    try {
        auto entries = detail::parse_openrouter_model_catalog(response->body);
        std::vector<std::string> models;
        models.reserve(entries.size());
        model_capabilities_.clear();
        for (auto& entry : entries) {
            model_capabilities_.emplace(entry.id, entry.capabilities);
            models.push_back(std::move(entry.id));
        }
        return {true, "OpenRouter key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

void OpenRouterProvider::trim_history(std::size_t max_entries) {
    const std::size_t keep_from = (!messages_.empty() && messages_.front().value("role", "") == "system") ? 1 : 0;
    while (messages_.size() > max_entries) {
        messages_.erase(messages_.begin() + static_cast<nlohmann::json::difference_type>(keep_from));
    }
}

ModelTurn OpenRouterProvider::start_turn(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction, const std::string& user_prompt,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    const auto caps = model_capabilities(model);
    if (messages_.empty() && !system_instruction.empty()) {
        if (caps.supports_system_instruction) {
            messages_.push_back({{"role", "system"}, {"content", system_instruction}});
        }
    }
    std::string prompt_text = user_prompt;
    if (!caps.supports_system_instruction && !system_instruction.empty() && messages_.empty()) {
        prompt_text = system_instruction + "\n\n" + user_prompt;
    }
    messages_.push_back({{"role", "user"}, {"content", std::move(prompt_text)}});
    return execute_turn_request(api_key, model, tools, callbacks, cancel_requested);
}

ModelTurn OpenRouterProvider::continue_turn(
    const std::string& api_key, const std::string& model,
    const std::string& /*system_instruction*/,
    const std::vector<ToolResponse>& tool_responses,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    for (const auto& resp : tool_responses) {
        messages_.push_back({
            {"role", "tool"},
            {"tool_call_id", resp.call_id},
            {"content", resp.result.dump()}
        });
    }
    return execute_turn_request(api_key, model, tools, callbacks, cancel_requested);
}

ModelTurn OpenRouterProvider::execute_turn_request(
    const std::string& api_key, const std::string& model,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (!client_)
        return {.ok = false, .error_message = "Client not initialized."};
    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    client_->set_read_timeout(90, 0);

    const auto caps = model_capabilities(model);
    const auto payload = detail::build_openrouter_payload(model, messages_, tools, caps.supports_tools);

    std::string text;
    nlohmann::json calls = nlohmann::json::array();
    std::string error_body;
    bool received_event = false;

    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key}
    };
    if (!config_.http_referer.empty()) {
        headers.emplace("HTTP-Referer", config_.http_referer);
    }
    if (!config_.app_title.empty()) {
        headers.emplace("X-Title", config_.app_title);
    }

    const std::string chat_path = detail::join_api_path(config_.api_path_prefix, "/chat/completions");

    const auto response = net::execute_stream_with_retry(
        [&] {
            return net::stream_post(
                *client_, chat_path.c_str(), headers,
                payload.dump(),
                [&](std::string_view event) {
                    detail::parse_openrouter_stream_chunk(event, text, calls, callbacks.on_text);
                },
                error_body, received_event, cancel_requested);
        },
        received_event, cancel_requested, callbacks.on_progress);

    if (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) {
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};
    }
    if (!response) {
        return {.ok = false, .error_message = "Network request failed: " + httplib::to_string(response.error())};
    }
    if (response->status < 200 || response->status >= 300) {
        const auto parsed_err = detail::parse_error(response->status, "OpenRouter", error_body);
        return {.ok = false, .error_message = parsed_err.message};
    }

    try {
        nlohmann::json message = {
            {"role", "assistant"},
            {"content", text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text)}
        };
        if (!calls.empty())
            message["tool_calls"] = calls;
        messages_.push_back(message);

        std::vector<ToolCall> tool_calls;
        for (const auto& call : calls) {
            nlohmann::json arguments;
            try {
                arguments = nlohmann::json::parse(call.at("function").at("arguments").get<std::string>());
            } catch (const std::exception&) {
                arguments = nlohmann::json::object();
            }
            tool_calls.push_back(ToolCall{
                .id = call.value("id", ""),
                .name = call.at("function").at("name").get<std::string>(),
                .arguments = std::move(arguments)
            });
        }

        return {
            .ok = true,
            .text = std::move(text),
            .tool_calls = std::move(tool_calls)
        };
    } catch (const std::exception& exception) {
        return {.ok = false, .error_message = "Could not read the API response: " + std::string(exception.what())};
    }
}

void OpenRouterProvider::cancel_active_request() {
    if (client_)
        client_->stop();
}

void OpenRouterProvider::reset_session() {
    messages_ = nlohmann::json::array();
}

std::size_t OpenRouterProvider::session_entries() const noexcept {
    return messages_.size();
}

} // namespace arn::core
