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
    check(usage && usage->empty(), "Bare /agent parses as an empty task");
    const auto task = arn::parse_agent_command("/AGENT   inspect this project");
    check(task && *task == "inspect this project", "Agent task is parsed and trimmed");
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
    const auto result = arn::run_agent_command("   ", "virtual-workspace", *orchestrator,
                                                {.output = output.sink()});
    check(result.status == arn::AgentCommandStatus::usage && !result.workflow,
          "Empty /agent prints usage");
    check(output.lines == std::vector<std::string>{"Usage: /agent <task>"},
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
    const auto result = arn::run_agent_command(objective, workspace, *orchestrator,
                                                {.output = output.sink()});
    check(result.status == arn::AgentCommandStatus::completed && result.workflow,
          "Agent command completes");
    check(state->created == std::vector<std::string>{"explorer", "planner", "coder", "reviewer"},
          "Command runs orchestration path in fixed order");
    check(state->objectives == std::vector<std::string>(4, objective),
          "Original task reaches every stage unchanged");
    check(state->working_directories == std::vector<std::filesystem::path>(4, workspace),
          "Current working directory reaches every stage");
    check(output.lines == std::vector<std::string>{
              "[Agent] Starting multi-agent workflow",
              "[Explorer] Inspecting project...", "[Explorer] Completed",
              "[Planner] Creating implementation plan...",
              "[Planner] Rate limited by provider; retrying in 0s (attempt 2/3)",
              "[Planner] Completed",
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
    const auto result = arn::run_agent_command("change code", "virtual-workspace", *orchestrator,
                                                {.output = output.sink()});
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
        "write a file", "virtual-workspace", *orchestrator, {.output = output.sink()},
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
        return arn::run_agent_command("long task", "virtual-workspace", *orchestrator,
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

} // namespace

int main() {
    command_parsing_is_isolated();
    usage_starts_no_workflow();
    successful_workflow_output_and_task_forwarding();
    stage_failure_is_identified();
    declined_confirmation_stays_declined();
    cancellation_returns_control();
    return 0;
}
