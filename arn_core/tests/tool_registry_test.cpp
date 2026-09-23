#include "arn/core/tool/tool_registry.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class DummyEchoTool final : public arn::core::ITool {
public:
    explicit DummyEchoTool(std::string name)
        : def_{std::move(name), "Echo tool for testing", {{"type", "object"}, {"properties", {{"msg", {{"type", "string"}}}}}}} {}

    const arn::core::ToolDefinition& definition() const noexcept override {
        return def_;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext&) override {
        return arn::core::ToolResult::success({{"echo", arguments.value("msg", "")}});
    }

private:
    arn::core::ToolDefinition def_;
};

class DummyConfirmTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "sensitive_op", "Requires confirmation", {{"type", "object"}}};
        return def;
    }

    bool requires_confirmation() const noexcept override {
        return true;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext& context) override {
        if (!context.confirm)
            return arn::core::ToolResult::failure("Confirmation required but no handler provided");
        arn::core::ConfirmationRequest req{"sensitive_op", arguments, "Perform sensitive operation", true, true};
        if (!context.confirm(req))
            return arn::core::ToolResult::failure("Operation denied by user");
        return arn::core::ToolResult::success({{"performed", true}});
    }
};

} // namespace

int main() {
    try {
        arn::core::ToolRegistry registry;
        check(registry.empty(), "Registry should be empty initially");
        check(registry.size() == 0, "Registry size should be 0");

        // 1. Registration
        auto echo1 = std::make_shared<DummyEchoTool>("echo");
        check(registry.register_tool(echo1), "Should successfully register tool 'echo'");
        check(registry.size() == 1, "Registry size should be 1");
        check(!registry.empty(), "Registry should not be empty");

        // 2. Duplicate tool handling
        auto echo2 = std::make_shared<DummyEchoTool>("echo");
        check(!registry.register_tool(echo2), "Duplicate tool registration must return false");
        check(registry.size() == 1, "Registry size must still be 1 after duplicate registration attempt");

        // Null tool registration
        check(!registry.register_tool(nullptr), "Null tool registration must return false");

        // 3. Lookup
        check(registry.has_tool("echo"), "has_tool should return true for registered tool");
        check(!registry.has_tool("nonexistent"), "has_tool should return false for unknown tool");
        check(registry.find_tool("echo") == echo1, "find_tool should return the registered tool");
        check(registry.find_tool("nonexistent") == nullptr, "find_tool should return nullptr for unknown tool");

        // 4. Definitions / Schema exposure
        auto sensitive = std::make_shared<DummyConfirmTool>();
        check(registry.register_tool(sensitive), "Should register sensitive_op tool");
        check(registry.size() == 2, "Registry size should be 2");

        const auto defs = registry.definitions();
        check(defs.size() == 2, "definitions() should return 2 entries");

        const auto defs_json = registry.definitions_json();
        check(defs_json.is_array() && defs_json.size() == 2, "definitions_json should return JSON array with 2 entries");
        check(defs_json[0].contains("name") && defs_json[0].contains("parameters"), "JSON tool definition missing required fields");

        // 5. Execution: Success
        auto exec_result = registry.execute("echo", {{"msg", "hello arn"}});
        check(exec_result.ok, "Execution of echo should succeed");
        check(exec_result.result.value("echo", "") == "hello arn", "Echo output mismatch");

        // 6. Unknown tool execution
        auto unknown_result = registry.execute("unknown_tool", {});
        check(!unknown_result.ok, "Unknown tool execution should fail");
        check(unknown_result.result.value("error", "") == "Unknown tool requested: unknown_tool",
              "Unknown tool error message mismatch");

        // 7. Confirmation handling
        bool confirmation_asked = false;
        arn::core::ConfirmationFn deny_fn = [&](const arn::core::ConfirmationRequest& req) {
            confirmation_asked = true;
            check(req.name == "sensitive_op", "Confirmation request tool name mismatch");
            return false;
        };

        auto denied_result = registry.execute("sensitive_op", {}, arn::core::ToolContext{nullptr, deny_fn});
        check(!denied_result.ok, "Denied operation should fail");
        check(confirmation_asked, "Confirmation handler was not invoked");

        bool approved_asked = false;
        arn::core::ConfirmationFn approve_fn = [&](const arn::core::ConfirmationRequest&) {
            approved_asked = true;
            return true;
        };

        auto approved_result = registry.execute("sensitive_op", {}, arn::core::ToolContext{nullptr, approve_fn});
        check(approved_result.ok, "Approved operation should succeed");
        check(approved_asked, "Approve confirmation handler was not invoked");

        // 8. IConfirmationHandler interface & make_confirmation_fn adapter
        class CustomConfirmationHandler final : public arn::core::IConfirmationHandler {
        public:
            int invocations{0};
            bool approved_response{true};

            bool confirm(const arn::core::ConfirmationRequest& req) override {
                ++invocations;
                return approved_response && req.tool_name() == "sensitive_op";
            }
        };

        CustomConfirmationHandler custom_handler;
        auto handler_fn = arn::core::make_confirmation_fn(custom_handler);
        auto handler_result = registry.execute("sensitive_op", {}, arn::core::ToolContext{nullptr, handler_fn});
        check(handler_result.ok, "IConfirmationHandler-backed execution should succeed");
        check(custom_handler.invocations == 1, "IConfirmationHandler should be invoked exactly once");

        // 9. Unregister and clear
        check(registry.unregister_tool("echo"), "unregister_tool should return true");
        check(!registry.has_tool("echo"), "has_tool should be false after unregister");
        check(!registry.unregister_tool("echo"), "unregister_tool again should return false");

        registry.clear();
        check(registry.empty(), "Registry should be empty after clear()");

        std::cout << "All ToolRegistry tests passed successfully.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ToolRegistry test failed: " << ex.what() << '\n';
        return 1;
    }
}
