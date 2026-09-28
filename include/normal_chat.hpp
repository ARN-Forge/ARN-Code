#pragma once

#include <arn/core/agent/agent_session.hpp>

#include <atomic>
#include <functional>
#include <string_view>

namespace arn {

struct NormalChatCallbacks {
    std::function<void(std::string_view)> append_text;
    std::function<void()> render;
};

struct NormalChatResult {
    ::arn::core::ApiResult api;
    bool context_active{};
};

/**
 * Runs normal chat without permitting streaming callbacks to query AgentSession.
 * AgentSession serializes prompt execution with its mutex, so session state is
 * sampled only after prompt() returns.
 */
[[nodiscard]] NormalChatResult
run_normal_chat(::arn::core::AgentSession& session,
                std::string_view input,
                const NormalChatCallbacks& callbacks,
                const std::atomic_bool* cancelled = nullptr);

} // namespace arn
