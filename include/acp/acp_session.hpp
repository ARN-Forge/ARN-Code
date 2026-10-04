#pragma once

#include "acp/acp_client.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace arn::acp {

// Identifies an ACP backend ARN Code can route through.
enum class AcpBackendKind {
    none,
    kiro,
    subprocess,
};

struct AcpBackendInfo {
    AcpBackendKind kind = AcpBackendKind::none;
    std::string label;            // human-readable ("Kiro via kiro-cli")
    std::string binary_path;      // resolved executable
    std::optional<std::string> agent_name;
};

// High-level result of a chat turn routed through an ACP backend.
struct AcpChatResult {
    bool ok = false;
    bool cancelled = false;
    std::string message;          // user-facing summary
    std::vector<std::string> tool_titles;
};

// Preserve all server options and their opaque IDs. Return an advertised ID,
// or empty to cancel. A blocking gate must observe stopped (EOF/cancel/shutdown).
using AcpPermissionGate = std::function<std::string(
    const AcpPermissionRequest& request, const std::atomic_bool& stopped)>;

// Existing y/N UX selects only once options; never silently grants "always".
[[nodiscard]] std::string confirmation_option_id(const AcpPermissionRequest& request,
                                                 bool allow);

// Hook for streaming text fragments (printed into the terminal).
using AcpTextSink = std::function<void(std::string_view)>;

// Hook for tool-call notifications (printed into the terminal as bullet items).
using AcpToolSink = std::function<void(const std::string& title)>;

// Forward declaration so the header does not require the ACP client header.
class AcpClient;

// Owns an ACP client + session for the active chat backend.
class AcpSession {
public:
    AcpSession();
    ~AcpSession();
    AcpSession(const AcpSession&) = delete;
    AcpSession& operator=(const AcpSession&) = delete;

    // Activate an ACP backend. Returns empty string on success or a
    // user-facing error message when the backend is unavailable.
    std::string activate(const AcpBackendInfo& backend);
    // Explicit transport startup, also usable by deterministic fake fixtures.
    std::string activate(const SubprocessSpec& spec);

    // Drop the current backend and stop its subprocess.
    void deactivate();

    // Whether an ACP backend is currently active (vs. native ARN provider).
    [[nodiscard]] bool active() const noexcept {
        std::lock_guard lock(state_mutex_);
        return client_ != nullptr;
    }

    // The currently active backend, if any.
    [[nodiscard]] const AcpBackendInfo& backend() const noexcept { return backend_; }

    // Returns the available models discovered from the backend, if supported.
    [[nodiscard]] const std::vector<std::string>& available_models() const noexcept {
        return available_models_;
    }

    [[nodiscard]] const std::optional<std::string>& current_model() const noexcept {
        return current_model_;
    }

    // Set the model on the active backend. Returns true on success.
    bool set_model(std::string_view model_id, std::string& error);

    // Run a single chat turn through the ACP backend.
    AcpChatResult chat(std::string_view prompt,
                       const AcpTextSink& on_text,
                       const AcpToolSink& on_tool,
                       AcpPermissionGate permission_gate,
                       const std::atomic_bool* cancelled);

private:
    std::string activate_transport(const AcpBackendInfo& backend, const SubprocessSpec& spec);
    mutable std::mutex state_mutex_;
    std::condition_variable turn_finished_;
    AcpBackendInfo backend_;
    std::shared_ptr<AcpClient> client_;
    AcpSessionState session_state_{};
    std::vector<std::string> available_models_;
    std::optional<std::string> current_model_;
    std::atomic<bool> turn_in_flight_{false};
    std::thread::id turn_thread_;
    std::shared_ptr<std::atomic_bool> active_stop_;
};

} // namespace arn::acp
