#include "api_client.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace arn {
namespace {

using json = nlohmann::json;
constexpr int max_tool_rounds = 12;
constexpr std::size_t max_history_entries = 40;

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

json tool_definitions() {
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

ApiResult deepseek_models(const std::string& api_key) {
    httplib::Client client("https://api.deepseek.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(30, 0);
    const auto response = client.Get("/models", {{"Authorization", "Bearer " + api_key}});
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
    const auto response = client.Get("/v1beta/models", {{"x-goog-api-key", api_key}});
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
                          json& messages) {
    httplib::Client client("https://api.deepseek.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(90, 0);
    if (messages.empty()) messages.push_back({{"role", "system"}, {"content", agent_instruction()}});
    messages.push_back({{"role", "user"}, {"content", prompt}});
    trim_history(messages, 1); // preserve the system instruction
    const auto definitions = tool_definitions();
    for (int round = 0; round < max_tool_rounds; ++round) {
        json api_tools = json::array();
        for (const auto& definition : definitions) api_tools.push_back({{"type", "function"}, {"function", definition}});
        const json payload = {{"model", model}, {"messages", messages}, {"tools", api_tools},
                              {"tool_choice", "auto"}, {"max_tokens", 2048}, {"stream", false}};
        const auto response = client.Post("/chat/completions", {{"Authorization", "Bearer " + api_key}}, payload.dump(), "application/json");
        if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
        if (response->status < 200 || response->status >= 300) return parse_error(response->status, "DeepSeek", response->body);
        try {
            const auto message = json::parse(response->body).at("choices").at(0).at("message");
            const auto calls = message.value("tool_calls", json::array());
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
                        json& contents) {
    httplib::Client client("https://generativelanguage.googleapis.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(90, 0);
    contents.push_back({{"role", "user"}, {"parts", {{{"text", prompt}}}}});
    trim_history(contents, 0);
    const json tool_config = json::array({{{"functionDeclarations", tool_definitions()}}});
    for (int round = 0; round < max_tool_rounds; ++round) {
        const json payload = {{"systemInstruction", {{"parts", {{{"text", agent_instruction()}}}}}},
                              {"contents", contents}, {"tools", tool_config}};
        const auto response = client.Post("/v1beta/models/" + model + ":generateContent", {{"x-goog-api-key", api_key}}, payload.dump(), "application/json");
        if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
        if (response->status < 200 || response->status >= 300) return parse_error(response->status, "Gemini", response->body);
        try {
            const auto content = json::parse(response->body).at("candidates").at(0).at("content");
            contents.push_back(content); // preserve provider metadata needed by Gemini in the next turn
            json response_parts = json::array();
            bool has_calls = false;
            for (const auto& part : content.at("parts")) {
                if (!part.contains("functionCall")) continue;
                has_calls = true;
                const auto& call = part.at("functionCall");
                const auto execution = execute_tool(tools, confirm, call.at("name").get<std::string>(), call.value("args", json::object()));
                response_parts.push_back({{"functionResponse", {{"name", call.at("name")},
                    {"id", call.value("id", "")}, {"response", execution.result}}}});
            }
            if (!has_calls) {
                std::string text;
                for (const auto& part : content.at("parts")) if (part.contains("text")) text += part.at("text").get<std::string>();
                return {true, text};
            }
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

ApiResult ApiClient::list_models(Provider provider, const std::string& api_key) const {
    if (api_key.empty()) return {false, "The API key cannot be empty."};
    if (provider == Provider::deepseek) return deepseek_models(api_key);
    if (provider == Provider::gemini) return gemini_models(api_key);
    return {false, "Select a provider first."};
}

ApiResult ApiClient::submit_prompt(Provider provider, const std::string& api_key, const std::string& model,
                                   const std::string& prompt, const ToolExecutor& tools,
                                   const ToolExecutor::ConfirmationFn& confirm) {
    if (api_key.empty()) return {false, "No API key is set."};
    if (model.empty()) return {false, "No model is selected. Use /model <name>."};
    if (provider != session_provider_ || model != session_model_) {
        reset_session();
        session_provider_ = provider;
        session_model_ = model;
    }
    if (provider == Provider::deepseek) return deepseek_prompt(api_key, model, prompt, tools, confirm, deepseek_messages_);
    if (provider == Provider::gemini) return gemini_prompt(api_key, model, prompt, tools, confirm, gemini_contents_);
    return {false, "Select a provider first."};
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
