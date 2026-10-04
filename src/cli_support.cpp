#include "cli_support.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace arn {
namespace {

void erase_last_utf8_character(std::string& text) {
    if (text.empty()) return;
    auto position = text.size() - 1;
    while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80)
        --position;
    text.erase(position);
}

std::string lower_ascii(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

bool is_key_command(std::string_view command) {
    return command == "/key-gemini" || command == "/key-deepseek" ||
        command == "/key-openrouter" || command == "/key-omniroute";
}

std::string_view trim_ascii_whitespace(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

void SecretInputBuffer::consume(const TerminalEvent& event) {
    if (state_ != SecretInputState::editing) return;
    switch (event.type) {
    case TerminalEventType::character:
        if (!event.text.empty()) {
            secret_ += event.text;
            ++characters_;
        }
        break;
    case TerminalEventType::backspace:
        if (!secret_.empty()) {
            erase_last_utf8_character(secret_);
            --characters_;
        }
        break;
    case TerminalEventType::enter:
        state_ = SecretInputState::submitted;
        break;
    case TerminalEventType::escape:
    case TerminalEventType::interrupt:
    case TerminalEventType::end_of_input:
        secret_.clear();
        characters_ = 0;
        state_ = SecretInputState::cancelled;
        break;
    default:
        break;
    }
}

std::string SecretInputBuffer::masked() const {
    return std::string(characters_, '*');
}

std::optional<std::string> SecretInputBuffer::take_submitted_secret() {
    if (state_ != SecretInputState::submitted || secret_.empty()) return std::nullopt;
    characters_ = 0;
    return std::exchange(secret_, {});
}

std::optional<ParsedApiKeyCommand> parse_api_key_command(std::string_view input) {
    input = trim_ascii_whitespace(input);
    const auto separator = input.find_first_of(" \t\r\n");
    const auto command = lower_ascii(std::string(input.substr(0, separator)));
    if (!is_key_command(command)) return std::nullopt;

    const auto remainder = separator == std::string_view::npos
        ? std::string_view{} : trim_ascii_whitespace(input.substr(separator + 1));
    const auto kind = command == "/key-gemini" ? ApiKeyCommand::gemini
        : command == "/key-deepseek" ? ApiKeyCommand::deepseek
        : command == "/key-openrouter" ? ApiKeyCommand::openrouter
        : ApiKeyCommand::omniroute;
    return ParsedApiKeyCommand{kind, !remainder.empty()};
}

std::string safe_command_echo(std::string_view input) {
    const auto parsed = parse_api_key_command(input);
    if (!parsed) return std::string(input);
    switch (parsed->command) {
    case ApiKeyCommand::gemini: return "/key-gemini";
    case ApiKeyCommand::deepseek: return "/key-deepseek";
    case ApiKeyCommand::openrouter: return "/key-openrouter";
    case ApiKeyCommand::omniroute: return "/key-omniroute";
    }
    return {};
}

std::string safe_command_display(std::string_view input) {
    const auto parsed = parse_api_key_command(input);
    if (!parsed) return std::string(input);
    auto display = safe_command_echo(input);
    if (!parsed->has_inline_value) return display;
    input = trim_ascii_whitespace(input);
    const auto separator = input.find_first_of(" \t\r\n");
    const auto argument = input.substr(separator + 1);
    display += ' ';
    for (const unsigned char byte : argument) {
        if ((byte & 0xC0) != 0x80) display += '*';
    }
    return display;
}

std::vector<std::string> help_lines() {
    return {
        "Commands:",
        "",
        "Agent:",
        "  /agent <task>              Run workflow with plan approval",
        "  /agent --auto <task>       Run workflow without plan approval",
        "  /agent explorer <task>     Run Explorer only (read-only)",
        "  /agent planner <task>      Run Planner only (read-only)",
        "  /agent coder <task>        Run Coder only (changes need approval)",
        "  /agent reviewer <task>     Run Reviewer only (read-only)",
        "",
        "Provider:",
        "  /provider <name>           Select provider: gemini, deepseek, openrouter, omniroute",
        "  /models                    List available models",
        "  /model <name>              Select model",
        "",
        "Credentials:",
        "  /key-gemini                Set Gemini API key securely",
        "  /key-deepseek              Set DeepSeek API key securely",
        "  /key-openrouter            Set OpenRouter API key securely",
        "  /key-omniroute             Set OmniRoute API key securely",
        "",
        "Session:",
        "  /status                    Show current configuration",
        "  /clear-session             Clear conversation history",
        "  /clear                     Clear screen",
        "  /exit                      Exit ARN",
    };
}

} // namespace arn
