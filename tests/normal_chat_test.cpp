#include "normal_chat.hpp"

#include <arn/core/provider/model_provider.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class StreamingProvider final : public arn::core::IModelProvider {
public:
    arn::core::ProviderType type() const noexcept override {
        return arn::core::ProviderType::custom;
    }
    std::string_view name() const noexcept override { return "Streaming test"; }
    std::string preferred_model(const std::vector<std::string>& models) const override {
        return models.empty() ? std::string{} : models.front();
    }
    arn::core::ApiResult list_models(const std::string&,
                                     const std::atomic_bool*) override {
        return {true, {}, {"test-model"}};
    }
    arn::core::ModelTurn start_turn(
        const std::string&, const std::string&, const std::string&,
        const std::string& user_prompt, const arn::core::ToolRegistry&,
        const arn::core::StreamCallbacks& callbacks,
        const std::atomic_bool*) override {
        check(user_prompt == "Привіт", "Ukrainian input must reach the provider unchanged");
        ++entries;
        for (const std::string_view chunk : {"Вітаю", ", ", "друже!"}) {
            if (callbacks.on_text) callbacks.on_text(chunk);
        }
        ++entries;
        return {.ok = true, .text = "Вітаю, друже!"};
    }
    arn::core::ModelTurn continue_turn(
        const std::string&, const std::string&, const std::string&,
        const std::vector<arn::core::ToolResponse>&, const arn::core::ToolRegistry&,
        const arn::core::StreamCallbacks&, const std::atomic_bool*) override {
        return {.ok = false, .error_message = "Unexpected continuation"};
    }
    void trim_history(std::size_t) override {}
    void cancel_active_request() override {}
    void reset_session() override { entries = 0; }
    std::size_t session_entries() const noexcept override { return entries; }

private:
    std::size_t entries{};
};

} // namespace

int main() {
    StreamingProvider provider;
    arn::core::AgentSession session;
    session.set_provider(&provider, "fake-test-key");
    session.select_model("test-model");

    std::string rendered;
    int redraws = 0;
    const auto result = arn::run_normal_chat(
        session, "Привіт",
        {
            .append_text = [&](std::string_view chunk) { rendered += chunk; },
            .render = [&] { ++redraws; },
        });

    check(result.api.ok, "Normal chat should complete");
    check(result.api.message == "Вітаю, друже!", "Final UTF-8 response must survive");
    check(rendered == result.api.message, "Streamed and final responses must match");
    check(redraws == 3, "Every streamed chunk must trigger a redraw");
    check(result.context_active, "Context state must be sampled after prompt completion");
    return 0;
}
