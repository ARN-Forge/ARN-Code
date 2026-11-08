#pragma once

#include "terminal.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arn {

enum class SecretInputState { editing, submitted, cancelled };

enum class ApiKeyCommand { gemini, deepseek, openrouter, omniroute };

struct ParsedApiKeyCommand {
    ApiKeyCommand command;
    bool has_inline_value{};
};

class SecretInputBuffer {
public:
    void consume(const TerminalEvent& event);

    [[nodiscard]] SecretInputState state() const noexcept { return state_; }
    [[nodiscard]] std::string masked() const;
    [[nodiscard]] std::optional<std::string> take_submitted_secret();

private:
    std::string secret_;
    std::size_t characters_{};
    SecretInputState state_{SecretInputState::editing};
};

[[nodiscard]] std::optional<ParsedApiKeyCommand>
parse_api_key_command(std::string_view input);
[[nodiscard]] std::string safe_command_echo(std::string_view input);
// Live editor shows masked arguments; transcript echo omits them entirely.
[[nodiscard]] std::string safe_command_display(std::string_view input);
// Raw console records have no reliable paste boundary. After a credential
// interaction, discard all input through an explicit Esc/Ctrl+C reset. Never
// infer a boundary from timing, Enter, queue exhaustion or ordinary text.
// discard_pending runs once, before show_prompt and the first read: an Esc or
// Ctrl+C received before the prompt cannot satisfy it, and one sent as soon
// as the prompt appears is never discarded with the stale input.
// Returns false on EOF; resize is the only optional rendering callback.
[[nodiscard]] bool wait_for_credential_input_reset(
    const std::function<TerminalEvent()>& read,
    const std::function<void()>& discard_pending,
    const std::function<void()>& show_prompt,
    const std::function<void()>& resize = {});
[[nodiscard]] std::vector<std::string> help_lines();

} // namespace arn
