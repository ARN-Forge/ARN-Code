#include "agent_workflow.hpp"

#include <arn/core/agent/agent_registry.hpp>

#include <algorithm>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace arn::core;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Script {
    AgentStatus status{AgentStatus::completed};
    std::string summary;
    std::optional<AgentError> error;
    bool block{false};
    bool request_confirmation{false};
    std::string progress;
};

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, Script, std::less<>> scripts;
    std::vector<std::string> created;
    std::vector<std::string> objectives;
    std::vector<std::filesystem::path> working_directories;
    std::vector<AgentContext> contexts;
    bool blocking{false};
    std::size_t cancellations{0};
    std::size_t confirmations{0};
};

class FakeRuntime final : public IAgentRuntime {
public:
    FakeRuntime(std::shared_ptr<State> state, std::string profile_id)
        : state_(std::move(state)), profile_id_(std::move(profile_id)) {}

    AgentResult execute(const AgentContext& context, const StreamCallbacks& callbacks,
                        const ConfirmationFn& confirm) override {
        Script script;
        {
            std::lock_guard lock(state_->mutex);
            state_->objectives.push_back(context.task.objective);
            state_->working_directories.push_back(context.task.working_directory);
            state_->contexts.push_back(context);
            script = state_->scripts.at(profile_id_);
            if (script.block) {
                state_->blocking = true;
                state_->cv.notify_all();
            }
        }
        if (script.block) {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return cancelled_; });
            return {AgentStatus::cancelled, "cancelled", {}, std::nullopt};
        }
        if (script.request_confirmation) {
            ConfirmationRequest request{"write_file", nlohmann::json::object(),
                                        "write test file", true};
            {
                std::lock_guard lock(state_->mutex);
                ++state_->confirmations;
            }
            if (!confirm || !confirm(request))
                return {AgentStatus::failed, "File change declined.", {},
                        AgentError{"permission_denied", "File change declined."}};
        }
        if (!script.progress.empty() && callbacks.on_progress)
            callbacks.on_progress(script.progress);
        return {script.status, script.summary, {}, script.error};
    }

    void cancel_active_request() override {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        {
            std::lock_guard lock(state_->mutex);
            ++state_->cancellations;
        }
        cv_.notify_all();
    }

private:
    std::shared_ptr<State> state_;
    std::string profile_id_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelled_{false};
};

void command_parsing_is_isolated() {
    check(!arn::parse_agent_command("normal chat"), "Normal chat is not an agent command");
    check(!arn::parse_agent_command("/status"), "Other commands remain unaffected");
    check(!arn::parse_agent_command("/agentic task"), "Command name must match exactly");
    const auto usage = arn::parse_agent_command("/agent");
    check(usage && usage->mode == arn::AgentCommandMode::invalid,
          "Bare /agent parses as invalid usage");
    const auto workflow = arn::parse_agent_command("/AGENT   inspect this project");
    check(workflow && workflow->mode == arn::AgentCommandMode::workflow
              && workflow->task == "inspect this project",
          "Normal workflow task is parsed and trimmed");
    const auto automatic = arn::parse_agent_command("/agent --auto implement this");
    check(automatic && automatic->mode == arn::AgentCommandMode::automatic_workflow
              && automatic->task == "implement this",
          "Auto workflow is explicit");
    for (const std::string id : {"explorer", "planner", "coder", "reviewer"}) {
        const auto direct = arn::parse_agent_command("/agent " + id + " do one thing");
        check(direct && direct->mode == arn::AgentCommandMode::direct
                  && direct->profile_id == id && direct->task == "do one thing",
              "Direct standard-agent command parses");
        const auto missing = arn::parse_agent_command("/agent " + id);
        check(missing && missing->mode == arn::AgentCommandMode::invalid,
              "Direct command requires a task");
    }
    check(arn::parse_agent_command("/agent --auto")->mode == arn::AgentCommandMode::invalid,
          "Auto mode requires a task");
    check(arn::parse_agent_command("/agent --unknown task")->mode == arn::AgentCommandMode::invalid,
          "Unknown option is invalid syntax");
    const auto ordinary = arn::parse_agent_command("/agent fix explorer output formatting");
    check(ordinary && ordinary->mode == arn::AgentCommandMode::workflow
              && ordinary->task == "fix explorer output formatting",
          "Ordinary first word is not treated as an agent name");
    const auto unknown_word = arn::parse_agent_command("/agent unknown something");
    check(unknown_word && unknown_word->mode == arn::AgentCommandMode::workflow,
          "Unrecognized words remain unambiguous workflow task text");
}

std::shared_ptr<State> default_state() {
    auto state = std::make_shared<State>();
    state->scripts.emplace("explorer", Script{AgentStatus::completed, "exploration"});
    state->scripts.emplace("planner", Script{AgentStatus::completed, "plan"});
    state->scripts.emplace("coder", Script{AgentStatus::completed, "changes"});
    state->scripts.emplace("reviewer", Script{AgentStatus::completed, "review"});
    return state;
}

std::unique_ptr<AgentOrchestrator> orchestrator_for(const std::shared_ptr<State>& state) {
    auto factory = [state](const AgentProfile& profile) -> std::unique_ptr<IAgentRuntime> {
        std::lock_guard lock(state->mutex);
        state->created.push_back(profile.id);
        return std::make_unique<FakeRuntime>(state, profile.id);
    };
    return std::make_unique<AgentOrchestrator>(standard_agents::make_registry(),
                                               std::move(factory));
}

struct CapturedOutput {
    std::vector<arn::AgentOutputLevel> levels;
    std::vector<std::string> lines;

    arn::AgentOutputSink sink() {
        return [&](arn::AgentOutputLevel level, std::string text) {
            levels.push_back(level);
            lines.push_back(std::move(text));
        };
    }
};

void usage_starts_no_workflow() {
    auto state = default_state();
    auto orchestrator = orchestrator_for(state);
    CapturedOutput output;
    const auto result = arn::run_agent_command({}, "virtual-workspace", *orchestrator,
                                                {.output = output.sink()});
    check(result.status == arn::AgentCommandStatus::usage && !result.workflow && !result.direct,
          "Empty /agent prints usage");
    check(output.lines == std::vector<std::string>{
              "Usage: /agent <task> | /agent --auto <task> | /agent <explorer|planner|coder|reviewer> <task>"},
          "Usage text is concise");
    check(state->created.empty() && state->objectives.empty(), "Usage starts no workflow");
}

void successful_workflow_output_and_task_forwarding() {
    auto state = default_state();
    state->scripts["planner"].progress = "Rate limited by provider; retrying in 0s (attempt 2/3)";
    auto orchestrator = orchestrator_for(state);
    CapturedOutput output;
    const std::string objective = "inspect this project and add a version command";
    const std::filesystem::path workspace = "virtual-workspace";
    const arn::ParsedAgentCommand command{arn::AgentCommandMode::workflow, {}, objective};
    std::string approved_plan;
    const auto result = arn::run_agent_command(
        command, workspace, *orchestrator,
        {.output = output.sink(),
         .decide_plan = [&](const ContextArtifact& plan) {
             approved_plan = plan.content;
             return ContinuationDecision::proceed;
         }});
    check(result.status == arn::AgentCommandStatus::completed && result.workflow,
          "Agent command completes");
    check(state->created == std::vector<std::string>{"explorer", "planner", "coder", "reviewer"},
          "Command runs orchestration path in fixed order");
    check(state->objectives == std::vector<std::string>(4, objective),
          "Original task reaches every stage unchanged");
    check(state->working_directories == std::vector<std::filesystem::path>(4, workspace),
          "Current working directory reaches every stage");
    check(approved_plan == "plan", "Real Planner artifact reaches approval callback");
    check(output.lines == std::vector<std::string>{
              "[Agent] Starting multi-agent workflow",
              "[Explorer] Inspecting project...", "[Explorer] Completed",
              "[Planner] Creating implementation plan...",
              "[Planner] Rate limited by provider; retrying in 0s (attempt 2/3)",
              "[Planner] Completed", "Implementation plan:", "plan",
              "[Coder] Applying changes...", "[Coder] Completed",
              "[Reviewer] Reviewing implementation...", "[Reviewer] Completed",
              "[Agent] Workflow completed", "review"},
          "Lifecycle and final output are ordered and readable");
}

void stage_failure_is_identified() {
    auto state = default_state();
    state->scripts["coder"] = {AgentStatus::failed, "compile failed",
                                AgentError{"compile_failed", "compile failed"}};
    auto orchestrator = orchestrator_for(state);
    CapturedOutput output;
    const arn::ParsedAgentCommand command{arn::AgentCommandMode::workflow, {}, "change code"};
    const auto result = arn::run_agent_command(
        command, "virtual-workspace", *orchestrator,
        {.output = output.sink(),
         .decide_plan = [](const ContextArtifact&) { return ContinuationDecision::proceed; }});
    check(result.status == arn::AgentCommandStatus::failed, "Failure status surfaced");
    check(state->created == std::vector<std::string>{"explorer", "planner", "coder"},
          "Failure prevents Reviewer");
    check(std::find(output.lines.begin(), output.lines.end(), "[Agent] Workflow failed at Coder")
              != output.lines.end(),
          "Failure output identifies Coder");
    check(std::find(output.lines.begin(), output.lines.end(), "compile failed")
              != output.lines.end(),
          "Failure output includes useful diagnostic");
}

void declined_confirmation_stays_declined() {
    auto state = default_state();
    state->scripts["coder"].request_confirmation = true;
    auto orchestrator = orchestrator_for(state);
    CapturedOutput output;
    std::size_t host_confirmations = 0;
    const auto result = arn::run_agent_command(
        {arn::AgentCommandMode::workflow, {}, "write a file"},
        "virtual-workspace", *orchestrator,
        {.output = output.sink(),
         .decide_plan = [](const ContextArtifact&) { return ContinuationDecision::proceed; }},
        [&](const ConfirmationRequest&) {
            ++host_confirmations;
            return false;
        });
    check(result.status == arn::AgentCommandStatus::failed, "Decline remains a failure");
    check(host_confirmations == 1 && state->confirmations == 1,
          "Existing host confirmation callback is forwarded exactly once");
    check(result.workflow && result.workflow->error
              && result.workflow->error->code == "permission_denied",
          "Decline is not converted into approval");
    check(state->created.size() == 3, "Decline prevents Reviewer");
}

void cancellation_returns_control() {
    auto state = default_state();
    state->scripts["explorer"].block = true;
    auto orchestrator = orchestrator_for(state);
    CapturedOutput output;
    auto future = std::async(std::launch::async, [&] {
        return arn::run_agent_command(
                                      {arn::AgentCommandMode::workflow, {}, "long task"},
                                      "virtual-workspace", *orchestrator,
                                      {.output = output.sink()});
    });
    {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->blocking; });
    }
    orchestrator->cancel_active_workflow();
    const auto result = future.get();
    check(result.status == arn::AgentCommandStatus::cancelled, "Cancellation returns cleanly");
    check(state->cancellations == 1 && state->created.size() == 1,
          "Cancellation reaches active runtime and prevents later stages");
    check(std::find(output.lines.begin(), output.lines.end(), "[Agent] Workflow cancelled")
              != output.lines.end(),
          "Cancellation output emitted");
}

void interactive_checkpoint_controls_pipeline() {
    {
        auto state = default_state();
        auto orchestrator = orchestrator_for(state);
        CapturedOutput output;
        std::string presented_plan;
        const auto result = arn::run_agent_command(
            {arn::AgentCommandMode::workflow, {}, "interactive task"},
            "virtual-workspace", *orchestrator,
            {.output = output.sink(),
             .decide_plan = [&](const ContextArtifact& plan) {
                 presented_plan = plan.content;
                 return ContinuationDecision::decline;
             }});
        check(result.status == arn::AgentCommandStatus::declined && result.workflow
                  && result.workflow->status == OrchestrationStatus::continuation_declined,
              "Plan decline has a dedicated command and workflow status");
        check(presented_plan == "plan", "Checkpoint displays the real Planner artifact");
        check(state->created == std::vector<std::string>{"explorer", "planner"},
              "Decline prevents Coder and Reviewer");
        check(result.workflow->executions.size() == 2,
              "Decline preserves Explorer and Planner records");
        check(std::find(output.lines.begin(), output.lines.end(), "Implementation plan:")
                  != output.lines.end()
                  && std::find(output.lines.begin(), output.lines.end(), "plan")
                         != output.lines.end(),
              "Interactive workflow prints the Planner artifact");
    }
    {
        auto state = default_state();
        auto orchestrator = orchestrator_for(state);
        const auto result = arn::run_agent_command(
            {arn::AgentCommandMode::workflow, {}, "cancel at plan"},
            "virtual-workspace", *orchestrator,
            {.decide_plan = [](const ContextArtifact&) { return ContinuationDecision::cancel; }});
        check(result.status == arn::AgentCommandStatus::cancelled,
              "Checkpoint cancellation remains distinct from decline");
        check(state->created == std::vector<std::string>{"explorer", "planner"},
              "Checkpoint cancellation prevents Coder and Reviewer");
    }
}

void auto_mode_runs_without_checkpoint() {
    auto state = default_state();
    auto orchestrator = orchestrator_for(state);
    std::size_t decisions = 0;
    const auto result = arn::run_agent_command(
        {arn::AgentCommandMode::automatic_workflow, {}, "automatic task"},
        "virtual-workspace", *orchestrator,
        {.decide_plan = [&](const ContextArtifact&) {
             ++decisions;
             return ContinuationDecision::decline;
         }});
    check(result.status == arn::AgentCommandStatus::completed,
          "Auto workflow completes");
    check(decisions == 0, "Auto workflow never asks for plan approval");
    check(state->created == std::vector<std::string>{"explorer", "planner", "coder", "reviewer"},
          "Auto workflow preserves all four stages in order");
}

void direct_agents_run_alone_with_registered_profiles() {
    for (const std::string id : {"explorer", "planner", "coder", "reviewer"}) {
        auto state = default_state();
        auto orchestrator = orchestrator_for(state);
        CapturedOutput output;
        const auto result = arn::run_agent_command(
            {arn::AgentCommandMode::direct, id, "direct task"},
            "virtual-workspace", *orchestrator, {.output = output.sink()});
        check(result.status == arn::AgentCommandStatus::completed && result.direct
                  && !result.workflow,
              "Direct execution returns AgentResult semantics");
        check(state->created == std::vector<std::string>{id}
                  && state->contexts.size() == 1
                  && state->contexts[0].profile.id == id,
              "Only the selected registered profile runs");
        const auto write_permission =
            state->contexts[0].profile.permissions.decision_for(OperationClass::write_file);
        check(write_permission == (id == "coder" ? PermissionDecision::ask_user
                                                  : PermissionDecision::deny),
              "Direct execution preserves standard profile write permissions");
    }

    auto state = default_state();
    state->scripts["coder"].request_confirmation = true;
    auto orchestrator = orchestrator_for(state);
    std::size_t confirmations = 0;
    const auto result = arn::run_agent_command(
        {arn::AgentCommandMode::direct, "coder", "write one file"},
        "virtual-workspace", *orchestrator, {},
        [&](const ConfirmationRequest&) {
            ++confirmations;
            return false;
        });
    check(result.status == arn::AgentCommandStatus::failed && result.direct
              && result.direct->error && result.direct->error->code == "permission_denied",
          "Direct Coder preserves confirmation denial");
    check(confirmations == 1 && state->created == std::vector<std::string>{"coder"},
          "Direct Coder requests confirmation without running other agents");
}

void direct_agent_cancellation_returns_control() {
    auto state = default_state();
    state->scripts["explorer"].block = true;
    auto orchestrator = orchestrator_for(state);
    auto future = std::async(std::launch::async, [&] {
        return arn::run_agent_command(
            {arn::AgentCommandMode::direct, "explorer", "long direct task"},
            "virtual-workspace", *orchestrator);
    });
    {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->blocking; });
    }
    orchestrator->cancel_active_execution();
    const auto result = future.get();
    check(result.status == arn::AgentCommandStatus::cancelled,
          "Direct agent cancellation returns cleanly");
    check(state->cancellations == 1 && state->created == std::vector<std::string>{"explorer"},
          "Direct cancellation reaches only the active runtime");
}

} // namespace

int main() {
    command_parsing_is_isolated();
    usage_starts_no_workflow();
    successful_workflow_output_and_task_forwarding();
    stage_failure_is_identified();
    declined_confirmation_stays_declined();
    cancellation_returns_control();
    interactive_checkpoint_controls_pipeline();
    auto_mode_runs_without_checkpoint();
    direct_agents_run_alone_with_registered_profiles();
    direct_agent_cancellation_returns_control();
    return 0;
}
