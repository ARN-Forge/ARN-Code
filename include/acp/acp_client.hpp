#pragma once

#include "acp/acp_subprocess.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace arn::acp {

// ACP protocol versions this client advertises as supported (newest first).
// v2 was finalised mid-2025 and is supported by current Kiro CLI.
inline constexpr std::uint32_t kLatestProtocolVersion = 2;

// Result of a single JSON-RPC request.
struct AcpResponse {
    bool ok = false;
    nlohmann::json result;   // present when ok == true
    nlohmann::json error;    // present when ok == false
};

// Capabilities advertised by the agent during initialization.
struct AcpAgentCapabilities {
    bool session_methods = false;       // session/* baseline (new, list, resume, close, prompt, cancel, update)
    bool load_session = false;          // session/load
    bool prompt_image = false;          // ContentBlock::Image
    bool prompt_audio = false;          // ContentBlock::Audio
    bool prompt_embedded_context = false;
    bool mcp_http = false;              // MCP servers via HTTP
    bool mcp_stdio = false;             // MCP servers via stdio
    std::vector<std::string> auth_methods;
};

// Permission option offered by the agent for a tool call.
struct AcpPermissionOption {
    std::string kind;       // "allow_once", "allow_always", "reject_once", "reject_always"
    std::string name;       // human-readable
    std::string option_id;  // opaque identifier passed back to the agent
};

// Tool call metadata emitted by the agent.
struct AcpToolCall {
    std::string tool_call_id;
    std::string title;
    std::string kind;       // "read", "edit", "delete", "move", "search", "execute", "think", "fetch", "switch_mode", "other"
    std::string status;     // "pending", "in_progress", "completed", "failed"
    nlohmann::json raw;     // full notification payload for advanced consumers
};

// Permission request issued by the agent.
struct AcpPermissionRequest {
    nlohmann::json request_id; // Original agent JSON-RPC ID, including string IDs.
    std::string session_id;
    std::string tool_call_id;
    std::string description;
    AcpToolCall tool_call;
    std::vector<AcpPermissionOption> options;
};

// Streaming callbacks emitted during a prompt turn.
struct AcpStreamCallbacks {
    std::function<void(std::string_view /*agent_message_chunk*/)> on_agent_text;
    std::function<void(std::string_view /*agent_thought_chunk*/)> on_agent_thought;
    std::function<void(const AcpToolCall&)> on_tool_call;
    std::function<void(const AcpToolCall&)> on_tool_call_update;
    std::function<void(const AcpPermissionRequest&)> on_permission_request;
    std::function<void()> on_turn_end;
    std::function<void(const std::string& /*status*/)> on_status_change;
};

// Per-session state. Created by initialize()/new_session().
struct AcpSessionState {
    std::string session_id;
    std::string working_directory;
    std::vector<std::string> available_models;   // populated lazily when discovered
    std::optional<std::string> current_model;

    [[nodiscard]] const std::vector<std::string>& get_models() const noexcept {
        return available_models;
    }
    [[nodiscard]] const std::optional<std::string>& get_model() const noexcept {
        return current_model;
    }
};

struct AcpInitInfo {
    std::string client_name = "arn-code";
    std::string client_title = "ARN Code";
    std::string client_version = "dev";
};

class AcpClient {
public:
    AcpClient();
    ~AcpClient();

    AcpClient(const AcpClient&) = delete;
    AcpClient& operator=(const AcpClient&) = delete;

    // Launch the ACP backend process. Throws std::runtime_error on failure.
    // After start() succeeds, the subprocess is running but no protocol
    // handshake has occurred yet.
    void start(const SubprocessSpec& spec, const AcpInitInfo& client_info);

    // Send initialize and exchange capabilities. Must succeed before
    // new_session() may be called. Returns the negotiated protocol version.
    std::uint32_t initialize(const std::vector<std::uint32_t>& supported_versions =
        {kLatestProtocolVersion, 1});

    // Tear down the subprocess and release resources. Idempotent.
    void shutdown();

    bool running() const noexcept { return running_.load(); }

    const AcpAgentCapabilities& capabilities() const noexcept { return capabilities_; }
    const std::string& agent_name() const noexcept { return agent_name_; }
    const std::string& agent_title() const noexcept { return agent_title_; }
    const std::string& agent_version() const noexcept { return agent_version_; }

    // Create a new session rooted at working_directory. The working directory
    // is the ARN Code project root, mirrored as the ACP session cwd so tool
    // activity scoped to the session respects the user's project.
    AcpSessionState new_session(const std::filesystem::path& working_directory);

    // Respond to the original agent request. Empty/invalid selections cancel;
    // only an exact advertised optionId can be sent as a selected outcome.
    AcpResponse request_permission(const std::string& session_id,
                                   const AcpPermissionRequest& request,
                                   const std::string& selected_option_id);

    // Send a prompt turn. Streaming updates are delivered through callbacks.
    // Returns the final response from the agent (which may include a stop
    // reason and structured error if any).
    AcpResponse prompt(const AcpSessionState& session,
                       std::string_view text,
                       const AcpStreamCallbacks& callbacks);

    // Best-effort cancellation. Sends session/cancel and tears down the
    // subprocess if it does not exit promptly.
    void cancel(const std::string& session_id);

    // Tell the agent to switch models mid-session. Returns ok=true if the
    // agent accepted the change.
    AcpResponse set_model(const AcpSessionState& session, std::string_view model_id);

    // Tell the agent to switch mode (e.g. agent configuration).
    AcpResponse set_mode(const AcpSessionState& session, std::string_view mode_id);

    // Discover models exposed by the backend, if any. Currently no ACP method
    // exists for this, so the Kiro adapter falls back to invoking the
    // backend's CLI directly (e.g. `kiro-cli chat --list-models -f json`).
    std::vector<std::string> list_models(const AcpSessionState& session);

private:
    AcpResponse send_request(const std::string& method, const nlohmann::json& params,
                             std::chrono::milliseconds timeout);
    void send_notification(const std::string& method, const nlohmann::json& params);
    void reader_loop();
    void fail_pending(const char* reason);
    void cancel_permissions(const std::string& session_id = {});
    bool write_permission_result(const nlohmann::json& id, const nlohmann::json& outcome);
    void dispatch_message(const nlohmann::json& message,
                          const AcpStreamCallbacks* active_callbacks);
    void handle_session_update(const nlohmann::json& update,
                               const AcpStreamCallbacks* callbacks);

    Subprocess subprocess_;
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_requested_{false};

    std::mutex write_mutex_;
    std::mutex shutdown_mutex_;
    std::mutex permission_mutex_;
    std::map<std::string, AcpPermissionRequest> permissions_;
    std::string line_buffer_;

    std::mutex pending_mutex_;
    std::int64_t next_request_id_ = 0;
    struct PendingRequest {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<AcpResponse> response;
        bool done = false;
    };
    std::map<std::int64_t, std::shared_ptr<PendingRequest>> pending_;

    AcpAgentCapabilities capabilities_;
    std::string agent_name_;
    std::string agent_title_;
    std::string agent_version_;
    std::uint32_t negotiated_version_ = 0;

    // Active session id when a prompt turn is in flight, so session/cancel
    // targets the right session. Multiple sessions per process are not yet
    // exercised by ARN; this is the simplest sound model.
    std::mutex active_session_mutex_;
    std::string active_session_;
    const AcpStreamCallbacks* active_callbacks_ = nullptr;
    std::map<std::string, AcpToolCall> active_tool_calls_;
};

} // namespace arn::acp
