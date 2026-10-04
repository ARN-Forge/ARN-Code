#include "cli_support.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

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
    const std::pair<std::string, arn::ApiKeyCommand> commands[] = {
        {"/key-gemini", arn::ApiKeyCommand::gemini},
        {"/key-deepseek", arn::ApiKeyCommand::deepseek},
        {"/key-openrouter", arn::ApiKeyCommand::openrouter},
        {"/key-omniroute", arn::ApiKeyCommand::omniroute},
    };
    for (const auto& [command, kind] : commands) {
        const auto exact = arn::parse_api_key_command(command);
        check(exact && exact->command == kind && !exact->has_inline_value,
              "Exact key command enters secure-input path");

        const auto surrounded = arn::parse_api_key_command("  \t" + command + " \r\n");
        check(surrounded && !surrounded->has_inline_value,
              "Surrounding whitespace preserves zero-argument command");

        const auto inline_value = arn::parse_api_key_command(command + " " + placeholder);
        check(inline_value && inline_value->has_inline_value,
              "Inline key value is rejected before secure input");
        const auto echoed = arn::safe_command_echo(command + " " + placeholder);
        check(echoed == command && echoed.find(placeholder) == std::string::npos,
              "Inline key text is never echoed");
    }
    check(!arn::parse_api_key_command("/status"), "Normal commands remain unaffected");
    check(arn::safe_command_echo("/status") == "/status",
          "Normal command display remains unchanged");
}

void help_is_line_based() {
    const auto lines = arn::help_lines();
    for (const std::string expected : {
             "  /agent <task>              Run workflow with plan approval",
             "  /agent --auto <task>       Run workflow without plan approval",
             "  /agent explorer <task>     Run Explorer only (read-only)",
             "  /agent planner <task>      Run Planner only (read-only)",
             "  /agent coder <task>        Run Coder only (changes need approval)",
             "  /agent reviewer <task>     Run Reviewer only (read-only)",
         }) {
        check(std::ranges::find(lines, expected) != lines.end(),
              "Every agent command is documented on its own line");
    }
    check(std::ranges::find(lines, "  /key-gemini                Set Gemini API key securely") !=
              lines.end(), "Secure key command is documented");
    check(std::ranges::find(lines, "  /key-omniroute             Set OmniRoute API key securely") !=
              lines.end(), "OmniRoute secure key command is documented");
    check(std::ranges::none_of(lines, [](const std::string& line) {
              return line.find("/agent <task>, /key-gemini") != std::string::npos;
          }), "Help does not contain the old comma-separated command list");
}

} // namespace

int main() {
    // Real-terminal trace: Tab completed /key to /key-gemini + space;
    // 53 printable bytes reached the editor before Enter (65 total bytes).
    for (const std::string command : {"/key-gemini", "/key-deepseek", "/key-openrouter", "/key-omniroute"}) {
        std::string buffer = command + " ";
        check(arn::safe_command_display(buffer) == command,
              "Completion whitespace is not an inline value");
        for (int i = 0; i < 53; ++i) {
            buffer += 'X'; // Synthetic data, never the user's redacted input.
            check(arn::safe_command_display(buffer) == command + " " + std::string(i + 1, '*'),
                  "Every appended inline character is visibly masked before submission");
        }
        check(arn::parse_api_key_command(buffer)->has_inline_value,
              "Observed inline sequence must still be rejected");
        check(arn::safe_command_echo(buffer) == command,
              "Transcript contains no inline value or masks");
        buffer.resize(command.size());
        check(!arn::parse_api_key_command(buffer)->has_inline_value,
              "Removing inline content restores secure-prompt command");
    }
    secure_input_is_masked_and_editable();
    cancellation_and_empty_input_preserve_existing_key();
    key_commands_are_safe_to_echo();
    help_is_line_based();
    return 0;
}
