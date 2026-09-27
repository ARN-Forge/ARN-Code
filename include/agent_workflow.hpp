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
};

enum class AgentCommandStatus { usage, completed, failed, cancelled, needs_input, needs_confirmation };

struct AgentCommandResult {
    AgentCommandStatus status{AgentCommandStatus::failed};
    std::optional<::arn::core::OrchestrationResult> workflow;
};

/** Parses the user-facing `/agent <task>` command.
 * Returns nullopt for normal chat and other commands; an engaged empty string
 * represents `/agent` without a task so the caller can print usage.
 */
[[nodiscard]] std::optional<std::string>
parse_agent_command(std::string_view input);

/** Builds the standard registry and a fresh-provider AgentRuntime factory.
 * Credentials and the selected model are copied in memory only. Every factory
 * call creates an independent provider/runtime while reusing ARN Code's existing
 * workspace-scoped tool implementations.
 */
[[nodiscard]] std::unique_ptr<::arn::core::AgentOrchestrator>
create_agent_orchestrator(AgentWorkflowConfig config);

/** Runs the `/agent` command body through an existing orchestrator.
 * Empty task text emits usage and does not execute a workflow.
 */
[[nodiscard]] AgentCommandResult
run_agent_command(std::string task,
                  const std::filesystem::path& project_root,
                  ::arn::core::AgentOrchestrator& orchestrator,
                  const AgentCommandCallbacks& callbacks = {},
                  const ::arn::core::ConfirmationFn& confirm = {});

} // namespace arn
