#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace arn::core {

struct ConfirmationRequest {
    std::string name;
    nlohmann::json arguments;
    std::string summary;
    bool changes_state{false};
    bool changes_files{false}; // Backward compatibility alias with ARN ToolRequest
    nlohmann::json preview;

    ConfirmationRequest() = default;

    ConfirmationRequest(std::string name_in,
                        nlohmann::json args_in,
                        std::string summary_in = {},
                        bool changes_state_in = false,
                        bool changes_files_in = false,
                        nlohmann::json preview_in = {})
        : name(std::move(name_in)),
          arguments(std::move(args_in)),
          summary(std::move(summary_in)),
          changes_state(changes_state_in),
          changes_files(changes_files_in || changes_state_in),
          preview(std::move(preview_in)) {}

    [[nodiscard]] const std::string& tool_name() const noexcept { return name; }
};

class IConfirmationHandler {
public:
    virtual ~IConfirmationHandler() = default;
    [[nodiscard]] virtual bool confirm(const ConfirmationRequest& request) = 0;
};

using ConfirmationFn = std::function<bool(const ConfirmationRequest&)>;

inline ConfirmationFn make_confirmation_fn(IConfirmationHandler& handler) {
    return [&handler](const ConfirmationRequest& request) {
        return handler.confirm(request);
    };
}

} // namespace arn::core
