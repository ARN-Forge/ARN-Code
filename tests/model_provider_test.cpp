#include "model_provider.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        check(arn::provider_from_name("gemini") == arn::Provider::gemini,
              "Gemini provider lookup failed");
        check(arn::provider_from_name("deepseek") == arn::Provider::deepseek,
              "DeepSeek provider lookup failed");
        check(arn::provider_from_name("openrouter") == arn::Provider::openrouter,
              "OpenRouter provider lookup failed");
        check(arn::provider_from_name("unknown") == arn::Provider::none,
              "Unknown provider was accepted");
        check(!arn::make_provider(arn::Provider::none), "None provider was constructed");

        auto gemini = arn::make_provider(arn::Provider::gemini);
        check(gemini && gemini->kind() == arn::Provider::gemini && gemini->name() == "Gemini",
              "Gemini provider metadata is invalid");
        check(gemini->preferred_model({"gemini-pro", "gemini-3.5-flash-lite"}) ==
                  "gemini-3.5-flash-lite",
              "Gemini default-model policy changed");
        check(gemini->preferred_model({"gemini-image", "gemini-custom-flash"}) ==
                  "gemini-custom-flash",
              "Gemini fallback model filtering changed");

        auto deepseek = arn::make_provider(arn::Provider::deepseek);
        check(deepseek && deepseek->kind() == arn::Provider::deepseek &&
                  deepseek->name() == "DeepSeek",
              "DeepSeek provider metadata is invalid");
        check(deepseek->preferred_model({"deepseek-reasoner", "deepseek-chat"}) == "deepseek-chat",
              "DeepSeek default-model policy changed");

        auto openrouter = arn::make_provider(arn::Provider::openrouter);
        check(openrouter && openrouter->kind() == arn::Provider::openrouter &&
                  openrouter->name() == "OpenRouter",
              "OpenRouter provider metadata is invalid");
        check(openrouter->preferred_model({"anthropic/claude-3.5-sonnet", "openai/gpt-4o"}) ==
                  "anthropic/claude-3.5-sonnet",
              "OpenRouter default-model policy changed");

        check(gemini->session_entries() == 0 && deepseek->session_entries() == 0 &&
                  openrouter->session_entries() == 0,
              "New providers must start without conversation state");

        std::cout << "Provider abstraction tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
