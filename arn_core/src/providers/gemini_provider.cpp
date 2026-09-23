#include "arn/core/provider/gemini_provider.hpp"
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

GeminiProvider::GeminiProvider(std::string endpoint)
    : endpoint_(std::move(endpoint)),
      client_(std::make_unique<httplib::Client>(endpoint_)) {
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(90, 0);
    client_->set_keep_alive(true);
}

GeminiProvider::~GeminiProvider() = default;

GeminiProvider::GeminiProvider(GeminiProvider&&) noexcept = default;
GeminiProvider& GeminiProvider::operator=(GeminiProvider&&) noexcept = default;

std::string GeminiProvider::preferred_model(const std::vector<std::string>& models) const {
    for (const auto candidate :
         {"gemini-flash-latest", "gemini-3.5-flash-lite", "gemini-3.5-flash",
          "gemini-3.1-flash-lite", "gemini-3.1-flash", "gemini-2.5-flash"}) {
        if (std::ranges::find(models, candidate) != models.end())
            return candidate;
    }
    for (const auto& candidate : models) {
        const auto model = detail::lower_ascii(candidate);
        if (model.find("gemini") != std::string::npos &&
            model.find("flash") != std::string::npos &&
            model.find("image") == std::string::npos &&
            model.find("tts") == std::string::npos &&
            model.find("transcribe") == std::string::npos)
            return candidate;
    }
    return models.empty() ? std::string{} : models.front();
}

ApiResult GeminiProvider::list_models(const std::string& api_key,
                                      const std::atomic_bool* cancel_requested) {
    if (!client_)
        return {false, "Client not initialized."};
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(30, 0);
    const auto response = net::execute_with_retry(
        [&] {
            return client_->Get("/v1beta/models?pageSize=1000", {{"x-goog-api-key", api_key}});
        },
        cancel_requested);
    if (!response)
        return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300)
        return detail::parse_error(response->status, "Gemini", response->body);
    try {
        auto models = net::parse_model_list(response->body, true);
        return {true, "Gemini key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

void GeminiProvider::trim_history(std::size_t max_entries) {
    while (contents_.size() > max_entries) {
        contents_.erase(contents_.begin());
    }
}

ModelTurn GeminiProvider::start_turn(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction, const std::string& user_prompt,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    contents_.push_back({{"role", "user"}, {"parts", {{{"text", user_prompt}}}}});
    return execute_turn_request(api_key, model, system_instruction, tools, callbacks, cancel_requested);
}

ModelTurn GeminiProvider::continue_turn(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction,
    const std::vector<ToolResponse>& tool_responses,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    nlohmann::json response_parts = nlohmann::json::array();
    for (const auto& resp : tool_responses) {
        response_parts.push_back({
            {"functionResponse", {
                {"name", resp.name},
                {"id", resp.call_id},
                {"response", resp.result}
            }}
        });
    }
    contents_.push_back({{"role", "user"}, {"parts", std::move(response_parts)}});
    return execute_turn_request(api_key, model, system_instruction, tools, callbacks, cancel_requested);
}

ModelTurn GeminiProvider::execute_turn_request(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (!client_)
        return {.ok = false, .error_message = "Client not initialized."};
    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    client_->set_read_timeout(90, 0);

    const auto payload = detail::build_gemini_payload(system_instruction, contents_, tools);

    std::string text;
    nlohmann::json function_calls = nlohmann::json::array();
    nlohmann::json model_response_parts = nlohmann::json::array();
    std::string error_body;
    bool received_event = false;

    const auto response = net::execute_stream_with_retry(
        [&] {
            return net::stream_post(
                *client_, "/v1beta/models/" + model + ":streamGenerateContent?alt=sse",
                {{"x-goog-api-key", api_key}}, payload.dump(),
                [&](std::string_view event) {
                    detail::parse_gemini_stream_chunk(event, text, model_response_parts,
                                                      function_calls, callbacks.on_text);
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
        const auto parsed_err = detail::parse_error(response->status, "Gemini", error_body);
        return {.ok = false, .error_message = parsed_err.message};
    }

    try {
        if (model_response_parts.empty())
            return {.ok = false, .error_message = "Gemini returned an empty streamed response."};

        contents_.push_back({{"role", "model"}, {"parts", model_response_parts}});

        std::vector<ToolCall> calls;
        for (const auto& fc : function_calls) {
            calls.push_back(ToolCall{
                .id = fc.value("id", ""),
                .name = fc.at("name").get<std::string>(),
                .arguments = fc.value("args", nlohmann::json::object())
            });
        }

        return {
            .ok = true,
            .text = std::move(text),
            .tool_calls = std::move(calls)
        };
    } catch (const std::exception& exception) {
        return {.ok = false, .error_message = "Could not read the API response: " + std::string(exception.what())};
    }
}

void GeminiProvider::cancel_active_request() {
    if (client_)
        client_->stop();
}

void GeminiProvider::reset_session() {
    contents_ = nlohmann::json::array();
}

std::size_t GeminiProvider::session_entries() const noexcept {
    return contents_.size();
}

} // namespace arn::core
