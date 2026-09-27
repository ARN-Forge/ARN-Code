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
        command == "/key-openrouter";
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

std::string safe_command_echo(std::string_view input) {
    const auto separator = input.find_first_of(" \t");
    auto command = lower_ascii(std::string(input.substr(0, separator)));
    return is_key_command(command) ? std::move(command) : std::string(input);
}

std::vector<std::string> help_lines() {
    return {
        "Commands:",
        "",
        "Agent:",
        "  /agent <task>              Run multi-agent workflow",
        "",
        "Provider:",
        "  /provider <name>           Select provider",
        "  /models                    List available models",
        "  /model <name>              Select model",
        "",
        "Credentials:",
        "  /key-gemini                Set Gemini API key securely",
        "  /key-deepseek              Set DeepSeek API key securely",
        "  /key-openrouter            Set OpenRouter API key securely",
        "",
        "Session:",
        "  /status                    Show current configuration",
        "  /clear-session             Clear conversation history",
        "  /clear                     Clear screen",
        "  /exit                      Exit ARN",
    };
}

} // namespace arn
