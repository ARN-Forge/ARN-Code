#include "arn/core/provider/model_provider.hpp"
#include "arn/core/provider/gemini_provider.hpp"
#include "arn/core/provider/deepseek_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"
#include "providers/provider_utils.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class DummySearchTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "search_docs",
            "Search project documentation",
            {
                {"type", "object"},
                {"properties", {
                    {"query", {{"type", "string"}, {"description", "The query to search"}}}
                }},
                {"required", {"query"}}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json&,
                                  const arn::core::ToolContext&) override {
        return arn::core::ToolResult::success({{"results", {"doc1.md", "doc2.md"}}});
    }
};

class DummyWeatherTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "get_weather",
            "Get current weather",
            {
                {"type", "object"},
                {"properties", {
                    {"city", {{"type", "string"}}}
                }}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json&,
                                  const arn::core::ToolContext&) override {
        return arn::core::ToolResult::success({{"temp", 22}});
    }
};

} // namespace

int main() {
    try {
        // 1. Provider creation and metadata
        check(!arn::core::create_provider(arn::core::ProviderType::none), "None provider was non-null");
        check(!arn::core::create_provider(arn::core::ProviderType::custom), "Custom provider was non-null");

        auto gemini = arn::core::create_provider(arn::core::ProviderType::gemini);
        check(gemini != nullptr, "Gemini provider creation failed");
        check(gemini->type() == arn::core::ProviderType::gemini, "Gemini type mismatch");
        check(gemini->kind() == arn::core::ProviderType::gemini, "Gemini kind mismatch");
        check(gemini->name() == "Gemini", "Gemini name mismatch");
        check(gemini->session_entries() == 0, "Gemini session must start empty");

        auto deepseek = arn::core::create_provider(arn::core::ProviderType::deepseek);
        check(deepseek != nullptr, "DeepSeek provider creation failed");
        check(deepseek->type() == arn::core::ProviderType::deepseek, "DeepSeek type mismatch");
        check(deepseek->kind() == arn::core::ProviderType::deepseek, "DeepSeek kind mismatch");
        check(deepseek->name() == "DeepSeek", "DeepSeek name mismatch");
        check(deepseek->session_entries() == 0, "DeepSeek session must start empty");

        check(arn::core::provider_type_name(arn::core::ProviderType::gemini) == "Gemini", "Name check");
        check(arn::core::provider_type_name(arn::core::ProviderType::deepseek) == "DeepSeek", "Name check");
        check(arn::core::provider_type_name(arn::core::ProviderType::custom) == "Custom", "Name check");
        check(arn::core::provider_type_name(arn::core::ProviderType::none) == "none", "Name check");

        check(arn::core::provider_type_from_name("gemini") == arn::core::ProviderType::gemini, "From name check");
        check(arn::core::provider_type_from_name("deepseek") == arn::core::ProviderType::deepseek, "From name check");
        check(arn::core::provider_type_from_name("custom") == arn::core::ProviderType::custom, "From name check");
        check(arn::core::provider_type_from_name("unknown") == arn::core::ProviderType::none, "From name check");

        // 2. Preferred model behavior
        check(gemini->preferred_model({"gemini-pro", "gemini-3.5-flash-lite"}) == "gemini-3.5-flash-lite",
              "Gemini default preference check");
        check(gemini->preferred_model({"gemini-image", "gemini-custom-flash"}) == "gemini-custom-flash",
              "Gemini flash filtering check");
        check(gemini->preferred_model({"gemini-tts", "gemini-transcribe", "gemini-2.0-flash"}) == "gemini-2.0-flash",
              "Gemini negative filter check");
        check(gemini->preferred_model({"unknown-model"}) == "unknown-model", "Gemini fallback check");
        check(gemini->preferred_model({}).empty(), "Gemini empty check");

        check(deepseek->preferred_model({"deepseek-reasoner", "deepseek-chat"}) == "deepseek-chat",
              "DeepSeek default preference check");
        check(deepseek->preferred_model({"deepseek-reasoner"}) == "deepseek-reasoner",
              "DeepSeek reasoner preference check");
        check(deepseek->preferred_model({"other-model"}) == "other-model", "DeepSeek fallback check");
        check(deepseek->preferred_model({}).empty(), "DeepSeek empty check");

        // 3. ToolRegistry schema serialization
        arn::core::ToolRegistry empty_registry;
        check(arn::core::detail::serialize_gemini_tools(empty_registry).empty(), "Empty gemini tools");
        check(arn::core::detail::serialize_deepseek_tools(empty_registry).empty(), "Empty deepseek tools");

        arn::core::ToolRegistry registry;
        registry.register_tool(std::make_shared<DummySearchTool>());
        registry.register_tool(std::make_shared<DummyWeatherTool>());
        check(registry.size() == 2, "Registry size");

        // Gemini tool serialization
        const auto gemini_tools = arn::core::detail::serialize_gemini_tools(registry);
        check(gemini_tools.is_array() && gemini_tools.size() == 1, "Gemini tools structure");
        check(gemini_tools[0].contains("functionDeclarations"), "Gemini functionDeclarations key");
        const auto& decls = gemini_tools[0]["functionDeclarations"];
        check(decls.size() == 2, "Gemini declaration count");
        check(decls[0].value("name", "") == "get_weather" || decls[1].value("name", "") == "get_weather", "Gemini tool weather name");
        check(decls[0].value("name", "") == "search_docs" || decls[1].value("name", "") == "search_docs", "Gemini tool search name");

        // DeepSeek tool serialization
        const auto deepseek_tools = arn::core::detail::serialize_deepseek_tools(registry);
        check(deepseek_tools.is_array() && deepseek_tools.size() == 2, "DeepSeek tools structure");
        check(deepseek_tools[0]["type"] == "function" && deepseek_tools[0].contains("function"), "DeepSeek tool item");
        check(deepseek_tools[1]["type"] == "function" && deepseek_tools[1].contains("function"), "DeepSeek tool item");

        // 4. Payload construction & system instruction propagation
        nlohmann::json gemini_contents = nlohmann::json::array();
        gemini_contents.push_back({
            {"role", "user"},
            {"parts", nlohmann::json::array({{{"text", "Hello"}}})}
        });

        const auto gemini_payload = arn::core::detail::build_gemini_payload("Custom system instructions", gemini_contents, registry);
        check(gemini_payload.contains("systemInstruction"), "Gemini system instruction key");
        check(gemini_payload["systemInstruction"]["parts"][0]["text"].get<std::string>() == "Custom system instructions",
              "Gemini system instruction propagation");
        check(gemini_payload.contains("tools"), "Gemini tools in payload");
        check(gemini_payload["contents"] == gemini_contents, "Gemini contents");

        // Empty system instruction test
        const auto gemini_payload_no_sys = arn::core::detail::build_gemini_payload("", gemini_contents, empty_registry);
        check(!gemini_payload_no_sys.contains("systemInstruction"), "Gemini payload without system instruction");
        check(!gemini_payload_no_sys.contains("tools"), "Gemini payload without tools");

        nlohmann::json deepseek_messages = nlohmann::json::array({
            {{"role", "system"}, {"content", "You are an assistant"}},
            {{"role", "user"}, {"content", "Hello"}}
        });
        const auto deepseek_payload = arn::core::detail::build_deepseek_payload("deepseek-chat", deepseek_messages, registry);
        check(deepseek_payload.value("model", "") == "deepseek-chat", "DeepSeek model");
        check(deepseek_payload["messages"] == deepseek_messages, "DeepSeek messages");
        check(deepseek_payload.contains("tools") && deepseek_payload.value("tool_choice", "") == "auto", "DeepSeek tools and choice");

        const auto deepseek_payload_no_tools = arn::core::detail::build_deepseek_payload("deepseek-chat", deepseek_messages, empty_registry);
        check(!deepseek_payload_no_tools.contains("tools"), "DeepSeek no tools");
        check(!deepseek_payload_no_tools.contains("tool_choice"), "DeepSeek no tool choice");

        // 5. Streamed parsing & protocol compatibility
        // 5a. Gemini stream parsing
        std::string gemini_text;
        nlohmann::json gemini_parts = nlohmann::json::array();
        nlohmann::json gemini_calls = nlohmann::json::array();
        std::string stream_delta;

        std::string gemini_chunk_text = R"({"candidates":[{"content":{"parts":[{"text":"Partial delta"}]}}]})";
        arn::core::detail::parse_gemini_stream_chunk(
            gemini_chunk_text, gemini_text, gemini_parts, gemini_calls,
            [&](std::string_view delta) { stream_delta += delta; });

        check(gemini_text == "Partial delta", "Gemini text accumulator");
        check(stream_delta == "Partial delta", "Gemini callback text");
        check(gemini_parts.size() == 1, "Gemini parts count");
        check(gemini_calls.empty(), "Gemini calls should be empty");

        // Test Gemini 3 thoughtSignature preservation on function call part
        std::string gemini_chunk_call = R"({
            "candidates": [{
                "content": {
                    "parts": [{
                        "thoughtSignature": "gemini3_thought_signature_xyz",
                        "functionCall": {
                            "name": "search_docs",
                            "args": {"query": "deep learning"}
                        }
                    }]
                }
            }]
        })";
        arn::core::detail::parse_gemini_stream_chunk(
            gemini_chunk_call, gemini_text, gemini_parts, gemini_calls, nullptr);

        check(gemini_parts.size() == 2, "Gemini parts should now have 2 parts");
        check(gemini_parts[1].contains("thoughtSignature") &&
              gemini_parts[1]["thoughtSignature"].get<std::string>() == "gemini3_thought_signature_xyz",
              "Gemini 3 thoughtSignature must be preserved exactly");
        check(gemini_calls.size() == 1, "Gemini function call extracted");
        check(gemini_calls[0].value("name", "") == "search_docs", "Gemini call name");
        check(gemini_calls[0]["args"]["query"].get<std::string>() == "deep learning", "Gemini call args");

        // 5b. DeepSeek stream parsing and argument delta aggregation
        std::string deepseek_text;
        nlohmann::json deepseek_calls = nlohmann::json::array();
        std::string ds_stream_delta;

        std::string ds_chunk_text = R"({"choices":[{"delta":{"content":"DeepSeek answer"}}]})";
        arn::core::detail::parse_deepseek_stream_chunk(
            ds_chunk_text, deepseek_text, deepseek_calls,
            [&](std::string_view delta) { ds_stream_delta += delta; });
        check(deepseek_text == "DeepSeek answer", "DeepSeek text accumulator");
        check(ds_stream_delta == "DeepSeek answer", "DeepSeek delta callback");

        // Multi-chunk streamed tool argument delta aggregation
        std::string ds_tool_chunk1 = R"({
            "choices": [{
                "delta": {
                    "tool_calls": [{
                        "index": 0,
                        "id": "call_123",
                        "type": "function",
                        "function": {
                            "name": "search_docs",
                            "arguments": "{\"query\":"
                        }
                    }]
                }
            }]
        })";

        std::string ds_tool_chunk2 = R"({
            "choices": [{
                "delta": {
                    "tool_calls": [{
                        "index": 0,
                        "function": {
                            "arguments": " \"c++23\"}"
                        }
                    }]
                }
            }]
        })";

        arn::core::detail::parse_deepseek_stream_chunk(ds_tool_chunk1, deepseek_text, deepseek_calls, nullptr);
        arn::core::detail::parse_deepseek_stream_chunk(ds_tool_chunk2, deepseek_text, deepseek_calls, nullptr);

        check(deepseek_calls.size() == 1, "DeepSeek tool call count");
        check(deepseek_calls[0].value("id", "") == "call_123", "DeepSeek tool call id");
        check(deepseek_calls[0]["function"].value("name", "") == "search_docs", "DeepSeek tool call name");
        check(deepseek_calls[0]["function"]["arguments"].get<std::string>() == "{\"query\": \"c++23\"}",
              "DeepSeek streamed arguments must be aggregated correctly across chunks");

        // Verify [DONE] packet does not break anything
        arn::core::detail::parse_deepseek_stream_chunk("[DONE]", deepseek_text, deepseek_calls, nullptr);
        check(deepseek_calls.size() == 1, "DeepSeek call count after [DONE]");

        std::cout << "All ARN Core provider tests passed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
