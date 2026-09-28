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
    case Status::continuation_declined: return AgentCommandStatus::declined;
    case Status::needs_input: return AgentCommandStatus::needs_input;
    case Status::needs_confirmation: return AgentCommandStatus::needs_confirmation;
    }
    return AgentCommandStatus::failed;
}

AgentCommandStatus command_status(::arn::core::AgentStatus status) {
    using Status = ::arn::core::AgentStatus;
    switch (status) {
    case Status::completed: return AgentCommandStatus::completed;
    case Status::failed: return AgentCommandStatus::failed;
    case Status::cancelled: return AgentCommandStatus::cancelled;
    case Status::needs_input: return AgentCommandStatus::needs_input;
    case Status::needs_confirmation: return AgentCommandStatus::needs_confirmation;
    }
    return AgentCommandStatus::failed;
}

bool is_direct_profile(std::string_view value) {
    using namespace ::arn::core::standard_agents;
    return value == explorer_id || value == planner_id || value == coder_id
        || value == reviewer_id;
}

std::string usage() {
    return "Usage: /agent <task> | /agent --auto <task> | "
           "/agent <explorer|planner|coder|reviewer> <task>";
}

} // namespace

std::optional<ParsedAgentCommand> parse_agent_command(std::string_view input) {
    const auto separator = input.find_first_of(" \t");
    const auto command = lower_ascii(std::string(input.substr(0, separator)));
    if (command != "/agent") return std::nullopt;
    if (separator == std::string_view::npos)
        return ParsedAgentCommand{};

    auto remainder = trim(std::string(input.substr(separator + 1)));
    if (remainder.empty()) return ParsedAgentCommand{};
    const auto token_end = remainder.find_first_of(" \t");
    const auto first = lower_ascii(remainder.substr(0, token_end));
    const auto tail = token_end == std::string::npos
        ? std::string{} : trim(remainder.substr(token_end + 1));

    if (first == "--auto") {
        if (tail.empty()) return ParsedAgentCommand{};
        return ParsedAgentCommand{AgentCommandMode::automatic_workflow, {}, tail};
    }
    if (first.starts_with("--")) return ParsedAgentCommand{};
    if (is_direct_profile(first)) {
        if (tail.empty()) return ParsedAgentCommand{};
        return ParsedAgentCommand{AgentCommandMode::direct, first, tail};
    }
    return ParsedAgentCommand{AgentCommandMode::workflow, {}, std::move(remainder)};
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

AgentCommandResult run_agent_command(const ParsedAgentCommand& command,
                                     const std::filesystem::path& project_root,
                                     ::arn::core::AgentOrchestrator& orchestrator,
                                     const AgentCommandCallbacks& callbacks,
                                     const ::arn::core::ConfirmationFn& confirm) {
    if (command.mode == AgentCommandMode::invalid || trim(command.task).empty()) {
        emit(callbacks.output, AgentOutputLevel::normal, usage());
        return {AgentCommandStatus::usage, std::nullopt, std::nullopt};
    }

    if (command.mode == AgentCommandMode::direct) {
        const auto name = stage_name(command.profile_id);
        emit(callbacks.output, AgentOutputLevel::normal,
             "[" + name + "] " + stage_activity(command.profile_id));
        auto runtime_callbacks = callbacks.runtime;
        const auto downstream_progress = runtime_callbacks.on_progress;
        runtime_callbacks.on_progress = [&](std::string_view message) {
            emit(callbacks.output, AgentOutputLevel::normal,
                 "[" + name + "] " + std::string(message));
            if (downstream_progress) downstream_progress(message);
        };
        ::arn::core::AgentTask task;
        task.id = "arn-code-agent:" + command.profile_id;
        task.objective = command.task;
        task.working_directory = project_root;
        auto result = orchestrator.execute_agent(command.profile_id, task,
                                                 runtime_callbacks, confirm);
        const auto status = command_status(result.status);
        if (result.status == ::arn::core::AgentStatus::completed) {
            emit(callbacks.output, AgentOutputLevel::good, "[" + name + "] Completed");
            if (!result.summary.empty())
                emit(callbacks.output, AgentOutputLevel::normal, result.summary);
        } else if (result.status == ::arn::core::AgentStatus::cancelled) {
            emit(callbacks.output, AgentOutputLevel::warning, "[" + name + "] Cancelled");
        } else {
            emit(callbacks.output, AgentOutputLevel::error, "[" + name + "] Failed");
            const auto message = result.error ? result.error->message : result.summary;
            if (!message.empty()) emit(callbacks.output, AgentOutputLevel::error, message);
        }
        return {status, std::nullopt, std::move(result)};
    }

    emit(callbacks.output, AgentOutputLevel::normal, "[Agent] Starting multi-agent workflow");
    ::arn::core::OrchestrationTask orchestration_task;
    orchestration_task.id = "arn-code-agent";
    orchestration_task.objective = command.task;
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
    if (command.mode == AgentCommandMode::workflow) {
        orchestration_callbacks.before_next_stage = [&](std::string_view profile_id,
                                                        const ::arn::core::AgentResult&,
                                                        const ::arn::core::ContextArtifact& artifact) {
            if (profile_id != ::arn::core::standard_agents::planner_id)
                return ::arn::core::ContinuationDecision::proceed;
            emit(callbacks.output, AgentOutputLevel::normal, "Implementation plan:");
            emit(callbacks.output, AgentOutputLevel::normal, artifact.content);
            if (!callbacks.decide_plan)
                return ::arn::core::ContinuationDecision::decline;
            return callbacks.decide_plan(artifact);
        };
    }

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
    case Status::continuation_declined:
        emit(callbacks.output, AgentOutputLevel::warning,
             "[Agent] Workflow stopped: continuation declined");
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
    return {status, std::move(workflow), std::nullopt};
}

} // namespace arn
