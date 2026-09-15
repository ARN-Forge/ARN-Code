#pragma once

#include <functional>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "tool_executor.hpp"

namespace arn {

enum class Provider { none, deepseek, gemini };

struct ApiResult {
    bool ok{};
    std::string message;
    std::vector<std::string> models;
    bool cancelled{};
};

class ApiClient {
public:
    using StreamCallback = std::function<void(std::string_view text)>;
    ApiClient() = default;
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    [[nodiscard]] ApiResult list_models(Provider provider, const std::string& api_key) const;
    [[nodiscard]] ApiResult submit_prompt(Provider provider, const std::string& api_key,
                                          const std::string& model, const std::string& prompt,
                                          const ToolExecutor& tools,
                                          const ToolExecutor::ConfirmationFn& confirm,
                                          const StreamCallback& on_text = {},
                                          const std::atomic_bool* cancel_requested = nullptr);
    // Safe to call from a separate thread while a request is running.
    void cancel_active_request();
    void reset_session();
    [[nodiscard]] std::size_t session_entries() const noexcept;

private:
    Provider session_provider_{Provider::none};
    std::string session_model_;
    nlohmann::json deepseek_messages_ = nlohmann::json::array();
    nlohmann::json gemini_contents_ = nlohmann::json::array();
    std::unique_ptr<httplib::Client> deepseek_client_;
    std::unique_ptr<httplib::Client> gemini_client_;
    std::mutex active_request_mutex_;
    httplib::Client* active_request_client_{};

    [[nodiscard]] httplib::Client& client_for(Provider provider);
};

[[nodiscard]] std::string provider_name(Provider provider);

} // namespace arn
