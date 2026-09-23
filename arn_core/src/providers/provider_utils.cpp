#include "provider_utils.hpp"

#include <algorithm>
#include <cctype>

namespace arn::core::detail {

std::string error_message(const nlohmann::json& body) {
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
                           error_message(nlohmann::json::parse(response_body))};
    } catch (const std::exception&) {
        return {false, provider + " returned HTTP " + std::to_string(status)};
    }
}

void trim_history(nlohmann::json& history, std::size_t keep_from) {
    while (history.size() > max_history_entries) {
        history.erase(history.begin() + static_cast<nlohmann::json::difference_type>(keep_from));
    }
}

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

nlohmann::json serialize_gemini_tools(const ToolRegistry& tools) {
    if (tools.empty())
        return nlohmann::json::array();
    return nlohmann::json::array({{{"functionDeclarations", tools.definitions_json()}}});
}

nlohmann::json build_gemini_payload(const std::string& system_instruction,
                                    const nlohmann::json& contents,
                                    const ToolRegistry& tools) {
    nlohmann::json payload = {{"contents", contents}};
    if (!system_instruction.empty()) {
        payload["systemInstruction"] = {{"parts", {{{"text", system_instruction}}}}};
    }
    if (!tools.empty()) {
        payload["tools"] = serialize_gemini_tools(tools);
    }
    return payload;
}

void parse_gemini_stream_chunk(std::string_view event,
                               std::string& text_accumulator,
                               nlohmann::json& model_response_parts,
                               nlohmann::json& function_calls,
                               const TextStreamCallback& on_text) {
    try {
        const auto parsed = nlohmann::json::parse(event);
        const auto packets = parsed.is_array() ? parsed : nlohmann::json::array({parsed});
        for (const auto& packet : packets) {
            const auto candidates_it = packet.find("candidates");
            if (candidates_it == packet.end() || !candidates_it->is_array() || candidates_it->empty())
                continue;
            const auto& first_candidate = (*candidates_it)[0];
            const auto content_it = first_candidate.find("content");
            if (content_it == first_candidate.end() || !content_it->is_object())
                continue;
            const auto parts_it = content_it->find("parts");
            if (parts_it == content_it->end() || !parts_it->is_array())
                continue;
            for (const auto& part : *parts_it) {
                model_response_parts.push_back(part);
                if (part.contains("text")) {
                    const auto chunk = part.at("text").get<std::string>();
                    text_accumulator += chunk;
                    if (on_text)
                        on_text(chunk);
                }
                if (part.contains("functionCall"))
                    function_calls.push_back(part.at("functionCall"));
            }
        }
    } catch (const std::exception&) {
        // Ignore malformed transient SSE data
    }
}

nlohmann::json serialize_deepseek_tools(const ToolRegistry& tools) {
    auto result = nlohmann::json::array();
    for (const auto& definition : tools.definitions_json()) {
        result.push_back({{"type", "function"}, {"function", definition}});
    }
    return result;
}

nlohmann::json build_deepseek_payload(const std::string& model,
                                      const nlohmann::json& messages,
                                      const ToolRegistry& tools) {
    nlohmann::json payload = {
        {"model", model},
        {"messages", messages},
        {"max_tokens", 2048},
        {"stream", true}
    };
    if (!tools.empty()) {
        payload["tools"] = serialize_deepseek_tools(tools);
        payload["tool_choice"] = "auto";
    }
    return payload;
}

void parse_deepseek_stream_chunk(std::string_view event,
                                 std::string& text_accumulator,
                                 nlohmann::json& tool_calls,
                                 const TextStreamCallback& on_text) {
    if (event == "[DONE]")
        return;
    try {
        const auto parsed = nlohmann::json::parse(event);
        const auto packets = parsed.is_array() ? parsed : nlohmann::json::array({parsed});
        for (const auto& packet : packets) {
            const auto choices_it = packet.find("choices");
            if (choices_it == packet.end() || !choices_it->is_array() || choices_it->empty())
                continue;
            const auto delta_it = (*choices_it)[0].find("delta");
            if (delta_it == (*choices_it)[0].end() || !delta_it->is_object())
                continue;
            const auto& delta = *delta_it;
            if (delta.contains("content") && !delta.at("content").is_null()) {
                const auto chunk = delta.at("content").get<std::string>();
                text_accumulator += chunk;
                if (on_text)
                    on_text(chunk);
            }
            for (const auto& change : delta.value("tool_calls", nlohmann::json::array())) {
                const auto index = change.value("index", 0U);
                while (tool_calls.size() <= index) {
                    tool_calls.push_back(
                        {{"id", ""},
                         {"type", "function"},
                         {"function", {{"name", ""}, {"arguments", ""}}}});
                }
                auto& call = tool_calls.at(index);
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
        // Ignore malformed transient SSE data
    }
}

std::string join_api_path(std::string_view prefix, std::string_view path) {
    if (prefix.empty()) return std::string(path);
    if (path.empty()) return std::string(prefix);
    if (prefix.ends_with('/') && path.starts_with('/')) {
        return std::string(prefix.substr(0, prefix.size() - 1)) + std::string(path);
    }
    if (!prefix.ends_with('/') && !path.starts_with('/')) {
        return std::string(prefix) + "/" + std::string(path);
    }
    return std::string(prefix) + std::string(path);
}

std::vector<OpenRouterModelEntry> parse_openrouter_model_catalog(std::string_view body) {
    const auto document = nlohmann::json::parse(body);
    std::vector<OpenRouterModelEntry> entries;
    const auto data_it = document.find("data");
    if (data_it == document.end() || !data_it->is_array())
        return entries;

    for (const auto& item : *data_it) {
        if (!item.is_object())
            continue;
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_string())
            continue;
        const auto id = id_it->get<std::string>();
        if (id.empty())
            continue;

        if (item.contains("architecture") && item["architecture"].is_object()) {
            const auto& arch = item["architecture"];
            if (arch.contains("modality") && arch["modality"].is_string()) {
                const auto modality = arch["modality"].get<std::string>();
                if (modality.find("embeddings") != std::string::npos ||
                    modality.find("image->image") != std::string::npos) {
                    continue;
                }
            }
        }

        ModelCapabilities caps;
        if (item.contains("supported_parameters") && item["supported_parameters"].is_array()) {
            const auto& params = item["supported_parameters"];
            if (!params.empty()) {
                bool has_tools = false;
                for (const auto& p : params) {
                    if (p.is_string() && p.get<std::string>() == "tools") {
                        has_tools = true;
                        break;
                    }
                }
                caps.supports_tools = has_tools;
            }
        }

        std::string name = item.value("name", id);
        std::string desc = item.value("description", "");

        entries.push_back(OpenRouterModelEntry{
            .id = id,
            .name = std::move(name),
            .description = std::move(desc),
            .capabilities = caps
        });
    }

    std::ranges::sort(entries, [](const auto& a, const auto& b) { return a.id < b.id; });
    return entries;
}

nlohmann::json serialize_openrouter_tools(const ToolRegistry& tools) {
    auto result = nlohmann::json::array();
    for (const auto& definition : tools.definitions_json()) {
        result.push_back({{"type", "function"}, {"function", definition}});
    }
    return result;
}

nlohmann::json build_openrouter_payload(const std::string& model,
                                        const nlohmann::json& messages,
                                        const ToolRegistry& tools,
                                        bool supports_tools) {
    nlohmann::json payload = {
        {"model", model},
        {"messages", messages},
        {"stream", true}
    };
    if (supports_tools && !tools.empty()) {
        payload["tools"] = serialize_openrouter_tools(tools);
        payload["tool_choice"] = "auto";
    }
    return payload;
}

void parse_openrouter_stream_chunk(std::string_view event,
                                   std::string& text_accumulator,
                                   nlohmann::json& tool_calls,
                                   const TextStreamCallback& on_text) {
    if (event == "[DONE]")
        return;
    try {
        const auto parsed = nlohmann::json::parse(event);
        const auto packets = parsed.is_array() ? parsed : nlohmann::json::array({parsed});
        for (const auto& packet : packets) {
            if (packet.contains("error"))
                continue;
            const auto choices_it = packet.find("choices");
            if (choices_it == packet.end() || !choices_it->is_array() || choices_it->empty())
                continue;
            const auto delta_it = (*choices_it)[0].find("delta");
            if (delta_it == (*choices_it)[0].end() || !delta_it->is_object())
                continue;
            const auto& delta = *delta_it;
            if (delta.contains("content") && !delta.at("content").is_null()) {
                const auto chunk = delta.at("content").get<std::string>();
                text_accumulator += chunk;
                if (on_text)
                    on_text(chunk);
            }
            for (const auto& change : delta.value("tool_calls", nlohmann::json::array())) {
                const auto index = change.value("index", 0U);
                while (tool_calls.size() <= index) {
                    tool_calls.push_back(
                        {{"id", ""},
                         {"type", "function"},
                         {"function", {{"name", ""}, {"arguments", ""}}}});
                }
                auto& call = tool_calls.at(index);
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
        // Ignore malformed transient SSE data
    }
}

} // namespace arn::core::detail
