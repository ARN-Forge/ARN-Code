#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tool_executor.hpp"

namespace arn {

enum class Provider { none, deepseek, gemini };

struct ApiResult {
    bool ok{};
    std::string message;
    std::vector<std::string> models;
};

class ApiClient {
public:
    [[nodiscard]] ApiResult list_models(Provider provider, const std::string& api_key) const;
    [[nodiscard]] ApiResult submit_prompt(Provider provider, const std::string& api_key,
                                          const std::string& model, const std::string& prompt,
                                          const ToolExecutor& tools,
                                          const ToolExecutor::ConfirmationFn& confirm);
    void reset_session();
    [[nodiscard]] std::size_t session_entries() const noexcept;

private:
    Provider session_provider_{Provider::none};
    std::string session_model_;
    nlohmann::json deepseek_messages_ = nlohmann::json::array();
    nlohmann::json gemini_contents_ = nlohmann::json::array();
};

[[nodiscard]] std::string provider_name(Provider provider);

} // namespace arn
