#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "model_provider.hpp"

namespace arn {

class ApiClient {
  public:
    using StreamCallback = ProviderStreamCallback;
    ApiClient() = default;
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
    Provider session_provider_{Provider::none};
    std::string session_model_;
    std::map<Provider, std::unique_ptr<ModelProvider>> providers_;
    std::mutex active_request_mutex_;
    ModelProvider* active_request_provider_{};

    [[nodiscard]] ModelProvider* provider_for(Provider provider);
};

} // namespace arn
