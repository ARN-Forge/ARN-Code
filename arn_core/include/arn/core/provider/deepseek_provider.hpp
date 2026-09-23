#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include "arn/core/provider/model_provider.hpp"

namespace httplib {
class Client;
}

namespace arn::core {

class DeepSeekProvider final : public IModelProvider {
public:
    explicit DeepSeekProvider(std::string endpoint = "https://api.deepseek.com");
    ~DeepSeekProvider() override;

    DeepSeekProvider(const DeepSeekProvider&) = delete;
    DeepSeekProvider& operator=(const DeepSeekProvider&) = delete;
    DeepSeekProvider(DeepSeekProvider&&) noexcept;
    DeepSeekProvider& operator=(DeepSeekProvider&&) noexcept;

    [[nodiscard]] ProviderType type() const noexcept override {
        return ProviderType::deepseek;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "DeepSeek";
    }

    [[nodiscard]] std::string
    preferred_model(const std::vector<std::string>& models) const override;

    [[nodiscard]] ApiResult
    list_models(const std::string& api_key,
                const std::atomic_bool* cancel_requested = nullptr) override;

    [[nodiscard]] ModelTurn
    start_turn(const std::string& api_key, const std::string& model,
               const std::string& system_instruction, const std::string& user_prompt,
               const ToolRegistry& tools,
               const StreamCallbacks& callbacks = {},
               const std::atomic_bool* cancel_requested = nullptr) override;

    [[nodiscard]] ModelTurn
    continue_turn(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction,
                  const std::vector<ToolResponse>& tool_responses,
                  const ToolRegistry& tools,
                  const StreamCallbacks& callbacks = {},
                  const std::atomic_bool* cancel_requested = nullptr) override;

    void trim_history(std::size_t max_entries) override;
    void cancel_active_request() override;
    void reset_session() override;
    [[nodiscard]] std::size_t session_entries() const noexcept override;

private:
    [[nodiscard]] ModelTurn
    execute_turn_request(const std::string& api_key, const std::string& model,
                         const ToolRegistry& tools,
                         const StreamCallbacks& callbacks,
                         const std::atomic_bool* cancel_requested);

    std::string endpoint_;
    std::unique_ptr<httplib::Client> client_;
    nlohmann::json messages_ = nlohmann::json::array();
};

} // namespace arn::core
