#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tool_executor.hpp"

namespace arn {

enum class Provider { none, deepseek, gemini };

struct ApiResult {
    bool ok{};
    std::string message;
    std::vector<std::string> models;
    bool cancelled{};
};

using ProviderStreamCallback = std::function<void(std::string_view text)>;

// Common boundary used by the rest of ARN. Implementations own all provider-specific
// authentication, HTTP formats, model discovery, streaming and conversation state.
class ModelProvider {
  public:
    virtual ~ModelProvider() = default;

    [[nodiscard]] virtual Provider kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::string
    preferred_model(const std::vector<std::string>& models) const = 0;
    [[nodiscard]] virtual ApiResult list_models(const std::string& api_key,
                                                const std::atomic_bool* cancel_requested) = 0;
    [[nodiscard]] virtual ApiResult
    submit_prompt(const std::string& api_key, const std::string& model, const std::string& prompt,
                  const ToolExecutor& tools, const ToolExecutor::ConfirmationFn& confirm,
                  const ProviderStreamCallback& on_text, const std::atomic_bool* cancel_requested,
                  const ProviderStreamCallback& on_progress) = 0;

    // May be called from another thread while list_models or submit_prompt is active.
    virtual void cancel_active_request() = 0;
    virtual void reset_session() = 0;
    [[nodiscard]] virtual std::size_t session_entries() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ModelProvider> make_provider(Provider provider);
[[nodiscard]] inline Provider provider_from_name(std::string_view name) {
    if (name == "gemini")
        return Provider::gemini;
    if (name == "deepseek")
        return Provider::deepseek;
    return Provider::none;
}

[[nodiscard]] inline std::string provider_name(Provider provider) {
    switch (provider) {
    case Provider::deepseek:
        return "DeepSeek";
    case Provider::gemini:
        return "Gemini";
    case Provider::none:
        return "none";
    }
    return "unknown";
}

} // namespace arn
