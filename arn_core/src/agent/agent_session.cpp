#include "arn/core/agent/agent_session.hpp"

#include <mutex>
#include <utility>

namespace arn::core {

struct AgentSession::Impl {
    AgentConfig config;
    std::shared_ptr<const ToolRegistry> tools;
    ConfirmationFn confirm_handler;
    std::shared_ptr<IModelProvider> owned_provider;
    IModelProvider* provider{nullptr};
    std::string api_key;
    std::string active_model;
    std::vector<std::string> available_models;
    mutable std::mutex mutex;
    std::atomic<IModelProvider*> active_provider_{nullptr};

    explicit Impl(AgentConfig cfg) : config(std::move(cfg)) {}

    ApiResult configure_provider(std::shared_ptr<IModelProvider> prov,
                                 std::string key,
                                 const std::atomic_bool* cancel) {
        std::lock_guard lock(mutex);
        owned_provider = std::move(prov);
        provider = owned_provider.get();
        api_key = std::move(key);
        active_model.clear();
        available_models.clear();

        if (!provider)
            return {false, "Provider cannot be null."};

        auto result = provider->list_models(api_key, cancel);
        if (result.ok) {
            available_models = result.models;
            active_model = provider->preferred_model(available_models);
        }
        return result;
    }

    void set_provider(IModelProvider* prov, std::string key) {
        std::lock_guard lock(mutex);
        owned_provider.reset();
        provider = prov;
        api_key = std::move(key);
    }

    ApiResult prompt(std::string_view text,
                     const StreamCallbacks& callbacks,
                     const std::atomic_bool* cancel) {
        std::lock_guard lock(mutex);
        if (!provider)
            return {false, "Select a provider first."};
        if (api_key.empty())
            return {false, "No API key is set."};
        if (active_model.empty())
            return {false, "No model is selected."};
        if (cancel && cancel->load())
            return {false, "Request cancelled.", {}, true};

        // Enforce history trimming before new user turn
        provider->trim_history(config.max_history_entries);

        static const ToolRegistry empty_registry;
        const ToolRegistry& registry = tools ? *tools : empty_registry;

        active_provider_.store(provider, std::memory_order_release);
        struct ActiveGuard {
            std::atomic<IModelProvider*>& target;
            ~ActiveGuard() { target.store(nullptr, std::memory_order_release); }
        } guard{active_provider_};

        ModelTurn turn = provider->start_turn(
            api_key, active_model, config.system_instruction,
            std::string(text), registry, callbacks, cancel);

        if (cancel && cancel->load(std::memory_order_relaxed))
            return {false, "Request cancelled.", {}, true};
        if (!turn.ok)
            return {false, turn.error_message, {}, turn.cancelled};

        int round = 0;
        while (!turn.tool_calls.empty()) {
            if (++round > config.max_tool_rounds) {
                return {false, "Stopped after too many tool calls."};
            }

            std::vector<ToolResponse> responses;
            responses.reserve(turn.tool_calls.size());

            for (const auto& call : turn.tool_calls) {
                if (cancel && cancel->load())
                    return {false, "Request cancelled.", {}, true};

                if (callbacks.on_progress)
                    callbacks.on_progress("Running project tool: " + call.name);

                const ToolContext context{
                    .cancel_requested = cancel,
                    .confirm = confirm_handler
                };

                ToolResult execution = registry.execute(call.name, call.arguments, context);
                responses.push_back(ToolResponse{
                    .call_id = call.id,
                    .name = call.name,
                    .result = std::move(execution.result)
                });
            }

            if (cancel && cancel->load())
                return {false, "Request cancelled.", {}, true};

            turn = provider->continue_turn(
                api_key, active_model, config.system_instruction,
                responses, registry, callbacks, cancel);

            if (cancel && cancel->load(std::memory_order_relaxed))
                return {false, "Request cancelled.", {}, true};
            if (!turn.ok)
                return {false, turn.error_message, {}, turn.cancelled};
        }

        return {true, turn.text};
    }
};

AgentSession::AgentSession(AgentConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

AgentSession::~AgentSession() = default;

AgentSession::AgentSession(AgentSession&&) noexcept = default;
AgentSession& AgentSession::operator=(AgentSession&&) noexcept = default;

void AgentSession::set_system_instruction(std::string instruction) {
    std::lock_guard lock(impl_->mutex);
    impl_->config.system_instruction = std::move(instruction);
}

const std::string& AgentSession::system_instruction() const noexcept {
    return impl_->config.system_instruction;
}

void AgentSession::set_tools(std::shared_ptr<const ToolRegistry> tools) {
    std::lock_guard lock(impl_->mutex);
    impl_->tools = std::move(tools);
}

const ToolRegistry* AgentSession::tools() const noexcept {
    return impl_->tools.get();
}

void AgentSession::set_confirmation_handler(ConfirmationFn confirm_handler) {
    std::lock_guard lock(impl_->mutex);
    impl_->confirm_handler = std::move(confirm_handler);
}

const ConfirmationFn& AgentSession::confirmation_handler() const noexcept {
    return impl_->confirm_handler;
}

ApiResult AgentSession::configure_provider(std::shared_ptr<IModelProvider> provider,
                                           std::string api_key,
                                           const std::atomic_bool* cancel) {
    return impl_->configure_provider(std::move(provider), std::move(api_key), cancel);
}

ApiResult AgentSession::configure_provider(ProviderType type,
                                           const std::string& api_key,
                                           const std::atomic_bool* cancel) {
    auto prov = create_provider(type);
    if (!prov)
        return {false, "Select a provider first."};
    return configure_provider(std::move(prov), api_key, cancel);
}

void AgentSession::set_provider(IModelProvider* provider, std::string api_key) {
    impl_->set_provider(provider, std::move(api_key));
}

std::vector<std::string> AgentSession::available_models() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->available_models;
}

std::string AgentSession::preferred_model() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->provider ? impl_->provider->preferred_model(impl_->available_models) : std::string{};
}

void AgentSession::select_model(std::string model) {
    std::lock_guard lock(impl_->mutex);
    impl_->active_model = std::move(model);
}

std::string AgentSession::active_model() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->active_model;
}

ApiResult AgentSession::prompt(std::string_view text,
                               const StreamCallbacks& callbacks,
                               const std::atomic_bool* cancel) {
    return impl_->prompt(text, callbacks, cancel);
}

void AgentSession::cancel_active_request() {
    auto* active = impl_->active_provider_.load(std::memory_order_acquire);
    if (active) {
        active->cancel_active_request();
    } else {
        std::lock_guard lock(impl_->mutex);
        if (impl_->provider)
            impl_->provider->cancel_active_request();
    }
}

void AgentSession::reset_session() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->provider)
        impl_->provider->reset_session();
}

std::size_t AgentSession::session_entries() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->provider ? impl_->provider->session_entries() : 0;
}

ProviderType AgentSession::active_provider() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->provider ? impl_->provider->type() : ProviderType::none;
}

IModelProvider* AgentSession::provider() const noexcept {
    return impl_->provider;
}

const AgentConfig& AgentSession::config() const noexcept {
    return impl_->config;
}

} // namespace arn::core
