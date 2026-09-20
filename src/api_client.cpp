#include "api_client.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
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
        if (error->is_string()) return error->get<std::string>();
        if (error->is_object()) return error->value("message", error->dump());
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
    for (const auto name : required) names.push_back(name);
    // Gemini's function-declaration schema does not accept JSON Schema's
    // additionalProperties field. Keep the portable subset shared by Gemini
    // and DeepSeek; ToolExecutor still validates every argument locally.
    json result = {{"type", "object"}, {"properties", json::object()}};
    for (const auto& [name, schema] : properties) result["properties"][name] = schema;
    if (!names.empty()) result["required"] = std::move(names);
    return result;
}

json build_tool_definitions() {
    const json path = {{"type", "string"}, {"description", "A relative path inside the current project."}};
    const json text = {{"type", "string"}};
    return json::array({
        {{"name", "list_files"}, {"description", "List files and folders in the project. Never inspect .git or environment files."},
         {"parameters", tool_parameters({{"path", path}})}},
        {{"name", "read_file"}, {"description", "Read a small UTF-8 text file in the project before changing it."},
         {"parameters", tool_parameters({{"path", path}}, {"path"})}},
        {{"name", "write_file"}, {"description", "Create or replace a UTF-8 text file. The user will be asked before the change."},
         {"parameters", tool_parameters({{"path", path}, {"content", text}}, {"path", "content"})}},
        {{"name", "replace_text"}, {"description", "Replace one unique exact text selection in an existing file. The user will be asked before the change."},
         {"parameters", tool_parameters({{"path", path}, {"old_text", text}, {"new_text", text}}, {"path", "old_text", "new_text"})}},
        {{"name", "delete_file"}, {"description", "Permanently delete one regular file. Use only when the user explicitly asked to delete it; confirmation is required."},
         {"parameters", tool_parameters({{"path", path}}, {"path"})}},
    });
}

const json& tool_definitions() {
    static const json definitions = build_tool_definitions();
    return definitions;
}

std::string agent_instruction() {
    return "You are ARN, a coding assistant. You may work only through the declared local tools. "
           "The tools are restricted to the folder in which ARN was started. Inspect relevant files before editing. "
           "Never claim a file changed unless a tool result confirms it. Never request deletion unless the user explicitly asks. "
           "Do not try to access secrets, .env files, or .git. Explain concisely what you completed.";
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

bool should_retry(const httplib::Result& response) {
    if (!response) return true;
    const int status = response->status;
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

std::chrono::milliseconds retry_delay(const httplib::Result& response, int attempt) {
    if (response && response->has_header("Retry-After")) {
        try {
            const auto seconds = std::stoi(response->get_header_value("Retry-After"));
            if (seconds >= 0 && seconds <= 60) return std::chrono::seconds(seconds);
        } catch (const std::exception&) {
            // A date-form Retry-After header is uncommon for these APIs; use
            // exponential backoff when it cannot be parsed as seconds.
        }
    }

    const int base_ms = 500 * (1 << attempt);
    std::uniform_int_distribution<int> jitter(0, 250);
    static thread_local std::mt19937 generator(std::random_device{}());
    return std::chrono::milliseconds(base_ms + jitter(generator));
}

template <typename RequestFn>
auto execute_with_retry(RequestFn&& request) {
    for (int attempt = 0;; ++attempt) {
        auto response = request();
        if (!should_retry(response) || attempt + 1 >= max_request_attempts) return response;
        std::this_thread::sleep_for(retry_delay(response, attempt));
    }
}

template <typename RequestFn>
auto execute_stream_with_retry(RequestFn&& request, const bool& received_event,
                               const std::atomic_bool* cancel_requested) {
    for (int attempt = 0;; ++attempt) {
        auto response = request();
        // Never replay a partially printed answer: retrying then would show
        // duplicated text to the person using ARN.
        if ((cancel_requested && cancel_requested->load(std::memory_order_relaxed)) || received_event ||
            !should_retry(response) || attempt + 1 >= max_request_attempts) return response;
        std::this_thread::sleep_for(retry_delay(response, attempt));
    }
}

class SseDecoder {
public:
    template <typename EventFn>
    void push(std::string_view bytes, EventFn&& on_event) {
        // Accept both SSE line endings. JSON carriage returns are escaped, so
        // stripping transport-level '\r' here cannot alter event data.
        for (const char byte : bytes) {
            if (byte != '\r') pending_.push_back(byte);
        }
        for (;;) {
            const auto end = pending_.find("\n\n");
            if (end == std::string::npos) break;
            std::string event = pending_.substr(0, end);
            pending_.erase(0, end + 2);

            std::string data;
            std::size_t start = 0;
            while (start <= event.size()) {
                const auto line_end = event.find('\n', start);
                std::string_view line(event.data() + start,
                                      (line_end == std::string::npos ? event.size() : line_end) - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (line.starts_with("data:")) {
                    line.remove_prefix(5);
                    if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
                    if (!data.empty()) data.push_back('\n');
                    data.append(line);
                }
                if (line_end == std::string::npos) break;
                start = line_end + 1;
            }
            if (!data.empty()) on_event(data);
        }
    }

    template <typename EventFn>
    void finish(EventFn&& on_event) {
        if (pending_.empty()) return;

        // Some HTTP servers omit the final blank SSE line. Treat the tail as
        // one final event instead of silently dropping the model response.
        if (pending_.starts_with("data:")) {
            push("\n\n", std::forward<EventFn>(on_event));
            return;
        }
        std::string tail = std::move(pending_);
        pending_.clear();
        on_event(tail);
    }

private:
    std::string pending_;
};

httplib::Result stream_post(httplib::Client& client, const std::string& path, const httplib::Headers& headers,
                            const std::string& body, const std::function<void(std::string_view)>& on_event,
                            std::string& error_body, bool& received_event,
                            const std::atomic_bool* cancel_requested) {
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = headers;
    request.headers.emplace("Accept", "text/event-stream");
    request.headers.emplace("Content-Type", "application/json");
    request.body = body;

    int status = 0;
    SseDecoder decoder;
    request.response_handler = [&status](const httplib::Response& response) {
        status = response.status;
        return true;
    };
    request.progress = [cancel_requested](std::uint64_t, std::uint64_t) {
        return !cancel_requested || !cancel_requested->load(std::memory_order_relaxed);
    };
    request.content_receiver = [&](const char* data, std::size_t size, std::uint64_t, std::uint64_t) {
        if (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) return false;
        if (status < 200 || status >= 300) {
            error_body.append(data, size);
            return true;
        }
        decoder.push(std::string_view(data, size), [&](std::string_view event) {
            received_event = true;
            on_event(event);
        });
        return true;
    };
    auto response = client.send(request);
    if (response && response->status >= 200 && response->status < 300) {
        decoder.finish([&](std::string_view event) {
            received_event = true;
            on_event(event);
        });
    }
    return response;
}

ApiResult deepseek_models(const std::string& api_key) {
    httplib::Client client("https://api.deepseek.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(30, 0);
    const auto response = execute_with_retry([&] {
        return client.Get("/models", {{"Authorization", "Bearer " + api_key}});
    });
    if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300) return parse_error(response->status, "DeepSeek", response->body);
    try {
        std::vector<std::string> models;
        for (const auto& model : json::parse(response->body).at("data")) models.push_back(model.at("id").get<std::string>());
        std::ranges::sort(models);
        return {true, "DeepSeek key verified.", std::move(models)};
    } catch (const std::exception& exception) { return {false, "Could not read the model list: " + std::string(exception.what())}; }
}

ApiResult gemini_models(const std::string& api_key) {
    httplib::Client client("https://generativelanguage.googleapis.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(30, 0);
    const auto response = execute_with_retry([&] {
        return client.Get("/v1beta/models", {{"x-goog-api-key", api_key}});
    });
    if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300) return parse_error(response->status, "Gemini", response->body);
    try {
        std::vector<std::string> models;
        for (const auto& model : json::parse(response->body).at("models")) {
            const auto methods = model.value("supportedGenerationMethods", std::vector<std::string>{});
            if (std::ranges::find(methods, "generateContent") != methods.end()) {
                auto name = model.at("name").get<std::string>();
                constexpr std::string_view prefix = "models/";
                if (name.starts_with(prefix)) name.erase(0, prefix.size());
                models.push_back(std::move(name));
            }
        }
        std::ranges::sort(models);
        return {true, "Gemini key verified.", std::move(models)};
    } catch (const std::exception& exception) { return {false, "Could not read the model list: " + std::string(exception.what())}; }
}

ApiResult deepseek_prompt(const std::string& api_key, const std::string& model, const std::string& prompt,
                          const ToolExecutor& tools, const ToolExecutor::ConfirmationFn& confirm,
                          const ApiClient::StreamCallback& on_text, const std::atomic_bool* cancel_requested,
                          json& messages, httplib::Client& client) {
    if (messages.empty()) messages.push_back({{"role", "system"}, {"content", agent_instruction()}});
    messages.push_back({{"role", "user"}, {"content", prompt}});
    trim_history(messages, 1); // preserve the system instruction
    for (int round = 0; round < max_tool_rounds; ++round) {
        static const json api_tools = [] {
            json result = json::array();
            for (const auto& definition : tool_definitions()) result.push_back({{"type", "function"}, {"function", definition}});
            return result;
        }();
        const json payload = {{"model", model}, {"messages", messages}, {"tools", api_tools},
                              {"tool_choice", "auto"}, {"max_tokens", 2048}, {"stream", true}};
        std::string text;
        json calls = json::array();
        std::string error_body;
        bool received_event = false;
        const auto response = execute_stream_with_retry([&] {
            return stream_post(client, "/chat/completions", {{"Authorization", "Bearer " + api_key}},
                               payload.dump(), [&](std::string_view event) {
                if (event == "[DONE]") return;
                try {
                    const auto parsed = json::parse(event);
                    const auto packets = parsed.is_array() ? parsed : json::array({parsed});
                    for (const auto& packet : packets) {
                        const auto& delta = packet.at("choices").at(0).at("delta");
                        if (delta.contains("content") && !delta.at("content").is_null()) {
                            const auto chunk = delta.at("content").get<std::string>();
                            text += chunk;
                            if (on_text) on_text(chunk);
                        }
                        for (const auto& change : delta.value("tool_calls", json::array())) {
                            const auto index = change.value("index", 0U);
                            while (calls.size() <= index) {
                                calls.push_back({{"id", ""}, {"type", "function"},
                                                 {"function", {{"name", ""}, {"arguments", ""}}}});
                            }
                            auto& call = calls.at(index);
                            if (change.contains("id")) call["id"] = change.at("id");
                            if (change.contains("type")) call["type"] = change.at("type");
                            if (change.contains("function")) {
                                const auto& function = change.at("function");
                                if (function.contains("name")) call["function"]["name"] = function.at("name");
                                if (function.contains("arguments")) {
                                    call["function"]["arguments"] = call["function"]["arguments"].get<std::string>() +
                                        function.at("arguments").get<std::string>();
                                }
                            }
                        }
                    }
                } catch (const std::exception&) {
                    // Ignore malformed transient SSE data. A missing final
                    // response is still caught below instead of executing a tool.
                }
            }, error_body, received_event, cancel_requested);
        }, received_event, cancel_requested);
        if (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) {
            return {false, "Request cancelled.", {}, true};
        }
        if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
        if (response->status < 200 || response->status >= 300) return parse_error(response->status, "DeepSeek", error_body);
        try {
            json message = {{"role", "assistant"}, {"content", text.empty() ? json(nullptr) : json(text)}};
            if (!calls.empty()) message["tool_calls"] = calls;
            messages.push_back(message);
            if (calls.empty()) return {true, message.value("content", "")};
            for (const auto& call : calls) {
                json arguments;
                try { arguments = json::parse(call.at("function").at("arguments").get<std::string>()); }
                catch (const std::exception&) { arguments = json::object(); }
                const auto execution = execute_tool(tools, confirm, call.at("function").at("name").get<std::string>(), arguments);
                messages.push_back({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", execution.result.dump()}});
            }
        } catch (const std::exception& exception) { return {false, "Could not read the API response: " + std::string(exception.what())}; }
    }
    return {false, "Stopped after too many tool calls."};
}

ApiResult gemini_prompt(const std::string& api_key, const std::string& model, const std::string& prompt,
                        const ToolExecutor& tools, const ToolExecutor::ConfirmationFn& confirm,
                        const ApiClient::StreamCallback& on_text, const std::atomic_bool* cancel_requested,
                        json& contents, httplib::Client& client) {
    contents.push_back({{"role", "user"}, {"parts", {{{"text", prompt}}}}});
    trim_history(contents, 0);
    static const json tool_config = json::array({{{"functionDeclarations", tool_definitions()}}});
    for (int round = 0; round < max_tool_rounds; ++round) {
        const json payload = {{"systemInstruction", {{"parts", {{{"text", agent_instruction()}}}}}},
                              {"contents", contents}, {"tools", tool_config}};
        std::string text;
        json function_calls = json::array();
        // Gemini 3 can attach thoughtSignature to the Part containing a
        // function call. Preserve every raw Part for the next API turn.
        json model_response_parts = json::array();
        std::string error_body;
        bool received_event = false;
        const auto response = execute_stream_with_retry([&] {
            return stream_post(client, "/v1beta/models/" + model + ":streamGenerateContent?alt=sse",
                               {{"x-goog-api-key", api_key}}, payload.dump(), [&](std::string_view event) {
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
                                if (on_text) on_text(chunk);
                            }
                            if (part.contains("functionCall")) function_calls.push_back(part.at("functionCall"));
                        }
                    }
                } catch (const std::exception&) {
                    // The final response validation below prevents a malformed
                    // event from being treated as a successful tool call.
                }
            }, error_body, received_event, cancel_requested);
        }, received_event, cancel_requested);
        if (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) {
            return {false, "Request cancelled.", {}, true};
        }
        if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
        if (response->status < 200 || response->status >= 300) return parse_error(response->status, "Gemini", error_body);
        try {
            if (model_response_parts.empty()) return {false, "Gemini returned an empty streamed response."};
            contents.push_back({{"role", "model"}, {"parts", model_response_parts}});
            json response_parts = json::array();
            for (const auto& call : function_calls) {
                const auto execution = execute_tool(tools, confirm, call.at("name").get<std::string>(), call.value("args", json::object()));
                response_parts.push_back({{"functionResponse", {{"name", call.at("name")},
                    {"id", call.value("id", "")}, {"response", execution.result}}}});
            }
            if (function_calls.empty()) return {true, text};
            contents.push_back({{"role", "user"}, {"parts", std::move(response_parts)}});
        } catch (const std::exception& exception) { return {false, "Could not read the API response: " + std::string(exception.what())}; }
    }
    return {false, "Stopped after too many tool calls."};
}

} // namespace

std::string provider_name(Provider provider) {
    switch (provider) {
    case Provider::deepseek: return "DeepSeek";
    case Provider::gemini: return "Gemini";
    case Provider::none: return "none";
    }
    return "unknown";
}

ApiClient::~ApiClient() = default;

httplib::Client& ApiClient::client_for(Provider provider) {
    auto& client = provider == Provider::deepseek ? deepseek_client_ : gemini_client_;
    if (!client) {
        client = std::make_unique<httplib::Client>(
            provider == Provider::deepseek ? "https://api.deepseek.com" : "https://generativelanguage.googleapis.com");
        client->set_connection_timeout(10, 0);
        client->set_read_timeout(90, 0);
        client->set_keep_alive(true);
    }
    return *client;
}

ApiResult ApiClient::list_models(Provider provider, const std::string& api_key) const {
    if (api_key.empty()) return {false, "The API key cannot be empty."};
    if (provider == Provider::deepseek) return deepseek_models(api_key);
    if (provider == Provider::gemini) return gemini_models(api_key);
    return {false, "Select a provider first."};
}

ApiResult ApiClient::submit_prompt(Provider provider, const std::string& api_key, const std::string& model,
                                   const std::string& prompt, const ToolExecutor& tools,
                                   const ToolExecutor::ConfirmationFn& confirm,
                                   const StreamCallback& on_text,
                                   const std::atomic_bool* cancel_requested) {
    if (api_key.empty()) return {false, "No API key is set."};
    if (model.empty()) return {false, "No model is selected. Use /model <name>."};
    if (provider == Provider::none) return {false, "Select a provider first."};
    if (provider != session_provider_ || model != session_model_) {
        reset_session();
        session_provider_ = provider;
        session_model_ = model;
    }
    auto& http_client = client_for(provider);
    {
        std::lock_guard lock(active_request_mutex_);
        active_request_client_ = &http_client;
    }

    ApiResult result;
    if (provider == Provider::deepseek) {
        result = deepseek_prompt(api_key, model, prompt, tools, confirm, on_text, cancel_requested,
                                 deepseek_messages_, http_client);
    } else if (provider == Provider::gemini) {
        result = gemini_prompt(api_key, model, prompt, tools, confirm, on_text, cancel_requested,
                               gemini_contents_, http_client);
    } else {
        result = {false, "Select a provider first."};
    }
    {
        std::lock_guard lock(active_request_mutex_);
        active_request_client_ = nullptr;
    }
    return result;
}

void ApiClient::cancel_active_request() {
    std::lock_guard lock(active_request_mutex_);
    if (active_request_client_) active_request_client_->stop();
}

void ApiClient::reset_session() {
    session_provider_ = Provider::none;
    session_model_.clear();
    deepseek_messages_ = json::array();
    gemini_contents_ = json::array();
}

std::size_t ApiClient::session_entries() const noexcept {
    return session_provider_ == Provider::deepseek ? deepseek_messages_.size() : gemini_contents_.size();
}

} // namespace arn
