#include "arn/core/provider/model_provider.hpp"
#include "arn/core/agent/agent_session.hpp"

namespace arn::core {

ApiResult IModelProvider::submit_prompt(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction, const std::string& user_prompt,
    const ToolRegistry& tools, const ConfirmationFn& confirm,
    const TextStreamCallback& on_text,
    const std::atomic_bool* cancel_requested,
    const ProgressCallback& on_progress) {

    AgentConfig config{.system_instruction = system_instruction};
    AgentSession session(std::move(config));
    session.set_provider(this, api_key);
    session.select_model(model);
    session.set_tools(std::shared_ptr<const ToolRegistry>(&tools, [](const ToolRegistry*) {}));
    session.set_confirmation_handler(confirm);
    return session.prompt(user_prompt, StreamCallbacks{.on_text = on_text, .on_progress = on_progress}, cancel_requested);
}

} // namespace arn::core
