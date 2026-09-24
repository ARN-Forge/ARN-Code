// Test-only provider implementation. Never linked into the ARN application.
#include "model_provider.hpp"
#include "server.hpp"
#include <arn/core/provider/model_provider.hpp>
#include <chrono>
#include <thread>

namespace arn {

class FixtureModelProvider final : public ::arn::core::IModelProvider {
public:
    ::arn::core::ProviderType type() const noexcept override {
        return ::arn::core::ProviderType::gemini;
    }

    std::string_view name() const noexcept override {
        return "Fixture";
    }

    std::string preferred_model(const std::vector<std::string>&) const override {
        return "fixture-model";
    }

    ::arn::core::ApiResult list_models(const std::string&,
                                       const std::atomic_bool*) override {
        return {true, "", {"fixture-model"}};
    }

    ::arn::core::ModelTurn
    start_turn(const std::string&, const std::string&,
               const std::string&, const std::string& user_prompt,
               const ::arn::core::ToolRegistry&,
               const ::arn::core::StreamCallbacks& callbacks,
               const std::atomic_bool* cancel_requested) override {
        if (callbacks.on_progress)
            callbacks.on_progress("Waiting for provider response (attempt 1)");
        if (callbacks.on_text)
            callbacks.on_text("fixture-start");
        if (user_prompt == "wait") {
            while (cancel_requested && !cancel_requested->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return ::arn::core::ModelTurn{.ok = false, .error_message = "cancelled", .cancelled = true};
        }
        const auto cmd = nlohmann::json::parse(user_prompt);
        return ::arn::core::ModelTurn{
            .ok = true,
            .tool_calls = {
                ::arn::core::ToolCall{
                    .id = "call-1",
                    .name = cmd.at("name").get<std::string>(),
                    .arguments = cmd.at("arguments")
                }
            }
        };
    }

    ::arn::core::ModelTurn
    continue_turn(const std::string&, const std::string&,
                  const std::string&,
                  const std::vector<::arn::core::ToolResponse>& tool_responses,
                  const ::arn::core::ToolRegistry&,
                  const ::arn::core::StreamCallbacks& callbacks,
                  const std::atomic_bool*) override {
        if (callbacks.on_text) {
            for (const auto& resp : tool_responses) {
                callbacks.on_text(resp.result.dump());
            }
        }
        return ::arn::core::ModelTurn{.ok = true, .text = "done"};
    }

    void trim_history(std::size_t) override {}
    void cancel_active_request() override {}
    void reset_session() override {}
    std::size_t session_entries() const noexcept override { return 0; }
};

std::unique_ptr<::arn::core::IModelProvider> make_provider(Provider) {
    return std::make_unique<FixtureModelProvider>();
}

} // namespace arn

int main() {
    return arn::run_server();
}
