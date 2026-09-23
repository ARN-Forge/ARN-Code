#include "arn/core/agent/agent_session.hpp"
#include "arn/core/provider/model_provider.hpp"
#include "arn/core/provider/openrouter_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"
#include "providers/provider_utils.hpp"

#include <httplib.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class AddTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "add",
            "Add two numbers",
            {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }},
                {"required", {"a", "b"}}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext&) override {
        const double a = arguments.value("a", 0.0);
        const double b = arguments.value("b", 0.0);
        return arn::core::ToolResult::success({{"sum", a + b}});
    }
};

class DummyDocTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "read_docs",
            "Read documentation",
            {
                {"type", "object"},
                {"properties", {
                    {"topic", {{"type", "string"}}}
                }}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json&,
                                  const arn::core::ToolContext&) override {
        return arn::core::ToolResult::success({{"content", "docs text"}});
    }
};

} // namespace

int main() {
    try {
        std::cout << "Starting OpenRouter provider tests...\n";

        // 1. Provider creation through generic factory
        {
            auto provider = arn::core::create_provider(arn::core::ProviderType::openrouter);
            check(provider != nullptr, "OpenRouter provider creation failed");
            check(provider->type() == arn::core::ProviderType::openrouter, "ProviderType mismatch");
            check(provider->kind() == arn::core::ProviderType::openrouter, "kind() mismatch");
            check(provider->name() == "OpenRouter", "Provider name mismatch");
            check(provider->session_entries() == 0, "Initial session entries should be 0");
        }

        // 2. Provider metadata and type conversions
        {
            check(arn::core::provider_type_name(arn::core::ProviderType::openrouter) == "OpenRouter",
                  "provider_type_name(openrouter)");
            check(arn::core::provider_type_from_name("openrouter") == arn::core::ProviderType::openrouter,
                  "provider_type_from_name(openrouter)");
            check(arn::core::provider_type_from_name("open-router") == arn::core::ProviderType::openrouter,
                  "provider_type_from_name(open-router)");
            check(arn::core::provider_type_from_name("open_router") == arn::core::ProviderType::openrouter,
                  "provider_type_from_name(open_router)");

            arn::core::OpenRouterProvider provider;
            check(provider.preferred_model({"anthropic/claude-3.5-sonnet", "openai/gpt-4o"}) ==
                      "anthropic/claude-3.5-sonnet",
                  "Preferred model exact match");
            check(provider.preferred_model({"custom/my-gpt-4o-tuned"}) == "custom/my-gpt-4o-tuned",
                  "Preferred model substring fallback");
            check(provider.preferred_model({"unknown/model-xyz"}) == "unknown/model-xyz",
                  "Preferred model first element fallback");
            check(provider.preferred_model({}).empty(), "Preferred model empty fallback");
        }

        // 3. Model catalog parsing & capability detection
        {
            const std::string catalog_json = R"({
                "data": [
                    {
                        "id": "anthropic/claude-3.5-sonnet",
                        "name": "Anthropic: Claude 3.5 Sonnet",
                        "description": "Smart model",
                        "supported_parameters": ["tools", "tool_choice", "temperature"]
                    },
                    {
                        "id": "meta-llama/llama-2-70b-chat",
                        "name": "Meta: Llama 2 70B",
                        "supported_parameters": ["temperature", "max_tokens"]
                    },
                    {
                        "id": "openai/text-embedding-3-small",
                        "architecture": {
                            "modality": "text->embeddings"
                        }
                    },
                    {
                        "id": "mistralai/mistral-7b-instruct",
                        "name": "Mistral 7B"
                    }
                ]
            })";

            const auto entries = arn::core::detail::parse_openrouter_model_catalog(catalog_json);
            check(entries.size() == 3, "Embedding model should be filtered out");
            
            // Should be sorted by id
            check(entries[0].id == "anthropic/claude-3.5-sonnet", "Entry 0 id intact");
            check(entries[0].capabilities.supports_tools, "Claude 3.5 supports tools");
            
            check(entries[1].id == "meta-llama/llama-2-70b-chat", "Entry 1 id intact");
            check(!entries[1].capabilities.supports_tools, "Llama 2 lacks tools in supported_parameters");

            check(entries[2].id == "mistralai/mistral-7b-instruct", "Entry 2 id intact");
            check(entries[2].capabilities.supports_tools, "Default without supported_parameters is true");
        }

        // 4. Malformed model catalog handling
        {
            // Empty data array
            check(arn::core::detail::parse_openrouter_model_catalog(R"({"data":[]})").empty(),
                  "Empty data array");

            // Missing data field
            check(arn::core::detail::parse_openrouter_model_catalog(R"({"other":123})").empty(),
                  "Missing data field");

            // Incomplete entries (missing id or invalid types)
            const std::string malformed_entries = R"({
                "data": [
                    {},
                    {"id": 12345},
                    {"id": ""},
                    {"id": "valid/model", "architecture": "not-an-object"}
                ]
            })";
            const auto res = arn::core::detail::parse_openrouter_model_catalog(malformed_entries);
            check(res.size() == 1, "Only the 1 valid entry should survive");
            check(res[0].id == "valid/model", "Valid model id");

            // Non-JSON string throws
            bool caught = false;
            try {
                arn::core::detail::parse_openrouter_model_catalog("<html>502 Bad Gateway</html>");
            } catch (const nlohmann::json::exception&) {
                caught = true;
            }
            check(caught, "Malformed JSON must throw json::exception");
        }

        // 5. System instruction serialization
        {
            arn::core::OpenRouterProvider provider;
            arn::core::ToolRegistry tools;
            std::atomic_bool cancel{true}; // Cancel immediately so no HTTP call happens

            // Test system instruction with cancellation before execute
            auto turn = provider.start_turn("key", "test-model", "Custom system prompt", "User prompt",
                                            tools, {}, &cancel);
            check(turn.cancelled, "Turn should be cancelled");
        }

        // 6. Conversation / history serialization & trimming
        {
            arn::core::OpenRouterProvider provider;
            check(provider.session_entries() == 0, "Empty session");
            provider.trim_history(10);
            check(provider.session_entries() == 0, "Trim on empty session");
            provider.reset_session();
            check(provider.session_entries() == 0, "Reset session");
        }

        // 7. ToolRegistry schema serialization
        {
            arn::core::ToolRegistry registry;
            registry.register_tool(std::make_shared<AddTool>());
            registry.register_tool(std::make_shared<DummyDocTool>());

            const auto tools_json = arn::core::detail::serialize_openrouter_tools(registry);
            check(tools_json.is_array() && tools_json.size() == 2, "Tool count serialized");
            check(tools_json[0]["type"] == "function", "Tool type function");
            check(tools_json[0]["function"].contains("name"), "Function contains name");
            check(tools_json[0]["function"].contains("description"), "Function contains description");
            check(tools_json[0]["function"].contains("parameters"), "Function contains parameters");

            nlohmann::json messages = nlohmann::json::array({{{"role", "user"}, {"content", "Hello"}}});

            // With supports_tools = true
            const auto payload_with_tools =
                arn::core::detail::build_openrouter_payload("model-a", messages, registry, true);
            check(payload_with_tools.contains("tools"), "Payload should contain tools");
            check(payload_with_tools.value("tool_choice", "") == "auto", "tool_choice should be auto");

            // With supports_tools = false (capability disabled)
            const auto payload_without_tools =
                arn::core::detail::build_openrouter_payload("model-b", messages, registry, false);
            check(!payload_without_tools.contains("tools"), "Payload must not contain tools when unsupported");
            check(!payload_without_tools.contains("tool_choice"), "Payload must not contain tool_choice");
        }

        // 8. Normal streamed text parsing
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();
            std::string streamed;

            const std::string chunk1 = R"({"choices":[{"delta":{"content":"Hello"}}]})";
            const std::string chunk2 = R"({"choices":[{"delta":{"content":" world!"}}]})";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls,
                                                             [&](std::string_view delta) { streamed += delta; });
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls,
                                                             [&](std::string_view delta) { streamed += delta; });

            check(text == "Hello world!", "Accumulated text mismatch");
            check(streamed == "Hello world!", "Streamed callback mismatch");
            check(tool_calls.empty(), "Tool calls should be empty");
        }

        // 9. Streamed tool-call parsing
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            const std::string chunk = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": "call_abc123",
                            "type": "function",
                            "function": {
                                "name": "add",
                                "arguments": "{\"a\": 5, \"b\": 7}"
                            }
                        }]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk, text, tool_calls, nullptr);
            check(tool_calls.size() == 1, "Tool call parsed");
            check(tool_calls[0].value("id", "") == "call_abc123", "Tool call id");
            check(tool_calls[0]["function"].value("name", "") == "add", "Tool function name");
            check(tool_calls[0]["function"]["arguments"].get<std::string>() == "{\"a\": 5, \"b\": 7}",
                  "Tool function arguments");
        }

        // 10. Tool argument delta aggregation across multiple chunks
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            const std::string chunk1 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": "call_multi_1",
                            "type": "function",
                            "function": {"name": "add", "arguments": "{\"a\":"}
                        }]
                    }
                }]
            })";

            const std::string chunk2 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "function": {"arguments": " 12, \"b\":"}
                        }]
                    }
                }]
            })";

            const std::string chunk3 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "function": {"arguments": " 34}"}
                        }]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk3, text, tool_calls, nullptr);

            check(tool_calls.size() == 1, "Expected single aggregated tool call");
            check(tool_calls[0]["function"]["arguments"].get<std::string>() == "{\"a\": 12, \"b\": 34}",
                  "Aggregated arguments mismatch");
        }

        // 11. Multiple tool calls in one response
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            // First chunk declares tool 0 and tool 1
            const std::string chunk1 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [
                            {"index": 0, "id": "call_0", "function": {"name": "add", "arguments": "{\"a\": 1,"}},
                            {"index": 1, "id": "call_1", "function": {"name": "read_docs", "arguments": "{\"topic\":"}}
                        ]
                    }
                }]
            })";

            // Second chunk streams arguments to both
            const std::string chunk2 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [
                            {"index": 0, "function": {"arguments": " \"b\": 2}"}},
                            {"index": 1, "function": {"arguments": " \"cpp\"}"}}
                        ]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls, nullptr);

            check(tool_calls.size() == 2, "Expected 2 independent tool calls");
            check(tool_calls[0].value("id", "") == "call_0", "Tool 0 id");
            check(tool_calls[0]["function"].value("name", "") == "add", "Tool 0 name");
            check(tool_calls[0]["function"]["arguments"].get<std::string>() == "{\"a\": 1, \"b\": 2}",
                  "Tool 0 arguments aggregated");

            check(tool_calls[1].value("id", "") == "call_1", "Tool 1 id");
            check(tool_calls[1]["function"].value("name", "") == "read_docs", "Tool 1 name");
            check(tool_calls[1]["function"]["arguments"].get<std::string>() == "{\"topic\": \"cpp\"}",
                  "Tool 1 arguments aggregated");

            // [DONE] marker must be ignored safely
            arn::core::detail::parse_openrouter_stream_chunk("[DONE]", text, tool_calls, nullptr);
            check(tool_calls.size() == 2, "Count after [DONE]");
        }

        // 12. Error response parsing
        {
            const auto err401 = arn::core::detail::parse_error(
                401, "OpenRouter", R"({"error": {"message": "Invalid API key", "code": 401}})");
            check(!err401.ok, "Error ok == false");
            check(err401.message.find("Invalid API key") != std::string::npos, "Parsed 401 message");

            const auto err402 = arn::core::detail::parse_error(
                402, "OpenRouter", R"({"error": "Insufficient credits"})");
            check(err402.message.find("Insufficient credits") != std::string::npos, "Parsed 402 string message");

            const auto err500 = arn::core::detail::parse_error(500, "OpenRouter", "Server error");
            check(err500.message.find("HTTP 500") != std::string::npos, "Parsed fallback 500 message");
        }

        // 13. Cancellation behavior without real network
        {
            arn::core::OpenRouterProvider provider;
            std::atomic_bool cancel{true};
            auto res = provider.list_models("test-key", &cancel);
            check(!res.ok, "Cancelled list_models should fail");
            check(res.cancelled, "Cancelled list_models should set cancelled flag");

            arn::core::ToolRegistry tools;
            auto turn = provider.start_turn("key", "test-model", "Sys", "Prompt", tools, {}, &cancel);
            check(!turn.ok && turn.cancelled, "Cancelled start_turn should set cancelled flag");

            auto turn2 = provider.continue_turn("key", "test-model", "Sys", {}, tools, {}, &cancel);
            check(!turn2.ok && turn2.cancelled, "Cancelled continue_turn should set cancelled flag");

            provider.cancel_active_request();
        }

        // 14. OpenRouter operation through AgentSession using a mock loopback transport
        {
            std::cout << "Running isolated loopback server test with AgentSession...\n";

            httplib::Server mock_server;
            int turn_count = 0;

            mock_server.Get("/api/v1/models", [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({
                    "data": [
                        {
                            "id": "openai/gpt-4o",
                            "name": "GPT-4o",
                            "supported_parameters": ["tools", "temperature"]
                        }
                    ]
                })", "application/json");
            });

            mock_server.Post("/api/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
                ++turn_count;
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");

                if (turn_count == 1) {
                    // Turn 1: Model requests tool call to add(10, 20)
                    std::string chunk1 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"call_add1\", \"type\": \"function\", \"function\": {\"name\": \"add\", \"arguments\": \"{\\\"a\\\": 10, \\\"b\\\": 20}\"}}]}}]}\n\n";
                    std::string chunk2 = "data: [DONE]\n\n";
                    res.set_content(chunk1 + chunk2, "text/event-stream");
                } else {
                    // Turn 2: Model receives tool result and produces final answer
                    std::string chunk1 = "data: {\"choices\": [{\"delta\": {\"content\": \"The sum is 30.\"}}]}\n\n";
                    std::string chunk2 = "data: [DONE]\n\n";
                    res.set_content(chunk1 + chunk2, "text/event-stream");
                }
            });

            const int port = mock_server.bind_to_any_port("127.0.0.1");
            check(port > 0, "Mock server failed to bind to port");

            std::thread server_thread([&] { mock_server.listen_after_bind(); });

            try {
                arn::core::OpenRouterConfig cfg{
                    .endpoint = "http://127.0.0.1:" + std::to_string(port),
                    .api_path_prefix = "/api/v1",
                    .http_referer = "https://github.com/arn-org/arn",
                    .app_title = "ARN Core Test"
                };

                auto provider = std::make_shared<arn::core::OpenRouterProvider>(cfg);
                check(provider->config().http_referer == "https://github.com/arn-org/arn", "Config referer");
                check(provider->config().app_title == "ARN Core Test", "Config app title");

                auto tools = std::make_shared<arn::core::ToolRegistry>();
                tools->register_tool(std::make_shared<AddTool>());

                arn::core::AgentSession session;
                session.set_tools(tools);
                session.set_system_instruction("You are a helpful assistant.");

                auto config_res = session.configure_provider(provider, "fake-api-key");
                check(config_res.ok, "Configure OpenRouter provider through AgentSession");
                check(session.active_provider() == arn::core::ProviderType::openrouter,
                      "Active provider is OpenRouter");
                check(session.active_model() == "openai/gpt-4o", "Selected active model");

                std::string streamed_result;
                auto prompt_res = session.prompt("What is 10 + 20?",
                                                 [&](std::string_view delta) { streamed_result += delta; });

                check(prompt_res.ok, "Prompt round failed");
                check(prompt_res.message == "The sum is 30.", "Prompt final message mismatch");
                check(streamed_result == "The sum is 30.", "Streamed result mismatch");
                check(turn_count == 2, "AgentSession must have performed 2 turns (tool call + final)");

                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
            } catch (...) {
                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
                throw;
            }
        }

        std::cout << "All 14 OpenRouter provider tests passed successfully!\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OpenRouter test error: " << error.what() << '\n';
        return 1;
    }
}
