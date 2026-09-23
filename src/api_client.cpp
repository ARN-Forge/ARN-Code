#include "api_client.hpp"

#include <arn/core/provider/model_provider.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace arn {
namespace {

std::string agent_instruction() {
    return "You are ARN, a coding assistant. You may work only through the declared local tools. "
           "The tools are restricted to the folder in which ARN was started. Inspect relevant "
           "files before editing. "
           "Never claim a file changed unless a tool result confirms it. Never request deletion "
           "unless the user explicitly asks. "
           "Do not try to access secrets, .env files, or .git. Explain concisely what you "
           "completed.";
}

class ModelProviderAdapter final : public ModelProvider {
public:
    explicit ModelProviderAdapter(std::unique_ptr<::arn::core::IModelProvider> provider)
        : provider_(std::move(provider)) {}

    Provider kind() const noexcept override {
        return provider_ ? provider_->type() : Provider::none;
    }

    std::string_view name() const noexcept override {
        return provider_ ? provider_->name() : "";
    }

    std::string preferred_model(const std::vector<std::string>& models) const override {
        return provider_ ? provider_->preferred_model(models) : std::string{};
    }

    ApiResult list_models(const std::string& api_key,
                          const std::atomic_bool* cancel_requested) override {
        return provider_ ? provider_->list_models(api_key, cancel_requested)
                         : ApiResult{false, "Provider not initialized."};
    }

    ApiResult submit_prompt(const std::string& api_key, const std::string& model,
                            const std::string& prompt, const ToolExecutor& tools,
                            const ToolExecutor::ConfirmationFn& confirm,
                            const ProviderStreamCallback& on_text,
                            const std::atomic_bool* cancel_requested,
                            const ProviderStreamCallback& on_progress) override {
        if (!provider_)
            return {false, "Provider not initialized."};
        return provider_->submit_prompt(api_key, model, agent_instruction(), prompt,
                                        tools.registry(), confirm, on_text,
                                        cancel_requested, on_progress);
    }

    void cancel_active_request() override {
        if (provider_)
            provider_->cancel_active_request();
    }

    void reset_session() override {
        if (provider_)
            provider_->reset_session();
    }

    std::size_t session_entries() const noexcept override {
        return provider_ ? provider_->session_entries() : 0;
    }

private:
    std::unique_ptr<::arn::core::IModelProvider> provider_;
};

} // namespace

std::unique_ptr<ModelProvider> make_provider(Provider provider) {
    auto core_provider = ::arn::core::create_provider(provider);
    if (!core_provider)
        return nullptr;
    return std::make_unique<ModelProviderAdapter>(std::move(core_provider));
}

ApiClient::~ApiClient() = default;

ModelProvider* ApiClient::provider_for(Provider provider) {
    if (provider == Provider::none)
        return nullptr;
    auto& implementation = providers_[provider];
    if (!implementation)
        implementation = make_provider(provider);
    return implementation.get();
}

ApiResult ApiClient::list_models(Provider provider, const std::string& api_key,
                                 const std::atomic_bool* cancel_requested) {
    if (api_key.empty())
        return {false, "The API key cannot be empty."};
    if (provider == Provider::none)
        return {false, "Select a provider first."};
    auto* implementation = provider_for(provider);
    if (!implementation)
        return {false, "Select a provider first."};
    {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = implementation;
    }
    struct Cleanup {
        std::function<void()> action;
        ~Cleanup() {
            action();
        }
    } cleanup{[&] {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = nullptr;
    }};
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    return implementation->list_models(api_key, cancel_requested);
}

ApiResult ApiClient::submit_prompt(Provider provider, const std::string& api_key,
                                   const std::string& model, const std::string& prompt,
                                   const ToolExecutor& tools,
                                   const ToolExecutor::ConfirmationFn& confirm,
                                   const StreamCallback& on_text,
                                   const std::atomic_bool* cancel_requested,
                                   const StreamCallback& on_progress) {
    if (api_key.empty())
        return {false, "No API key is set."};
    if (model.empty())
        return {false, "No model is selected. Use /model <name>."};
    if (provider == Provider::none)
        return {false, "Select a provider first."};
    if (provider != session_provider_ || model != session_model_) {
        reset_session();
        session_provider_ = provider;
        session_model_ = model;
    }
    auto* implementation = provider_for(provider);
    if (!implementation)
        return {false, "Select a provider first."};
    {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = implementation;
    }

    struct Cleanup {
        std::function<void()> action;
        ~Cleanup() {
            action();
        }
    } cleanup{[&] {
        std::lock_guard lock(active_request_mutex_);
        active_request_provider_ = nullptr;
    }};
    return implementation->submit_prompt(api_key, model, prompt, tools, confirm, on_text,
                                         cancel_requested, on_progress);
}

void ApiClient::cancel_active_request() {
    std::lock_guard lock(active_request_mutex_);
    if (active_request_provider_)
        active_request_provider_->cancel_active_request();
}

void ApiClient::reset_session() {
    session_provider_ = Provider::none;
    session_model_.clear();
    for (auto& [provider, implementation] : providers_) {
        (void)provider;
        implementation->reset_session();
    }
}

std::size_t ApiClient::session_entries() const noexcept {
    if (session_provider_ == Provider::none)
        return 0;
    const auto implementation = providers_.find(session_provider_);
    return implementation == providers_.end() ? 0 : implementation->second->session_entries();
}

std::string ApiClient::preferred_model(Provider provider, const std::vector<std::string>& models) {
    const auto* implementation = provider_for(provider);
    return implementation ? implementation->preferred_model(models) : std::string{};
}

} // namespace arn
