#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include "arn/core/confirmation/confirmation_request.hpp"

namespace arn::core {

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json parameter_schema;
};

struct ToolContext {
    const std::atomic_bool* cancel_requested{nullptr};
    ConfirmationFn confirm;
};

struct ToolResult {
    bool ok{false};
    nlohmann::json result;
    std::string error_message;

    ToolResult() = default;
    ToolResult(bool ok_in, nlohmann::json result_in, std::string err = {})
        : ok(ok_in), result(std::move(result_in)), error_message(std::move(err)) {}

    static ToolResult success(nlohmann::json output) {
        return {true, std::move(output), {}};
    }
    static ToolResult failure(std::string message) {
        return {false, {{"error", message}}, message};
    }
};

class ITool {
public:
    virtual ~ITool() = default;

    [[nodiscard]] virtual const ToolDefinition& definition() const noexcept = 0;
    [[nodiscard]] virtual bool requires_confirmation() const noexcept { return false; }
    [[nodiscard]] virtual ToolResult execute(const nlohmann::json& arguments,
                                             const ToolContext& context) = 0;
};

} // namespace arn::core
