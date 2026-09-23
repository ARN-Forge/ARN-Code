#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tool_executor.hpp"
#include <arn/core/provider/model_provider.hpp>

namespace arn {

using Provider = ::arn::core::ProviderType;
using ApiResult = ::arn::core::ApiResult;
using ProviderStreamCallback = ::arn::core::TextStreamCallback;

// Common boundary used by ARN application code. Implementations adapt arn::core::IModelProvider.
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
    return ::arn::core::provider_type_from_name(name);
}

[[nodiscard]] inline std::string provider_name(Provider provider) {
    return std::string(::arn::core::provider_type_name(provider));
}

} // namespace arn
