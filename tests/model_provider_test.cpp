#include "model_provider.hpp"
#include <arn/core/provider/omniroute_provider.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

struct OmniEnvironment {
    std::optional<std::string> primary, legacy;
    static void set(const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1); else unsetenv(name);
#endif
    }
    OmniEnvironment() {
        if (const auto* value = std::getenv("ARN_OMNIROUTE_BASE_URL")) primary = value;
        if (const auto* value = std::getenv("OMNIROUTE_BASE_URL")) legacy = value;
        set("ARN_OMNIROUTE_BASE_URL", nullptr);
        set("OMNIROUTE_BASE_URL", nullptr);
    }
    ~OmniEnvironment() {
        set("ARN_OMNIROUTE_BASE_URL", primary ? primary->c_str() : nullptr);
        set("OMNIROUTE_BASE_URL", legacy ? legacy->c_str() : nullptr);
    }
};

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        OmniEnvironment environment;
        check(arn::provider_from_name("gemini") == arn::Provider::gemini,
              "Gemini provider lookup failed");
        check(arn::provider_from_name("deepseek") == arn::Provider::deepseek,
              "DeepSeek provider lookup failed");
        check(arn::provider_from_name("openrouter") == arn::Provider::openrouter,
              "OpenRouter provider lookup failed");
        check(arn::provider_from_name("omniroute") == arn::Provider::omniroute,
              "OmniRoute provider lookup failed");
        check(arn::provider_from_name("omni-route") == arn::Provider::omniroute,
              "OmniRoute hyphenated provider lookup failed");
        check(arn::provider_from_name("omni_route") == arn::Provider::omniroute,
              "OmniRoute underscore provider lookup failed");
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

        auto omniroute = arn::make_provider(arn::Provider::omniroute);
        check(arn::provider_endpoint(*omniroute) == "http://localhost:20128/v1",
              "OmniRoute needs no environment override for standard local installation");
        check(omniroute && omniroute->kind() == arn::Provider::omniroute &&
                  omniroute->name() == "OmniRoute",
              "OmniRoute provider metadata is invalid");
        check(omniroute->preferred_model({"kiro", "other-model"}) == "kiro",
              "OmniRoute default-model policy changed");
        check(omniroute->preferred_model({"anthropic/claude-3.7-sonnet", "openai/gpt-4o"}) ==
                  "anthropic/claude-3.7-sonnet",
              "OmniRoute claude-3.7-sonnet preferred check");

        check(gemini->session_entries() == 0 && deepseek->session_entries() == 0 &&
                  openrouter->session_entries() == 0 && omniroute->session_entries() == 0,
              "New providers must start without conversation state");

#ifdef _WIN32
        _putenv("ARN_OMNIROUTE_BASE_URL=http://test-server:1234/api/v1");
#else
        setenv("ARN_OMNIROUTE_BASE_URL", "http://test-server:1234/api/v1", 1);
#endif
        auto omniroute_env = arn::make_provider(arn::Provider::omniroute);
        auto* omni_concrete = dynamic_cast<arn::core::OmniRouteProvider*>(omniroute_env.get());
        check(omni_concrete && omni_concrete->base_url() == "http://test-server:1234/api/v1",
              "OmniRoute provider respects ARN_OMNIROUTE_BASE_URL");
        check(arn::provider_endpoint(*omniroute_env) == "http://test-server:1234/api/v1",
              "Status endpoint must reflect the actual provider");
        OmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", "https://other.example/v1");
        check(arn::provider_endpoint(*omniroute_env) == "http://test-server:1234/api/v1",
              "Status must not reread the environment for an existing provider");
        OmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", "http://FAKE_SECRET@localhost/v1");
        auto invalid_omni = arn::make_provider(arn::Provider::omniroute);
        check(arn::provider_endpoint(*invalid_omni).empty(), "Invalid endpoint must not be displayed");
        const auto invalid_result = invalid_omni->list_models("FAKE_SECRET");
        check(!invalid_result.ok && invalid_result.message.find("FAKE_SECRET") == std::string::npos,
              "Malformed custom endpoint must fail safely without echoing credentials");
#ifdef _WIN32
        _putenv("ARN_OMNIROUTE_BASE_URL=");
#else
        unsetenv("ARN_OMNIROUTE_BASE_URL");
#endif

        std::cout << "Provider abstraction tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
