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

std::string error_message(const nlohmann::json& body) {
    if (const auto error = body.find("error"); error != body.end()) {
        if (error->is_string()) return error->get<std::string>();
        if (error->is_object()) return error->value("message", error->dump());
    }
    return body.value("message", "Unknown API error");
}

ApiResult parse_error(int status, const std::string& provider, const std::string& response_body) {
    try {
        return {false, provider + " returned HTTP " + std::to_string(status) + ": " +
                           error_message(nlohmann::json::parse(response_body))};
    } catch (const std::exception&) {
        return {false, provider + " returned HTTP " + std::to_string(status)};
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
        for (const auto& model : nlohmann::json::parse(response->body).at("data")) {
            models.push_back(model.at("id").get<std::string>());
        }
        std::ranges::sort(models);
        return {true, "DeepSeek key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
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
        for (const auto& model : nlohmann::json::parse(response->body).at("models")) {
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
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

ApiResult deepseek_prompt(const std::string& api_key, const std::string& model, const std::string& prompt) {
    httplib::Client client("https://api.deepseek.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(60, 0);
    const nlohmann::json payload = {
        {"model", model}, {"messages", {{{"role", "user"}, {"content", prompt}}}},
        {"max_tokens", 1024}, {"stream", false},
    };
    const auto response = client.Post("/chat/completions", {{"Authorization", "Bearer " + api_key}},
                                      payload.dump(), "application/json");
    if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300) return parse_error(response->status, "DeepSeek", response->body);
    try {
        return {true, nlohmann::json::parse(response->body).at("choices").at(0).at("message").value("content", "")};
    } catch (const std::exception& exception) {
        return {false, "Could not read the API response: " + std::string(exception.what())};
    }
}

ApiResult gemini_prompt(const std::string& api_key, const std::string& model, const std::string& prompt) {
    httplib::Client client("https://generativelanguage.googleapis.com");
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(60, 0);
    nlohmann::json content;
    content["parts"] = nlohmann::json::array();
    content["parts"].push_back({{"text", prompt}});
    nlohmann::json payload;
    payload["contents"] = nlohmann::json::array();
    payload["contents"].push_back(std::move(content));
    const auto response = client.Post("/v1beta/models/" + model + ":generateContent",
                                      {{"x-goog-api-key", api_key}}, payload.dump(), "application/json");
    if (!response) return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300) return parse_error(response->status, "Gemini", response->body);
    try {
        return {true, nlohmann::json::parse(response->body).at("candidates").at(0)
                          .at("content").at("parts").at(0).value("text", "")};
    } catch (const std::exception& exception) {
        return {false, "Could not read the API response: " + std::string(exception.what())};
    }
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

ApiResult ApiClient::submit_prompt(Provider provider, const std::string& api_key,
                                   const std::string& model, const std::string& prompt) const {
    if (api_key.empty()) return {false, "No API key is set."};
    if (model.empty()) return {false, "No model is selected. Use /model <name>."};
    if (provider == Provider::deepseek) return deepseek_prompt(api_key, model, prompt);
    if (provider == Provider::gemini) return gemini_prompt(api_key, model, prompt);
    return {false, "Select a provider first."};
}

} // namespace arn
