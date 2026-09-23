#pragma once

#include <atomic>
#include <map>
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

struct OpenRouterConfig {
    std::string endpoint{"https://openrouter.ai"};
    std::string api_path_prefix{"/api/v1"};
    std::string http_referer; // Optional HTTP-Referer header
    std::string app_title;    // Optional X-Title header
};

class OpenRouterProvider final : public IModelProvider {
public:
    explicit OpenRouterProvider(OpenRouterConfig config = {});
    explicit OpenRouterProvider(std::string endpoint, std::string api_path_prefix = "/api/v1");
    ~OpenRouterProvider() override;

    OpenRouterProvider(const OpenRouterProvider&) = delete;
    OpenRouterProvider& operator=(const OpenRouterProvider&) = delete;
    OpenRouterProvider(OpenRouterProvider&&) noexcept;
    OpenRouterProvider& operator=(OpenRouterProvider&&) noexcept;

    [[nodiscard]] ProviderType type() const noexcept override {
        return ProviderType::openrouter;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "OpenRouter";
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

    [[nodiscard]] ModelCapabilities model_capabilities(std::string_view model) const override;

    void set_config(OpenRouterConfig config);
    [[nodiscard]] const OpenRouterConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] ModelTurn
    execute_turn_request(const std::string& api_key, const std::string& model,
                         const ToolRegistry& tools,
                         const StreamCallbacks& callbacks,
                         const std::atomic_bool* cancel_requested);

    void init_client();

    OpenRouterConfig config_;
    std::unique_ptr<httplib::Client> client_;
    nlohmann::json messages_ = nlohmann::json::array();
    std::map<std::string, ModelCapabilities, std::less<>> model_capabilities_;
};

} // namespace arn::core
