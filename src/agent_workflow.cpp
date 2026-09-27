#include "agent_workflow.hpp"

#include "tools/coding_tools.hpp"

#include <arn/core/agent/agent_registry.hpp>
#include <arn/core/agent/agent_runtime.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace arn {
namespace {

void emit(const AgentOutputSink& output, AgentOutputLevel level, std::string text) {
    if (output) output(level, std::move(text));
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string stage_name(std::string_view profile_id) {
    if (profile_id.empty()) return "Agent";
    std::string result(profile_id);
    result.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())));
    return result;
}

std::string stage_activity(std::string_view profile_id) {
    if (profile_id == "explorer") return "Inspecting project...";
    if (profile_id == "planner") return "Creating implementation plan...";
    if (profile_id == "coder") return "Applying changes...";
    if (profile_id == "reviewer") return "Reviewing implementation...";
    return "Working...";
}

AgentCommandStatus command_status(::arn::core::OrchestrationStatus status) {
    using Status = ::arn::core::OrchestrationStatus;
    switch (status) {
    case Status::completed: return AgentCommandStatus::completed;
    case Status::failed: return AgentCommandStatus::failed;
    case Status::cancelled: return AgentCommandStatus::cancelled;
    case Status::needs_input: return AgentCommandStatus::needs_input;
    case Status::needs_confirmation: return AgentCommandStatus::needs_confirmation;
    }
    return AgentCommandStatus::failed;
}

} // namespace

std::optional<std::string> parse_agent_command(std::string_view input) {
    const auto separator = input.find_first_of(" \t");
    const auto command = lower_ascii(std::string(input.substr(0, separator)));
    if (command != "/agent") return std::nullopt;
    if (separator == std::string_view::npos) return std::string{};
    return trim(std::string(input.substr(separator + 1)));
}

std::unique_ptr<::arn::core::AgentOrchestrator>
create_agent_orchestrator(AgentWorkflowConfig config) {
    auto tools = create_agent_tool_bindings(config.project_root, std::move(config.on_change));
    auto factory = [provider = config.provider,
                    api_key = std::move(config.api_key),
                    model = std::move(config.model),
                    tools = std::move(tools)](const ::arn::core::AgentProfile&)
        -> std::unique_ptr<::arn::core::IAgentRuntime> {
        return std::make_unique<::arn::core::AgentRuntime>(
            make_provider(provider), api_key, model, tools);
    };
    return std::make_unique<::arn::core::AgentOrchestrator>(
        ::arn::core::standard_agents::make_registry(), std::move(factory));
}

AgentCommandResult run_agent_command(std::string task,
                                     const std::filesystem::path& project_root,
                                     ::arn::core::AgentOrchestrator& orchestrator,
                                     const AgentCommandCallbacks& callbacks,
                                     const ::arn::core::ConfirmationFn& confirm) {
    task = trim(std::move(task));
    if (task.empty()) {
        emit(callbacks.output, AgentOutputLevel::normal, "Usage: /agent <task>");
        return {AgentCommandStatus::usage, std::nullopt};
    }

    emit(callbacks.output, AgentOutputLevel::normal, "[Agent] Starting multi-agent workflow");
    ::arn::core::OrchestrationTask orchestration_task;
    orchestration_task.id = "arn-code-agent";
    orchestration_task.objective = task;
    orchestration_task.working_directory = project_root;

    ::arn::core::OrchestrationCallbacks orchestration_callbacks;
    orchestration_callbacks.runtime = callbacks.runtime;
    std::string active_stage = "Agent";
    const auto downstream_progress = callbacks.runtime.on_progress;
    orchestration_callbacks.runtime.on_progress = [&](std::string_view message) {
        emit(callbacks.output, AgentOutputLevel::normal,
             "[" + active_stage + "] " + std::string(message));
        if (downstream_progress) downstream_progress(message);
    };
    orchestration_callbacks.on_stage_started = [&](std::string_view profile_id) {
        active_stage = stage_name(profile_id);
        emit(callbacks.output, AgentOutputLevel::normal,
             "[" + active_stage + "] " + stage_activity(profile_id));
    };
    orchestration_callbacks.on_stage_finished = [&](std::string_view profile_id,
                                                    const ::arn::core::AgentResult& result) {
        const auto level = result.status == ::arn::core::AgentStatus::completed
            ? AgentOutputLevel::good : AgentOutputLevel::warning;
        const auto state = result.status == ::arn::core::AgentStatus::completed
            ? "Completed" : "Stopped";
        emit(callbacks.output, level, "[" + stage_name(profile_id) + "] " + state);
    };

    auto workflow = orchestrator.execute(orchestration_task, orchestration_callbacks, confirm);
    const auto status = command_status(workflow.status);
    using Status = ::arn::core::OrchestrationStatus;
    switch (workflow.status) {
    case Status::completed:
        emit(callbacks.output, AgentOutputLevel::good, "[Agent] Workflow completed");
        if (!workflow.summary.empty())
            emit(callbacks.output, AgentOutputLevel::normal, workflow.summary);
        break;
    case Status::failed: {
        const auto stage = workflow.error && !workflow.error->profile_id.empty()
            ? " at " + stage_name(workflow.error->profile_id) : std::string{};
        emit(callbacks.output, AgentOutputLevel::error, "[Agent] Workflow failed" + stage);
        const auto message = workflow.error ? workflow.error->message : workflow.summary;
        if (!message.empty()) emit(callbacks.output, AgentOutputLevel::error, message);
        break;
    }
    case Status::cancelled:
        emit(callbacks.output, AgentOutputLevel::warning, "[Agent] Workflow cancelled");
        break;
    case Status::needs_input:
        emit(callbacks.output, AgentOutputLevel::warning, "[Agent] Workflow needs input");
        if (!workflow.summary.empty())
            emit(callbacks.output, AgentOutputLevel::warning, workflow.summary);
        break;
    case Status::needs_confirmation:
        emit(callbacks.output, AgentOutputLevel::warning, "[Agent] Workflow needs confirmation");
        if (!workflow.summary.empty())
            emit(callbacks.output, AgentOutputLevel::warning, workflow.summary);
        break;
    }
    return {status, std::move(workflow)};
}

} // namespace arn
