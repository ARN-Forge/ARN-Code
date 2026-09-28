#pragma once

#include "model_provider.hpp"

#include <arn/core/agent/agent_orchestrator.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace arn {

struct AgentWorkflowConfig {
    Provider provider{Provider::none};
    std::string api_key;
    std::string model;
    std::filesystem::path project_root;
    std::function<void()> on_change;
};

enum class AgentOutputLevel { normal, good, warning, error };
using AgentOutputSink = std::function<void(AgentOutputLevel level, std::string text)>;

struct AgentCommandCallbacks {
    AgentOutputSink output;
    ::arn::core::StreamCallbacks runtime;
    std::function<::arn::core::ContinuationDecision(
        const ::arn::core::ContextArtifact& plan)> decide_plan;
};

enum class AgentCommandMode { workflow, automatic_workflow, direct, invalid };

struct ParsedAgentCommand {
    AgentCommandMode mode{AgentCommandMode::invalid};
    std::string profile_id;
    std::string task;
};

enum class AgentCommandStatus {
    usage,
    completed,
    failed,
    cancelled,
    declined,
    needs_input,
    needs_confirmation
};

struct AgentCommandResult {
    AgentCommandStatus status{AgentCommandStatus::failed};
    std::optional<::arn::core::OrchestrationResult> workflow;
    std::optional<::arn::core::AgentResult> direct;
};

/** Parses workflow, automatic workflow and exact standard-agent subcommands.
 * Returns nullopt for normal chat and other commands. Invalid/missing agent
 * arguments return an engaged command with mode == invalid.
 */
[[nodiscard]] std::optional<ParsedAgentCommand>
parse_agent_command(std::string_view input);

/** Builds the standard registry and a fresh-provider AgentRuntime factory.
 * Credentials and the selected model are copied in memory only. Every factory
 * call creates an independent provider/runtime while reusing ARN Code's existing
 * workspace-scoped tool implementations.
 */
[[nodiscard]] std::unique_ptr<::arn::core::AgentOrchestrator>
create_agent_orchestrator(AgentWorkflowConfig config);

/** Runs a parsed `/agent` command through the registry/runtime-backed executor.
 * Invalid input emits usage and performs no provider or tool work.
 */
[[nodiscard]] AgentCommandResult
run_agent_command(const ParsedAgentCommand& command,
                  const std::filesystem::path& project_root,
                  ::arn::core::AgentOrchestrator& orchestrator,
                  const AgentCommandCallbacks& callbacks = {},
                  const ::arn::core::ConfirmationFn& confirm = {});

} // namespace arn
