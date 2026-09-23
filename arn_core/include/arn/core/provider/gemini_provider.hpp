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

class GeminiProvider final : public IModelProvider {
public:
    explicit GeminiProvider(std::string endpoint = "https://generativelanguage.googleapis.com");
    ~GeminiProvider() override;

    GeminiProvider(const GeminiProvider&) = delete;
    GeminiProvider& operator=(const GeminiProvider&) = delete;
    GeminiProvider(GeminiProvider&&) noexcept;
    GeminiProvider& operator=(GeminiProvider&&) noexcept;

    [[nodiscard]] ProviderType type() const noexcept override {
        return ProviderType::gemini;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "Gemini";
    }

    [[nodiscard]] std::string
    preferred_model(const std::vector<std::string>& models) const override;

    [[nodiscard]] ApiResult
    list_models(const std::string& api_key,
                const std::atomic_bool* cancel_requested = nullptr) override;

    [[nodiscard]] ApiResult
    submit_prompt(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction, const std::string& user_prompt,
                  const ToolRegistry& tools, const ConfirmationFn& confirm,
                  const TextStreamCallback& on_text = {},
                  const std::atomic_bool* cancel_requested = nullptr,
                  const ProgressCallback& on_progress = {}) override;

    using IModelProvider::submit_prompt;

    void cancel_active_request() override;
    void reset_session() override;
    [[nodiscard]] std::size_t session_entries() const noexcept override;

private:
    std::string endpoint_;
    std::unique_ptr<httplib::Client> client_;
    nlohmann::json contents_ = nlohmann::json::array();
};

} // namespace arn::core
