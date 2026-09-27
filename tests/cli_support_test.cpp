#include "cli_support.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void secure_input_is_masked_and_editable() {
    arn::SecretInputBuffer input;
    const std::string placeholder = "unit-test-secret";
    std::string captured_output;
    for (const char character : placeholder) {
        input.consume({arn::TerminalEventType::character, std::string(1, character)});
        captured_output += input.masked();
    }
    check(input.masked() == std::string(placeholder.size(), '*'), "Characters are masked");
    check(captured_output.find(placeholder) == std::string::npos,
          "Captured display never contains plaintext");
    input.consume({arn::TerminalEventType::backspace, {}});
    check(input.masked().size() == placeholder.size() - 1, "Backspace removes one mask");
    input.consume({arn::TerminalEventType::character, "t"});
    input.consume({arn::TerminalEventType::enter, {}});
    const auto submitted = input.take_submitted_secret();
    check(submitted && *submitted == placeholder, "Enter submits the internal key unchanged");
}

void cancellation_and_empty_input_preserve_existing_key() {
    std::string configured = "existing-placeholder";
    arn::SecretInputBuffer cancelled;
    cancelled.consume({arn::TerminalEventType::character, "x"});
    cancelled.consume({arn::TerminalEventType::escape, {}});
    if (auto replacement = cancelled.take_submitted_secret()) configured = *replacement;
    check(configured == "existing-placeholder", "Escape preserves configured key");

    arn::SecretInputBuffer interrupted;
    interrupted.consume({arn::TerminalEventType::character, "x"});
    interrupted.consume({arn::TerminalEventType::interrupt, {}});
    check(interrupted.state() == arn::SecretInputState::cancelled &&
              !interrupted.take_submitted_secret(),
          "Ctrl+C uses cancellation semantics and does not submit a key");

    arn::SecretInputBuffer empty;
    empty.consume({arn::TerminalEventType::enter, {}});
    if (auto replacement = empty.take_submitted_secret()) configured = *replacement;
    check(configured == "existing-placeholder", "Empty submission preserves configured key");
}

void key_commands_are_safe_to_echo() {
    const std::string placeholder = "unit-test-secret";
    const auto echoed = arn::safe_command_echo("/key-gemini " + placeholder);
    check(echoed == "/key-gemini" && echoed.find(placeholder) == std::string::npos,
          "Inline key text is never echoed");
}

void help_is_line_based() {
    const auto lines = arn::help_lines();
    check(std::ranges::find(lines, "  /agent <task>              Run multi-agent workflow") !=
              lines.end(), "Agent command is documented");
    check(std::ranges::find(lines, "  /key-gemini                Set Gemini API key securely") !=
              lines.end(), "Secure key command is documented");
    check(std::ranges::none_of(lines, [](const std::string& line) {
              return line.find("/agent <task>, /key-gemini") != std::string::npos;
          }), "Help does not contain the old comma-separated command list");
}

} // namespace

int main() {
    secure_input_is_masked_and_editable();
    cancellation_and_empty_input_preserve_existing_key();
    key_commands_are_safe_to_echo();
    help_is_line_based();
    return 0;
}
