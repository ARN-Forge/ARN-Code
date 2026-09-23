#include "api_client.hpp"
#include "model_list.hpp"

#include <arn/core/net/http_client.hpp>
#include <arn/core/net/model_parser.hpp>
#include <arn/core/net/sse_decoder.hpp>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace arn {
namespace {

using json = nlohmann::json;
constexpr int max_tool_rounds = 12;
constexpr std::size_t max_history_entries = 40;
constexpr int max_request_attempts = 3;

std::string error_message(const json& body) {
    if (const auto error = body.find("error"); error != body.end()) {
        if (error->is_string())
            return error->get<std::string>();
        if (error->is_object())
            return error->value("message", error->dump());
    }
    return body.value("message", "Unknown API error");
}

ApiResult parse_error(int status, const std::string& provider, const std::string& response_body) {
    try {
        return {false, provider + " returned HTTP " + std::to_string(status) + ": " +
                           error_message(json::parse(response_body))};
    } catch (const std::exception&) {
        return {false, provider + " returned HTTP " + std::to_string(status)};
    }
}

json tool_parameters(std::initializer_list<std::pair<const char*, json>> properties,
                     std::initializer_list<const char*> required = {}) {
    json names = json::array();
    for (const auto name : required)
        names.push_back(name);
    // Gemini's function-declaration schema does not accept JSON Schema's
    // additionalProperties field. Keep the portable subset shared by Gemini
    // and DeepSeek; ToolExecutor still validates every argument locally.
    json result = {{"type", "object"}, {"properties", json::object()}};
    for (const auto& [name, schema] : properties)
        result["properties"][name] = schema;
    if (!names.empty())
        result["required"] = std::move(names);
    return result;
}

json build_tool_definitions() {
    const json path = {{"type", "string"},
                       {"description", "A relative path inside the current project."}};
    const json text = {{"type", "string"}};
    return json::array({
        {{"name", "list_files"},
         {"description",
          "List files and folders in the project. Never inspect .git or environment files."},
         {"parameters", tool_parameters({{"path", path}})}},
        {{"name", "read_file"},
         {"description", "Read a small UTF-8 text file in the project before changing it."},
         {"parameters", tool_parameters({{"path", path}}, {"path"})}},
        {{"name", "write_file"},
         {"description",
          "Create or replace a UTF-8 text file. The user will be asked before the change."},
         {"parameters", tool_parameters({{"path", path}, {"content", text}}, {"path", "content"})}},
        {{"name", "replace_text"},
         {"description", "Replace one unique exact text selection in an existing file. The user "
                         "will be asked before the change."},
         {"parameters", tool_parameters({{"path", path}, {"old_text", text}, {"new_text", text}},
                                        {"path", "old_text", "new_text"})}},
        {{"name", "delete_file"},
         {"description", "Permanently delete one regular file. Use only when the user explicitly "
                         "asked to delete it; confirmation is required."},
         {"parameters", tool_parameters({{"path", path}}, {"path"})}},
    });
}

const json& tool_definitions() {
    static const json definitions = build_tool_definitions();
    return definitions;
}

std::string agent_instruction() {
    return "You are ARN, a coding assistant. You may work only through the declared local tools. "
           "The tools are restricted to the folder in which ARN was started. Inspect relevant "
           "files before editing. "
           "Never claim a file changed unless a tool result confirms it. Never request deletion "
           "unless the user explicitly asks. "
           "Do not try to access secrets, .env files, or .git. Explain concisely what you "
           "completed.";
}

ToolExecution execute_tool(const ToolExecutor& tools, const ToolExecutor::ConfirmationFn& confirm,
                           const std::string& name, const json& arguments) {
    return tools.execute(name, arguments, confirm);
}

void trim_history(json& history, std::size_t keep_from) {
    while (history.size() > max_history_entries) {
        history.erase(history.begin() + static_cast<json::difference_type>(keep_from));
    }
}

using arn::core::net::should_retry;
using arn::core::net::retry_delay;
using arn::core::net::execute_with_retry;
using arn::core::net::execute_stream_with_retry;
using arn::core::net::SseDecoder;
using arn::core::net::stream_post;

ApiResult deepseek_models(const std::string& api_key, httplib::Client& client,
                          const std::atomic_bool* cancel_requested) {
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(30, 0);
    const auto response = execute_with_retry(
        [&] { return client.Get("/models", {{"Authorization", "Bearer " + api_key}}); },
        cancel_requested);
    if (!response)
        return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300)
        return parse_error(response->status, "DeepSeek", response->body);
    try {
        auto models = detail::parse_model_list(response->body, false);
        return {true, "DeepSeek key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

ApiResult gemini_models(const std::string& api_key, httplib::Client& client,
                        const std::atomic_bool* cancel_requested) {
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(30, 0);
    const auto response = execute_with_retry(
        [&] {
            // Gemini returns only 50 models by default. The catalogue can now be
            // larger than that, and the first page is not guaranteed to contain a
            // text-generation model. Request the documented maximum page size.
            return client.Get("/v1beta/models?pageSize=1000", {{"x-goog-api-key", api_key}});
        },
        cancel_requested);
    if (!response)
        return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300)
        return parse_error(response->status, "Gemini", response->body);
    try {
        auto models = detail::parse_model_list(response->body, true);
        return {true, "Gemini key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

ApiResult deepseek_prompt(const std::string& api_key, const std::string& model,
                          const std::string& prompt, const ToolExecutor& tools,
                          const ToolExecutor::ConfirmationFn& confirm,
                          const ApiClient::StreamCallback& on_text,
                          const std::atomic_bool* cancel_requested, json& messages,
                          httplib::Client& client, const ApiClient::StreamCallback& on_progress) {
    if (messages.empty())
        messages.push_back({{"role", "system"}, {"content", agent_instruction()}});
    messages.push_back({{"role", "user"}, {"content", prompt}});
    trim_history(messages, 1); // preserve the system instruction
    for (int round = 0; round < max_tool_rounds; ++round) {
        if (cancel_requested && cancel_requested->load())
            return {false, "Request cancelled.", {}, true};
        static const json api_tools = [] {
            json result = json::array();
            for (const auto& definition : tool_definitions())
                result.push_back({{"type", "function"}, {"function", definition}});
            return result;
        }();
        const json payload = {{"model", model},        {"messages", messages}, {"tools", api_tools},
                              {"tool_choice", "auto"}, {"max_tokens", 2048},   {"stream", true}};
        std::string text;
        json calls = json::array();
        std::string error_body;
        bool received_event = false;
        const auto response = execute_stream_with_retry(
            [&] {
                return stream_post(
                    client, "/chat/completions", {{"Authorization", "Bearer " + api_key}},
                    payload.dump(),
                    [&](std::string_view event) {
                        if (event == "[DONE]")
                            return;
                        try {
                            const auto parsed = json::parse(event);
                            const auto packets = parsed.is_array() ? parsed : json::array({parsed});
                            for (const auto& packet : packets) {
                                const auto& delta = packet.at("choices").at(0).at("delta");
                                if (delta.contains("content") && !delta.at("content").is_null()) {
                                    const auto chunk = delta.at("content").get<std::string>();
                                    text += chunk;
                                    if (on_text)
                                        on_text(chunk);
                                }
                                for (const auto& change :
                                     delta.value("tool_calls", json::array())) {
                                    const auto index = change.value("index", 0U);
                                    while (calls.size() <= index) {
                                        calls.push_back(
                                            {{"id", ""},
                                             {"type", "function"},
                                             {"function", {{"name", ""}, {"arguments", ""}}}});
                                    }
                                    auto& call = calls.at(index);
                                    if (change.contains("id"))
                                        call["id"] = change.at("id");
                                    if (change.contains("type"))
                                        call["type"] = change.at("type");
                                    if (change.contains("function")) {
                                        const auto& function = change.at("function");
                                        if (function.contains("name"))
                                            call["function"]["name"] = function.at("name");
                                        if (function.contains("arguments")) {
                                            call["function"]["arguments"] =
                                                call["function"]["arguments"].get<std::string>() +
                                                function.at("arguments").get<std::string>();
                                        }
                                    }
                                }
                            }
                        } catch (const std::exception&) {
                            // Ignore malformed transient SSE data. A missing final
                            // response is still caught below instead of executing a tool.
                        }
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
            return parse_error(response->status, "DeepSeek", error_body);
        try {
            json message = {{"role", "assistant"},
                            {"content", text.empty() ? json(nullptr) : json(text)}};
            if (!calls.empty())
                message["tool_calls"] = calls;
            messages.push_back(message);
            if (calls.empty())
                return {true, message.value("content", "")};
            for (const auto& call : calls) {
                json arguments;
                try {
                    arguments = json::parse(call.at("function").at("arguments").get<std::string>());
                } catch (const std::exception&) {
                    arguments = json::object();
                }
                if (cancel_requested && cancel_requested->load())
                    return {false, "Request cancelled.", {}, true};
                if (on_progress)
                    on_progress("Running project tool: " +
                                call.at("function").at("name").get<std::string>());
                const auto execution = execute_tool(
                    tools, confirm, call.at("function").at("name").get<std::string>(), arguments);
                messages.push_back({{"role", "tool"},
                                    {"tool_call_id", call.at("id")},
                                    {"content", execution.result.dump()}});
            }
        } catch (const std::exception& exception) {
            return {false, "Could not read the API response: " + std::string(exception.what())};
        }
    }
    return {false, "Stopped after too many tool calls."};
}

ApiResult gemini_prompt(const std::string& api_key, const std::string& model,
                        const std::string& prompt, const ToolExecutor& tools,
                        const ToolExecutor::ConfirmationFn& confirm,
                        const ApiClient::StreamCallback& on_text,
                        const std::atomic_bool* cancel_requested, json& contents,
                        httplib::Client& client, const ApiClient::StreamCallback& on_progress) {
    contents.push_back({{"role", "user"}, {"parts", {{{"text", prompt}}}}});
    trim_history(contents, 0);
    static const json tool_config = json::array({{{"functionDeclarations", tool_definitions()}}});
    for (int round = 0; round < max_tool_rounds; ++round) {
        if (cancel_requested && cancel_requested->load())
            return {false, "Request cancelled.", {}, true};
        const json payload = {{"systemInstruction", {{"parts", {{{"text", agent_instruction()}}}}}},
                              {"contents", contents},
                              {"tools", tool_config}};
        std::string text;
        json function_calls = json::array();
        // Gemini 3 can attach thoughtSignature to the Part containing a
        // function call. Preserve every raw Part for the next API turn.
        json model_response_parts = json::array();
        std::string error_body;
        bool received_event = false;
        const auto response = execute_stream_with_retry(
            [&] {
                return stream_post(
                    client, "/v1beta/models/" + model + ":streamGenerateContent?alt=sse",
                    {{"x-goog-api-key", api_key}}, payload.dump(),
                    [&](std::string_view event) {
                        try {
                            const auto parsed = json::parse(event);
                            const auto packets = parsed.is_array() ? parsed : json::array({parsed});
                            for (const auto& packet : packets) {
                                const auto& content = packet.at("candidates").at(0).at("content");
                                for (const auto& part : content.at("parts")) {
                                    model_response_parts.push_back(part);
                                    if (part.contains("text")) {
                                        const auto chunk = part.at("text").get<std::string>();
                                        text += chunk;
                                        if (on_text)
                                            on_text(chunk);
                                    }
                                    if (part.contains("functionCall"))
                                        function_calls.push_back(part.at("functionCall"));
                                }
                            }
                        } catch (const std::exception&) {
                            // The final response validation below prevents a malformed
                            // event from being treated as a successful tool call.
                        }
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
            return parse_error(response->status, "Gemini", error_body);
        try {
            if (model_response_parts.empty())
                return {false, "Gemini returned an empty streamed response."};
            contents.push_back({{"role", "model"}, {"parts", model_response_parts}});
            json response_parts = json::array();
            for (const auto& call : function_calls) {
                if (cancel_requested && cancel_requested->load())
                    return {false, "Request cancelled.", {}, true};
                if (on_progress)
                    on_progress("Running project tool: " + call.at("name").get<std::string>());
                const auto execution =
                    execute_tool(tools, confirm, call.at("name").get<std::string>(),
                                 call.value("args", json::object()));
                response_parts.push_back({{"functionResponse",
                                           {{"name", call.at("name")},
                                            {"id", call.value("id", "")},
                                            {"response", execution.result}}}});
            }
            if (function_calls.empty())
                return {true, text};
            contents.push_back({{"role", "user"}, {"parts", std::move(response_parts)}});
        } catch (const std::exception& exception) {
            return {false, "Could not read the API response: " + std::string(exception.what())};
        }
    }
    return {false, "Stopped after too many tool calls."};
}

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

class DeepSeekProvider final : public ModelProvider {
  public:
    DeepSeekProvider() : client_("https://api.deepseek.com") {
        client_.set_connection_timeout(10, 0);
        client_.set_read_timeout(90, 0);
        client_.set_keep_alive(true);
    }

    Provider kind() const noexcept override {
        return Provider::deepseek;
    }
    std::string_view name() const noexcept override {
        return "DeepSeek";
    }
    std::string preferred_model(const std::vector<std::string>& models) const override {
        for (const auto candidate : {"deepseek-chat", "deepseek-reasoner"}) {
            if (std::ranges::find(models, candidate) != models.end())
                return candidate;
        }
        return models.empty() ? std::string{} : models.front();
    }
    ApiResult list_models(const std::string& api_key,
                          const std::atomic_bool* cancel_requested) override {
        return deepseek_models(api_key, client_, cancel_requested);
    }
    ApiResult submit_prompt(const std::string& api_key, const std::string& model,
                            const std::string& prompt, const ToolExecutor& tools,
                            const ToolExecutor::ConfirmationFn& confirm,
                            const ProviderStreamCallback& on_text,
                            const std::atomic_bool* cancel_requested,
                            const ProviderStreamCallback& on_progress) override {
        client_.set_read_timeout(90, 0);
        return deepseek_prompt(api_key, model, prompt, tools, confirm, on_text, cancel_requested,
                               messages_, client_, on_progress);
    }
    void cancel_active_request() override {
        client_.stop();
    }
    void reset_session() override {
        messages_ = json::array();
    }
    std::size_t session_entries() const noexcept override {
        return messages_.size();
    }

  private:
    httplib::Client client_;
    json messages_ = json::array();
};

class GeminiProvider final : public ModelProvider {
  public:
    GeminiProvider() : client_("https://generativelanguage.googleapis.com") {
        client_.set_connection_timeout(10, 0);
        client_.set_read_timeout(90, 0);
        client_.set_keep_alive(true);
    }

    Provider kind() const noexcept override {
        return Provider::gemini;
    }
    std::string_view name() const noexcept override {
        return "Gemini";
    }
    std::string preferred_model(const std::vector<std::string>& models) const override {
        for (const auto candidate :
             {"gemini-flash-latest", "gemini-3.5-flash-lite", "gemini-3.5-flash",
              "gemini-3.1-flash-lite", "gemini-3.1-flash", "gemini-2.5-flash"}) {
            if (std::ranges::find(models, candidate) != models.end())
                return candidate;
        }
        for (const auto& candidate : models) {
            const auto model = lower_ascii(candidate);
            if (model.find("gemini") != std::string::npos &&
                model.find("flash") != std::string::npos &&
                model.find("image") == std::string::npos &&
                model.find("tts") == std::string::npos &&
                model.find("transcribe") == std::string::npos)
                return candidate;
        }
        return models.empty() ? std::string{} : models.front();
    }
    ApiResult list_models(const std::string& api_key,
                          const std::atomic_bool* cancel_requested) override {
        return gemini_models(api_key, client_, cancel_requested);
    }
    ApiResult submit_prompt(const std::string& api_key, const std::string& model,
                            const std::string& prompt, const ToolExecutor& tools,
                            const ToolExecutor::ConfirmationFn& confirm,
                            const ProviderStreamCallback& on_text,
                            const std::atomic_bool* cancel_requested,
                            const ProviderStreamCallback& on_progress) override {
        client_.set_read_timeout(90, 0);
        return gemini_prompt(api_key, model, prompt, tools, confirm, on_text, cancel_requested,
                             contents_, client_, on_progress);
    }
    void cancel_active_request() override {
        client_.stop();
    }
    void reset_session() override {
        contents_ = json::array();
    }
    std::size_t session_entries() const noexcept override {
        return contents_.size();
    }

  private:
    httplib::Client client_;
    json contents_ = json::array();
};

} // namespace

std::unique_ptr<ModelProvider> make_provider(Provider provider) {
    switch (provider) {
    case Provider::deepseek:
        return std::make_unique<DeepSeekProvider>();
    case Provider::gemini:
        return std::make_unique<GeminiProvider>();
    case Provider::none:
        return {};
    }
    return {};
}

ApiClient::~ApiClient() = default;

ModelProvider* ApiClient::provider_for(Provider provider) {
    if (provider == Provider::none)
        return nullptr;
    auto& implementation = providers_[provider];
    if (!implementation)
        implementation = make_provider(provider);
    return implementation.get();
}

ApiResult ApiClient::list_models(Provider provider, const std::string& api_key,
                                 const std::atomic_bool* cancel_requested) {
    if (api_key.empty())
        return {false, "The API key cannot be empty."};
    if (provider == Provider::none)
        return {false, "Select a provider first."};
    auto* implementation = provider_for(provider);
    if (!implementation)
        return {false, "Select a provider first."};
    {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = implementation;
    }
    struct Cleanup {
        std::function<void()> action;
        ~Cleanup() {
            action();
        }
    } cleanup{[&] {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = nullptr;
    }};
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    return implementation->list_models(api_key, cancel_requested);
}

ApiResult ApiClient::submit_prompt(Provider provider, const std::string& api_key,
                                   const std::string& model, const std::string& prompt,
                                   const ToolExecutor& tools,
                                   const ToolExecutor::ConfirmationFn& confirm,
                                   const StreamCallback& on_text,
                                   const std::atomic_bool* cancel_requested,
                                   const StreamCallback& on_progress) {
    if (api_key.empty())
        return {false, "No API key is set."};
    if (model.empty())
        return {false, "No model is selected. Use /model <name>."};
    if (provider == Provider::none)
        return {false, "Select a provider first."};
    if (provider != session_provider_ || model != session_model_) {
        reset_session();
        session_provider_ = provider;
        session_model_ = model;
    }
    auto* implementation = provider_for(provider);
    if (!implementation)
        return {false, "Select a provider first."};
    {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = implementation;
    }

    struct Cleanup {
        std::function<void()> action;
        ~Cleanup() {
            action();
        }
    } cleanup{[&] {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = nullptr;
    }};
    return implementation->submit_prompt(api_key, model, prompt, tools, confirm, on_text,
                                         cancel_requested, on_progress);
}

void ApiClient::cancel_active_request() {
    std::lock_guard lock(active_request_mutex_);
    if (active_request_provider_)
        active_request_provider_->cancel_active_request();
}

void ApiClient::reset_session() {
    session_provider_ = Provider::none;
    session_model_.clear();
    for (auto& [provider, implementation] : providers_) {
        (void)provider;
        implementation->reset_session();
    }
}

std::size_t ApiClient::session_entries() const noexcept {
    if (session_provider_ == Provider::none)
        return 0;
    const auto implementation = providers_.find(session_provider_);
    return implementation == providers_.end() ? 0 : implementation->second->session_entries();
}

std::string ApiClient::preferred_model(Provider provider, const std::vector<std::string>& models) {
    const auto* implementation = provider_for(provider);
    return implementation ? implementation->preferred_model(models) : std::string{};
}

} // namespace arn
