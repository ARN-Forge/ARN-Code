#include "normal_chat.hpp"

#include <utility>

namespace arn {

NormalChatResult run_normal_chat(::arn::core::AgentSession& session,
                                 std::string_view input,
                                 const NormalChatCallbacks& callbacks,
                                 const std::atomic_bool* cancelled) {
    auto result = session.prompt(
        input,
        ::arn::core::StreamCallbacks{
            .on_text = [&](std::string_view text) {
                if (callbacks.append_text) callbacks.append_text(text);
                if (callbacks.render) callbacks.render();
            },
        },
        cancelled);
    // prompt() has released AgentSession's mutex at this point.
    const bool context_active = session.session_entries() != 0;
    return {std::move(result), context_active};
}

} // namespace arn
