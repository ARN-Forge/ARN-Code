#include "arn/core/provider/deepseek_provider.hpp"
#include "arn/core/net/http_client.hpp"
#include "arn/core/net/model_parser.hpp"
#include "arn/core/net/sse_decoder.hpp"
#include "provider_utils.hpp"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <ranges>
#include <utility>

namespace arn::core {

DeepSeekProvider::DeepSeekProvider(std::string endpoint)
    : endpoint_(std::move(endpoint)),
      client_(std::make_unique<httplib::Client>(endpoint_)) {
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(90, 0);
    client_->set_keep_alive(true);
}

DeepSeekProvider::~DeepSeekProvider() = default;

DeepSeekProvider::DeepSeekProvider(DeepSeekProvider&&) noexcept = default;
DeepSeekProvider& DeepSeekProvider::operator=(DeepSeekProvider&&) noexcept = default;

std::string DeepSeekProvider::preferred_model(const std::vector<std::string>& models) const {
    for (const auto candidate : {"deepseek-chat", "deepseek-reasoner"}) {
        if (std::ranges::find(models, candidate) != models.end())
            return candidate;
    }
    return models.empty() ? std::string{} : models.front();
}

ApiResult DeepSeekProvider::list_models(const std::string& api_key,
                                        const std::atomic_bool* cancel_requested) {
    if (!client_)
        return {false, "Client not initialized."};
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(30, 0);
    const auto response = net::execute_with_retry(
        [&] { return client_->Get("/models", {{"Authorization", "Bearer " + api_key}}); },
        cancel_requested);
    if (!response)
        return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300)
        return detail::parse_error(response->status, "DeepSeek", response->body);
    try {
        auto models = net::parse_model_list(response->body, false);
        return {true, "DeepSeek key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

void DeepSeekProvider::trim_history(std::size_t max_entries) {
    const std::size_t keep_from = (!messages_.empty() && messages_.front().value("role", "") == "system") ? 1 : 0;
    while (messages_.size() > max_entries) {
        messages_.erase(messages_.begin() + static_cast<nlohmann::json::difference_type>(keep_from));
    }
}

ModelTurn DeepSeekProvider::start_turn(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction, const std::string& user_prompt,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (messages_.empty() && !system_instruction.empty()) {
        messages_.push_back({{"role", "system"}, {"content", system_instruction}});
    }
    messages_.push_back({{"role", "user"}, {"content", user_prompt}});
    return execute_turn_request(api_key, model, tools, callbacks, cancel_requested);
}

ModelTurn DeepSeekProvider::continue_turn(
    const std::string& api_key, const std::string& model,
    const std::string& /*system_instruction*/,
    const std::vector<ToolResponse>& tool_responses,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    for (const auto& resp : tool_responses) {
        messages_.push_back({
            {"role", "tool"},
            {"tool_call_id", resp.call_id},
            {"content", resp.result.dump()}
        });
    }
    return execute_turn_request(api_key, model, tools, callbacks, cancel_requested);
}

ModelTurn DeepSeekProvider::execute_turn_request(
    const std::string& api_key, const std::string& model,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (!client_)
        return {.ok = false, .error_message = "Client not initialized."};
    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    client_->set_read_timeout(90, 0);

    const auto payload = detail::build_deepseek_payload(model, messages_, tools);

    std::string text;
    nlohmann::json calls = nlohmann::json::array();
    std::string error_body;
    bool received_event = false;

    const auto response = net::execute_stream_with_retry(
        [&] {
            return net::stream_post(
                *client_, "/chat/completions", {{"Authorization", "Bearer " + api_key}},
                payload.dump(),
                [&](std::string_view event) {
                    detail::parse_deepseek_stream_chunk(event, text, calls, callbacks.on_text);
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
        const auto parsed_err = detail::parse_error(response->status, "DeepSeek", error_body);
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

void DeepSeekProvider::cancel_active_request() {
    if (client_)
        client_->stop();
}

void DeepSeekProvider::reset_session() {
    messages_ = nlohmann::json::array();
}

std::size_t DeepSeekProvider::session_entries() const noexcept {
    return messages_.size();
}

} // namespace arn::core
