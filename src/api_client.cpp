#include "api_client.hpp"
#include "agent/coding_prompt.hpp"

namespace arn {

ApiClient::ApiClient()
    : session_(std::make_unique<::arn::core::AgentSession>()) {
    session_->set_system_instruction(coding_prompt());
}

ApiClient::~ApiClient() = default;

ApiResult ApiClient::list_models(Provider provider, const std::string& api_key,
                                 const std::atomic_bool* cancel_requested) {
    if (api_key.empty())
        return {false, "The API key cannot be empty."};
    if (provider == Provider::none)
        return {false, "Select a provider first."};
    auto prov = make_provider(provider);
    if (!prov)
        return {false, "Select a provider first."};
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    return prov->list_models(api_key, cancel_requested);
}

std::string ApiClient::preferred_model(Provider provider,
                                       const std::vector<std::string>& models) {
    auto prov = make_provider(provider);
    return prov ? prov->preferred_model(models) : std::string{};
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
        auto prov = make_provider(provider);
        auto config_res = session_->configure_provider(std::move(prov), api_key, cancel_requested);
        if (!config_res.ok)
            return config_res;
        session_->select_model(model);
    }

    session_->set_tools(tools.registry_ptr());
    session_->set_confirmation_handler(confirm);

    const ::arn::core::StreamCallbacks callbacks{
        .on_text = on_text,
        .on_progress = on_progress
    };

    return session_->prompt(prompt, callbacks, cancel_requested);
}

void ApiClient::cancel_active_request() {
    if (session_)
        session_->cancel_active_request();
}

void ApiClient::reset_session() {
    session_provider_ = Provider::none;
    session_model_.clear();
    if (session_)
        session_->reset_session();
}

std::size_t ApiClient::session_entries() const noexcept {
    return session_ ? session_->session_entries() : 0;
}

} // namespace arn
