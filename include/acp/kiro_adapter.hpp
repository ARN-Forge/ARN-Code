#pragma once

#include "acp/acp_client.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace arn::acp {

// Result of probing for Kiro CLI availability.
enum class KiroStatus {
    ready,                // kiro-cli found and authenticated
    not_installed,        // kiro-cli not found on PATH (and ARN_KIRO_BIN not set)
    not_authenticated,    // kiro-cli exists but `kiro-cli whoami` failed
    internal_error,       // spawn failure
};

struct KiroAvailability {
    KiroStatus status = KiroStatus::not_installed;
    std::string resolved_path;          // path to kiro-cli when discovered
    std::string detail;                 // human-readable detail
};

// Discover the Kiro CLI binary. Honors the ARN_KIRO_BIN override, then PATH.
KiroAvailability probe_kiro_cli();

// Adapter that knows how to launch `kiro-cli acp` and discover its model
// catalogue (via `kiro-cli chat --list-models -f json`, since ACP does not
// define model discovery today).
class KiroAdapter {
public:
    // Resolve the executable using the same logic as probe_kiro_cli().
    static KiroAvailability resolve();

    // Construct a Kiro-specific subprocess spec for ACP mode.
    // The optional agent_name maps to `kiro-cli acp --agent <name>`.
    static SubprocessSpec make_spec(const std::filesystem::path& kiro_cli,
                                    const std::optional<std::string>& agent_name = std::nullopt,
                                    const std::optional<std::string>& model = std::nullopt);

    // Discover available models via the official CLI command. Returns an
    // empty vector when the backend cannot produce a listing.
    // Never consumes paid Kiro credits; this is a read-only metadata call.
    static std::vector<std::string> list_models(const std::filesystem::path& kiro_cli);

    // Returns the human-readable label for this agent.
    static std::string agent_label();
};

} // namespace arn::acp
