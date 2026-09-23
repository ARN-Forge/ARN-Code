#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "arn/core/confirmation/confirmation_request.hpp"
#include "arn/core/provider/model_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"

namespace arn::core {

struct AgentConfig {
    std::string system_instruction;
    int max_tool_rounds{12};
    std::size_t max_history_entries{40};
};

class AgentSession {
public:
    explicit AgentSession(AgentConfig config = {});
    ~AgentSession();

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;
    AgentSession(AgentSession&&) noexcept;
    AgentSession& operator=(AgentSession&&) noexcept;

    void set_system_instruction(std::string instruction);
    [[nodiscard]] const std::string& system_instruction() const noexcept;

    void set_tools(std::shared_ptr<const ToolRegistry> tools);
    [[nodiscard]] const ToolRegistry* tools() const noexcept;

    void set_confirmation_handler(ConfirmationFn confirm_handler);
    [[nodiscard]] const ConfirmationFn& confirmation_handler() const noexcept;

    // Provider configuration
    [[nodiscard]] ApiResult configure_provider(std::shared_ptr<IModelProvider> provider,
                                               std::string api_key,
                                               const std::atomic_bool* cancel = nullptr);

    [[nodiscard]] ApiResult configure_provider(ProviderType type,
                                               const std::string& api_key,
                                               const std::atomic_bool* cancel = nullptr);

    void set_provider(IModelProvider* provider, std::string api_key);

    [[nodiscard]] std::vector<std::string> available_models() const;
    [[nodiscard]] std::string preferred_model() const;
    void select_model(std::string model);
    [[nodiscard]] std::string active_model() const;

    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const StreamCallbacks& callbacks = {},
                                   const std::atomic_bool* cancel = nullptr);

    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const TextStreamCallback& on_text) {
        return prompt(text, StreamCallbacks{.on_text = on_text}, nullptr);
    }

    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const std::atomic_bool* cancel) {
        return prompt(text, StreamCallbacks{}, cancel);
    }

    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const TextStreamCallback& on_text,
                                   const std::atomic_bool* cancel,
                                   const ProgressCallback& on_progress) {
        return prompt(text, StreamCallbacks{.on_text = on_text, .on_progress = on_progress}, cancel);
    }

    void cancel_active_request();
    void reset_session();
    [[nodiscard]] std::size_t session_entries() const noexcept;
    [[nodiscard]] ProviderType active_provider() const noexcept;
    [[nodiscard]] IModelProvider* provider() const noexcept;
    [[nodiscard]] const AgentConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arn::core
