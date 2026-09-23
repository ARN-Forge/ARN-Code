#include "arn/core/agent/agent_session.hpp"
#include "arn/core/provider/model_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class CalcTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "calculate",
            "Calculate mathematical expression",
            {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }}
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

class ProtectedTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "dangerous_op",
            "A sensitive operation requiring approval",
            {{"type", "object"}}
        };
        return def;
    }

    bool requires_confirmation() const noexcept override {
        return true;
    }

    arn::core::ToolResult execute(const nlohmann::json&,
                                  const arn::core::ToolContext& context) override {
        arn::core::ConfirmationRequest req("dangerous_op", nlohmann::json::object(), "Approval needed", true);
        if (context.confirm && !context.confirm(req)) {
            return arn::core::ToolResult::failure("Operation declined by user.");
        }
        return arn::core::ToolResult::success({{"applied", true}});
    }
};

class MockModelProvider final : public arn::core::IModelProvider {
public:
    arn::core::ProviderType type() const noexcept override {
        return arn::core::ProviderType::custom;
    }

    std::string_view name() const noexcept override {
        return "Mock";
    }

    std::string preferred_model(const std::vector<std::string>& models) const override {
        return models.empty() ? "" : models.front();
    }

    arn::core::ApiResult list_models(const std::string&, const std::atomic_bool*) override {
        return {true, "Mock models listed", {"mock-model-1", "mock-model-2"}};
    }

    void trim_history(std::size_t max_entries) override {
        history_trimmed_to = max_entries;
        while (recorded_turns.size() > max_entries) {
            recorded_turns.erase(recorded_turns.begin());
        }
    }

    void cancel_active_request() override {
        was_cancelled = true;
    }

    void reset_session() override {
        recorded_turns.clear();
        canned_turns.clear();
        canned_turn_index = 0;
        was_reset = true;
    }

    std::size_t session_entries() const noexcept override {
        return recorded_turns.size();
    }

    arn::core::ModelTurn start_turn(
        const std::string& api_key, const std::string& model,
        const std::string& system_instruction, const std::string& user_prompt,
        const arn::core::ToolRegistry& tools,
        const arn::core::StreamCallbacks& callbacks = {},
        const std::atomic_bool* cancel_requested = nullptr) override {

        if (cancel_requested && cancel_requested->load())
            return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

        last_api_key = api_key;
        last_model = model;
        last_system_instruction = system_instruction;
        last_user_prompt = user_prompt;
        last_tool_definitions = tools.definitions();
        recorded_turns.push_back("user: " + user_prompt);

        if (on_start_turn)
            return on_start_turn();

        if (canned_turn_index < canned_turns.size()) {
            auto turn = canned_turns[canned_turn_index++];
            if (callbacks.on_text && !turn.text.empty())
                callbacks.on_text(turn.text);
            return turn;
        }

        if (callbacks.on_text)
            callbacks.on_text("Default mock response");
        return {.ok = true, .text = "Default mock response"};
    }

    arn::core::ModelTurn continue_turn(
        const std::string&, const std::string&,
        const std::string&,
        const std::vector<arn::core::ToolResponse>& tool_responses,
        const arn::core::ToolRegistry&,
        const arn::core::StreamCallbacks& callbacks = {},
        const std::atomic_bool* cancel_requested = nullptr) override {

        if (cancel_requested && cancel_requested->load())
            return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

        last_tool_responses = tool_responses;
        for (const auto& tr : tool_responses) {
            recorded_turns.push_back("tool: " + tr.name + " -> " + tr.result.dump());
        }

        if (on_continue_turn)
            return on_continue_turn(tool_responses);

        if (canned_turn_index < canned_turns.size()) {
            auto turn = canned_turns[canned_turn_index++];
            if (callbacks.on_text && !turn.text.empty())
                callbacks.on_text(turn.text);
            return turn;
        }

        if (callbacks.on_text)
            callbacks.on_text("Final mock response after tool");
        return {.ok = true, .text = "Final mock response after tool"};
    }

    std::string last_api_key;
    std::string last_model;
    std::string last_system_instruction;
    std::string last_user_prompt;
    std::vector<arn::core::ToolDefinition> last_tool_definitions;
    std::vector<arn::core::ToolResponse> last_tool_responses;
    std::vector<std::string> recorded_turns;
    std::vector<arn::core::ModelTurn> canned_turns;
    std::size_t canned_turn_index{0};
    std::size_t history_trimmed_to{0};
    bool was_cancelled{false};
    bool was_reset{false};
    std::function<arn::core::ModelTurn()> on_start_turn;
    std::function<arn::core::ModelTurn(const std::vector<arn::core::ToolResponse>&)> on_continue_turn;
};

} // namespace

int main() {
    try {
        std::cout << "Starting AgentSession tests...\n";

        // 1. Text-only request
        {
            auto mock = std::make_shared<MockModelProvider>();
            arn::core::AgentSession session;
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");
            session.select_model("mock-model-1");

            std::string streamed;
            auto res = session.prompt("What is the capital of France?",
                                      [&](std::string_view delta) { streamed += delta; });
            check(res.ok, "Text prompt failed");
            check(res.message == "Default mock response", "Text response content");
            check(streamed == "Default mock response", "Streamed text delta");
            check(mock->last_user_prompt == "What is the capital of France?", "User prompt passed to provider");
            check(mock->last_api_key == "test-key", "API key passed to provider");
            check(mock->last_model == "mock-model-1", "Model passed to provider");
        }

        // 2. System instruction propagation
        {
            auto mock = std::make_shared<MockModelProvider>();
            arn::core::AgentConfig config{.system_instruction = "Be concise and clear."};
            arn::core::AgentSession session(config);
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");

            check(session.system_instruction() == "Be concise and clear.", "Initial instruction getter");
            check(session.prompt("Hello").ok, "Prompt Hello");
            check(mock->last_system_instruction == "Be concise and clear.", "System instruction propagated");

            session.set_system_instruction("Be elaborate.");
            check(session.system_instruction() == "Be elaborate.", "Updated instruction getter");
            check(session.prompt("Hi again").ok, "Prompt Hi again");
            check(mock->last_system_instruction == "Be elaborate.", "Updated instruction propagated");
        }

        // 3. Conversation / history propagation and trimming
        {
            auto mock = std::make_shared<MockModelProvider>();
            arn::core::AgentConfig config{.max_history_entries = 10};
            arn::core::AgentSession session(config);
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");

            check(session.prompt("Turn 1").ok, "Prompt Turn 1");
            check(session.prompt("Turn 2").ok, "Prompt Turn 2");
            check(session.session_entries() == 2, "Session entries count");
            check(mock->history_trimmed_to == 10, "History trimming limit enforced");

            session.reset_session();
            check(session.session_entries() == 0, "Session entries after reset");
            check(mock->was_reset, "Provider was reset");
        }

        // 4. Single tool call execution and tool result returned to model
        {
            auto mock = std::make_shared<MockModelProvider>();
            auto tools = std::make_shared<arn::core::ToolRegistry>();
            tools->register_tool(std::make_shared<CalcTool>());

            arn::core::AgentSession session;
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");
            session.set_tools(tools);

            mock->canned_turns = {
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Let me calculate that.",
                    .tool_calls = {
                        arn::core::ToolCall{.id = "call_1", .name = "calculate", .arguments = {{"a", 10}, {"b", 32}}}
                    }
                },
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "The sum is 42."
                }
            };

            std::string progress_msg;
            auto res = session.prompt("What is 10 + 32?",
                                      arn::core::StreamCallbacks{
                                          .on_text = nullptr,
                                          .on_progress = [&](std::string_view msg) { progress_msg = msg; }
                                      });

            check(res.ok, "Tool prompt success");
            check(res.message == "The sum is 42.", "Final model text");
            check(progress_msg == "Running project tool: calculate", "Progress message emitted");
            check(mock->last_tool_responses.size() == 1, "One tool response sent to provider");
            check(mock->last_tool_responses[0].call_id == "call_1", "Tool call ID preserved");
            check(mock->last_tool_responses[0].name == "calculate", "Tool name preserved");
            check(mock->last_tool_responses[0].result.value("sum", 0.0) == 42.0, "Tool calculation result");
        }

        // 5. Multiple sequential tool calls (multi-round)
        {
            auto mock = std::make_shared<MockModelProvider>();
            auto tools = std::make_shared<arn::core::ToolRegistry>();
            tools->register_tool(std::make_shared<CalcTool>());

            arn::core::AgentSession session;
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");
            session.set_tools(tools);

            // Turn 1 calls calculate (1 + 2)
            // Turn 2 calls calculate (result + 5)
            // Turn 3 completes
            mock->canned_turns = {
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Step 1",
                    .tool_calls = {arn::core::ToolCall{.id = "c1", .name = "calculate", .arguments = {{"a", 1}, {"b", 2}}}}
                },
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Step 2",
                    .tool_calls = {arn::core::ToolCall{.id = "c2", .name = "calculate", .arguments = {{"a", 3}, {"b", 5}}}}
                },
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Final answer is 8."
                }
            };

            std::vector<std::string> progress_log;
            auto res = session.prompt("Multi step math",
                                      arn::core::StreamCallbacks{
                                          .on_text = nullptr,
                                          .on_progress = [&](std::string_view msg) { progress_log.push_back(std::string(msg)); }
                                      });

            check(res.ok, "Multi-round success");
            check(res.message == "Final answer is 8.", "Multi-round final text");
            check(progress_log.size() == 2, "Two progress events");
            check(mock->recorded_turns.size() == 3, "User prompt + 2 tool results recorded");
        }

        // 6. Unknown tool behavior
        {
            auto mock = std::make_shared<MockModelProvider>();
            arn::core::AgentSession session;
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");

            mock->canned_turns = {
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Trying unknown tool",
                    .tool_calls = {arn::core::ToolCall{.id = "u1", .name = "fly_to_moon", .arguments = {}}}
                },
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Sorry, that tool does not exist."
                }
            };

            auto res = session.prompt("Fly me to the moon");
            check(res.ok, "Unknown tool handled gracefully");
            check(res.message == "Sorry, that tool does not exist.", "Unknown tool model explanation");
            check(mock->last_tool_responses.size() == 1, "Tool response returned for unknown tool");
            check(mock->last_tool_responses[0].result.contains("error"), "Error reported in tool response");
        }

        // 7. Rejected confirmation
        {
            auto mock = std::make_shared<MockModelProvider>();
            auto tools = std::make_shared<arn::core::ToolRegistry>();
            tools->register_tool(std::make_shared<ProtectedTool>());

            arn::core::AgentSession session;
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");
            session.set_tools(tools);
            session.set_confirmation_handler([](const arn::core::ConfirmationRequest&) {
                return false; // Deny approval
            });

            mock->canned_turns = {
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "Executing sensitive action",
                    .tool_calls = {arn::core::ToolCall{.id = "p1", .name = "dangerous_op", .arguments = {}}}
                },
                arn::core::ModelTurn{
                    .ok = true,
                    .text = "The user rejected the action."
                }
            };

            auto res = session.prompt("Run dangerous op");
            check(res.ok, "Declined confirmation handled gracefully");
            check(res.message == "The user rejected the action.", "Declined action model explanation");
            check(mock->last_tool_responses.size() == 1, "Tool response sent back");
            check(mock->last_tool_responses[0].result.value("error", "") == "Operation declined by user.",
                  "Decline error recorded in tool response");
        }

        // 8. Cancellation
        {
            auto mock = std::make_shared<MockModelProvider>();
            arn::core::AgentSession session;
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");

            std::atomic_bool cancel_flag{true};
            auto res = session.prompt("This should be cancelled immediately", &cancel_flag);
            check(!res.ok && res.cancelled, "Pre-cancelled prompt returned cancelled status");

            session.cancel_active_request();
            check(mock->was_cancelled, "cancel_active_request forwarded to provider");
        }

        // 9. Maximum tool-round protection
        {
            auto mock = std::make_shared<MockModelProvider>();
            auto tools = std::make_shared<arn::core::ToolRegistry>();
            tools->register_tool(std::make_shared<CalcTool>());

            arn::core::AgentConfig config{.max_tool_rounds = 4};
            arn::core::AgentSession session(config);
            check(session.configure_provider(mock, "test-key").ok, "Configure provider");
            session.set_tools(tools);

            // Provider that loops infinitely calling calculate
            mock->on_start_turn = []() {
                return arn::core::ModelTurn{
                    .ok = true,
                    .text = "Infinite loop start",
                    .tool_calls = {arn::core::ToolCall{.id = "inf", .name = "calculate", .arguments = {{"a", 1}, {"b", 1}}}}
                };
            };
            mock->on_continue_turn = [](const std::vector<arn::core::ToolResponse>&) {
                return arn::core::ModelTurn{
                    .ok = true,
                    .text = "Infinite loop continue",
                    .tool_calls = {arn::core::ToolCall{.id = "inf", .name = "calculate", .arguments = {{"a", 1}, {"b", 1}}}}
                };
            };

            auto res = session.prompt("Loop forever");
            check(!res.ok, "Infinite tool loop was aborted");
            check(res.message == "Stopped after too many tool calls.", "Max tool rounds error message");
        }

        // 10. Provider independence with Mock
        {
            auto mock = std::make_shared<MockModelProvider>();
            arn::core::AgentSession session;
            auto res = session.configure_provider(mock, "mock-api-key");
            check(res.ok, "Mock provider configuration");
            check(session.active_provider() == arn::core::ProviderType::custom, "Active provider type");
            check(session.active_model() == "mock-model-1", "Auto-selected preferred model");
            check(session.available_models().size() == 2, "Available models populated");
            check(session.provider() == mock.get(), "Provider pointer accessor");
        }

        std::cout << "All AgentSession tests passed successfully.\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "AgentSession test failed: " << err.what() << '\n';
        return 1;
    }
}
