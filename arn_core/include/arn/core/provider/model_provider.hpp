#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include "arn/core/confirmation/confirmation_request.hpp"
#include "arn/core/tool/tool_registry.hpp"

namespace arn::core {

enum class ProviderType {
    none,
    deepseek,
    gemini,
    openrouter,
    custom
};

[[nodiscard]] inline std::string_view provider_type_name(ProviderType type) noexcept {
    switch (type) {
    case ProviderType::deepseek:
        return "DeepSeek";
    case ProviderType::gemini:
        return "Gemini";
    case ProviderType::openrouter:
        return "OpenRouter";
    case ProviderType::custom:
        return "Custom";
    case ProviderType::none:
        return "none";
    }
    return "unknown";
}

[[nodiscard]] inline ProviderType provider_type_from_name(std::string_view name) noexcept {
    if (name == "gemini")
        return ProviderType::gemini;
    if (name == "deepseek")
        return ProviderType::deepseek;
    if (name == "openrouter" || name == "open-router" || name == "open_router")
        return ProviderType::openrouter;
    if (name == "custom")
        return ProviderType::custom;
    return ProviderType::none;
}

struct ApiResult {
    bool ok{false};
    std::string message;
    std::vector<std::string> models;
    bool cancelled{false};
};

using TextStreamCallback = std::function<void(std::string_view text_delta)>;
using ProgressCallback = std::function<void(std::string_view progress_message)>;

struct StreamCallbacks {
    TextStreamCallback on_text;
    ProgressCallback on_progress;
};

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json arguments;
};

struct ToolResponse {
    std::string call_id;
    std::string name;
    nlohmann::json result;
};

struct ModelTurn {
    bool ok{false};
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::string error_message;
    bool cancelled{false};
};

struct ModelCapabilities {
    bool supports_tools{true};
    bool supports_streaming{true};
    bool supports_system_instruction{true};
};

class IModelProvider {
public:
    virtual ~IModelProvider() = default;

    [[nodiscard]] virtual ProviderType type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::string
    preferred_model(const std::vector<std::string>& models) const = 0;

    [[nodiscard]] virtual ModelCapabilities model_capabilities(std::string_view /*model*/) const {
        return ModelCapabilities{};
    }

    [[nodiscard]] virtual ApiResult list_models(const std::string& api_key,
                                                const std::atomic_bool* cancel_requested = nullptr) = 0;

    // Single-turn model generation step initiated by user input
    [[nodiscard]] virtual ModelTurn
    start_turn(const std::string& api_key, const std::string& model,
               const std::string& system_instruction, const std::string& user_prompt,
               const ToolRegistry& tools,
               const StreamCallbacks& callbacks = {},
               const std::atomic_bool* cancel_requested = nullptr) = 0;

    // Continuation of an active turn feeding back tool execution responses
    [[nodiscard]] virtual ModelTurn
    continue_turn(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction,
                  const std::vector<ToolResponse>& tool_responses,
                  const ToolRegistry& tools,
                  const StreamCallbacks& callbacks = {},
                  const std::atomic_bool* cancel_requested = nullptr) = 0;

    virtual void trim_history(std::size_t max_entries) = 0;
    virtual void cancel_active_request() = 0;
    virtual void reset_session() = 0;
    [[nodiscard]] virtual std::size_t session_entries() const noexcept = 0;

    // Convenience / backward compatibility: executes a full prompt turn loop via AgentSession
    [[nodiscard]] virtual ApiResult
    submit_prompt(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction, const std::string& user_prompt,
                  const ToolRegistry& tools, const ConfirmationFn& confirm,
                  const TextStreamCallback& on_text = {},
                  const std::atomic_bool* cancel_requested = nullptr,
                  const ProgressCallback& on_progress = {});

    [[nodiscard]] virtual ApiResult
    submit_prompt(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction, const std::string& user_prompt,
                  const ToolRegistry& tools, const ConfirmationFn& confirm,
                  const StreamCallbacks& callbacks,
                  const std::atomic_bool* cancel_requested = nullptr) {
        return submit_prompt(api_key, model, system_instruction, user_prompt, tools, confirm,
                             callbacks.on_text, cancel_requested, callbacks.on_progress);
    }

    // Backward compatibility helper matching legacy kind()
    [[nodiscard]] ProviderType kind() const noexcept { return type(); }
};

[[nodiscard]] std::unique_ptr<IModelProvider> create_provider(ProviderType type);

} // namespace arn::core
