#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "model_provider.hpp"
#include "tools/coding_tools.hpp"
#include <arn/core/agent/agent_session.hpp>

namespace arn {

// Backward-compatibility facade over arn::core::AgentSession for legacy callers
class ApiClient {
public:
    using StreamCallback = ProviderStreamCallback;
    ApiClient();
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    [[nodiscard]] ApiResult list_models(Provider provider, const std::string& api_key,
                                        const std::atomic_bool* cancel_requested = nullptr);
    [[nodiscard]] std::string preferred_model(Provider provider,
                                              const std::vector<std::string>& models);
    [[nodiscard]] ApiResult submit_prompt(Provider provider, const std::string& api_key,
                                          const std::string& model, const std::string& prompt,
                                          const ToolExecutor& tools,
                                          const ToolExecutor::ConfirmationFn& confirm,
                                          const StreamCallback& on_text = {},
                                          const std::atomic_bool* cancel_requested = nullptr,
                                          const StreamCallback& on_progress = {});
    // Safe to call from a separate thread while a request is running.
    void cancel_active_request();
    void reset_session();
    [[nodiscard]] std::size_t session_entries() const noexcept;

private:
    std::unique_ptr<::arn::core::AgentSession> session_;
    Provider session_provider_{Provider::none};
    std::string session_model_;
};

} // namespace arn
