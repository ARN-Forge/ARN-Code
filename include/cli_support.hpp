#pragma once

#include "terminal.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arn {

enum class SecretInputState { editing, submitted, cancelled };

enum class ApiKeyCommand { gemini, deepseek, openrouter };

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
[[nodiscard]] std::vector<std::string> help_lines();

} // namespace arn
