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

ApiResult GeminiProvider::submit_prompt(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction, const std::string& user_prompt,
    const ToolRegistry& tools, const ConfirmationFn& confirm,
    const TextStreamCallback& on_text,
    const std::atomic_bool* cancel_requested,
    const ProgressCallback& on_progress) {

    if (!client_)
        return {false, "Client not initialized."};
    client_->set_read_timeout(90, 0);
    contents_.push_back({{"role", "user"}, {"parts", {{{"text", user_prompt}}}}});
    detail::trim_history(contents_, 0);

    for (int round = 0; round < detail::max_tool_rounds; ++round) {
        if (cancel_requested && cancel_requested->load())
            return {false, "Request cancelled.", {}, true};

        const auto payload = detail::build_gemini_payload(system_instruction, contents_, tools);

        std::string text;
        nlohmann::json function_calls = nlohmann::json::array();
        // Gemini 3 can attach thoughtSignature to the Part containing a
        // function call. Preserve every raw Part for the next API turn.
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
                                                          function_calls, on_text);
                    },
                    error_body, received_event, cancel_requested);
            },
            received_event, cancel_requested, on_progress);

        if (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) {
            return {false, "Request cancelled.", {}, true};
        }
        if (!response)
            return {false, "Network request failed: " + httplib::to_string(response.error())};
        if (response->status < 200 || response->status >= 300)
            return detail::parse_error(response->status, "Gemini", error_body);

        try {
            if (model_response_parts.empty())
                return {false, "Gemini returned an empty streamed response."};

            contents_.push_back({{"role", "model"}, {"parts", model_response_parts}});

            nlohmann::json response_parts = nlohmann::json::array();
            for (const auto& call : function_calls) {
                if (cancel_requested && cancel_requested->load())
                    return {false, "Request cancelled.", {}, true};
                const auto tool_name = call.at("name").get<std::string>();
                if (on_progress)
                    on_progress("Running project tool: " + tool_name);

                const ToolContext context{
                    .cancel_requested = cancel_requested,
                    .confirm = confirm
                };
                const auto execution = tools.execute(tool_name, call.value("args", nlohmann::json::object()), context);
                response_parts.push_back({{"functionResponse",
                                           {{"name", tool_name},
                                            {"id", call.value("id", "")},
                                            {"response", execution.result}}}});
            }

            if (function_calls.empty())
                return {true, text};

            contents_.push_back({{"role", "user"}, {"parts", std::move(response_parts)}});
        } catch (const std::exception& exception) {
            return {false, "Could not read the API response: " + std::string(exception.what())};
        }
    }

    return {false, "Stopped after too many tool calls."};
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
