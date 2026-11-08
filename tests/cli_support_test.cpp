#include "cli_support.hpp"

#include <algorithm>
#include <deque>
#include <iostream>
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

void key_aliases_are_redacted_before_dispatch() {
    // main's normalize_command accepts these without '/'. Display must apply
    // the same security classification before the command is submitted.
    for (const std::string canonical : {
             "/key-gemini", "/key-deepseek", "/key-openrouter", "/key-omniroute"}) {
        for (const auto spelling : {canonical, canonical.substr(1)}) {
            std::string buffer = "  " + spelling + " ";
            for (const char ch : std::string("FAKE_KEY_123")) {
                buffer += ch;
                const auto display = arn::safe_command_display(buffer);
                check(display.find("FAKE") == std::string::npos,
                      "Key alias exposes typed characters before dispatch");
                check(display.starts_with(canonical + " "), "Alias masking lost command identity");
            }
            const auto parsed = arn::parse_api_key_command(buffer);
            check(parsed && parsed->has_inline_value, "Key alias is still an inline attempt");
            check(arn::safe_command_echo(buffer) == canonical, "Key alias leaks into history");
            while (buffer.back() != ' ') {
                buffer.pop_back();
                check(arn::safe_command_display(buffer).find("FAKE") == std::string::npos,
                      "Backspace exposes previously hidden characters");
            }
            check(!arn::parse_api_key_command(buffer)->has_inline_value,
                  "Deleting alias argument must restore secure-input path");
            // A paste still enters Windows as character records. Also check
            // the sanitization boundary on the complete logical value.
            buffer += "FAKE_КЛЮЧ_123";
            check(arn::safe_command_display(buffer) == canonical + " " + std::string(13, '*'),
                  "Pasted Ukrainian alias argument is not masked per code point");
            check(arn::safe_command_echo(buffer) == canonical, "Pasted alias leaks into history");
        }
    }
    check(arn::safe_command_display("/status") == "/status" &&
          arn::safe_command_display("status") == "status" &&
          arn::safe_command_display("Привіт") == "Привіт",
          "Normal commands and chat must not be reclassified");
    check(arn::safe_command_display("KEY-GEMINI FAKE_VALUE") == "/key-gemini **********",
          "Case-insensitive dispatch alias must be masked");
}

void secure_input_survives_resize_and_clears_on_cancel() {
    using T = arn::TerminalEventType;
    for (const auto stop : {T::escape, T::interrupt, T::end_of_input}) {
        arn::SecretInputBuffer input;
        std::string displayed;
        // Real Windows read_event emits one Unicode scalar per character
        // event, including surrogate pairs, rather than a whole paste chunk.
        for (const auto& ch : std::vector<std::string>{"F", "A", "K", "E", "_", "К", "🙂"}) {
            input.consume({T::character, ch});
            displayed += input.masked();
            input.consume({T::resize});
            check(input.state() == arn::SecretInputState::editing,
                  "Resize submitted/cancelled secure input");
        }
        check(input.masked() == "*******", "Unicode key masks count characters, not bytes");
        input.consume({T::backspace});
        check(input.masked() == "******", "Backspace must erase a full surrogate-pair character");
        input.consume({stop});
        check(input.masked().empty() && !input.take_submitted_secret(),
              "Cancellation/EOF retained a displayable key");
        input.consume({T::enter});
        check(!input.take_submitted_secret(), "Enter after cancellation must not submit");
        check(displayed.find_first_not_of('*') == std::string::npos,
              "Secure-input display contains more than masking characters");
        arn::SecretInputBuffer next;
        next.consume({T::enter});
        check(!next.take_submitted_secret(), "Previous key contaminated the next input session");
    }
}

} // namespace

void credential_reset_is_an_explicit_boundary() {
    using T = arn::TerminalEventType;
    for (const auto stop : {T::escape, T::interrupt, T::end_of_input}) {
        const std::vector<arn::TerminalEvent> events{
            {T::character, "FAKE_FIRST", 4}, {T::enter, {}, 65535},
            {T::resize}, {T::none}, // No boundary inferred from an empty queue.
            {T::character, "FAKE_DELAYED_CONTINUATION"}, {T::tab},
            {T::backspace, {}, 4}, {T::enter}, {stop},
            {T::character, "ordinary fresh action"}};
        std::size_t next{}, resizes{}, discards{}, prompts{};
        const bool resumed = arn::wait_for_credential_input_reset([&] {
            check(next < events.size(), "Input-reset gate read past its boundary");
            return events[next++];
        }, [&] { ++discards; }, [&] { ++prompts; }, [&] { ++resizes; });
        check(resumed == (stop != T::end_of_input) && next == events.size() - 1 && resizes == 1,
              "Credential reset guessed a paste boundary or consumed the next action");
        check(discards == 1, "Credential reset did not discard stale input exactly once");
        check(prompts == 1, "Credential reset did not show its prompt exactly once");
        check(arn::safe_command_display(events[next].text) == events[next].text,
              "Fresh ordinary input was classified as a credential continuation");
    }
}

void credential_reset_ignores_stale_stop_events() {
    using T = arn::TerminalEventType;
    // An Esc or Ctrl+C received before the reset prompt (for example during
    // key verification) is backend state that discard_pending must remove.
    for (const auto stale : {T::escape, T::interrupt}) {
        const std::vector<arn::TerminalEvent> events{
            {stale}, {T::character, "FAKE_QUEUED_TAIL"}, {T::enter},
            {T::character, "FAKE_AFTER_PROMPT"}, {T::escape},
            {T::character, "ordinary fresh action"}};
        std::size_t next{}, first_read = events.size();
        const bool resumed = arn::wait_for_credential_input_reset([&] {
            check(next < events.size(), "Input-reset gate read past its boundary");
            if (first_read == events.size()) first_read = next;
            return events[next++];
        }, [&] { next = 3; }, [] {}, {});
        check(first_read == 3, "Credential reset read before discarding stale input");
        check(resumed && next == 5, "A stale Esc/Ctrl+C satisfied the credential reset");
    }
}

void credential_reset_keeps_key_sent_when_prompt_appears() {
    using T = arn::TerminalEventType;
    // The queue models the console: discard_pending empties it. A user or
    // test sends the reset key the instant the prompt is shown; that key is
    // fresh and must survive, while the stale Esc queued earlier must not.
    for (const auto stop : {T::escape, T::interrupt}) {
        std::deque<arn::TerminalEvent> queue{{T::escape}, {T::character, "FAKE_STALE_TAIL"}};
        std::vector<std::string> calls;
        const bool resumed = arn::wait_for_credential_input_reset([&] {
            calls.emplace_back("read");
            if (queue.empty()) return arn::TerminalEvent{T::end_of_input};
            auto event = queue.front();
            queue.pop_front();
            return event;
        }, [&] { calls.emplace_back("discard"); queue.clear(); },
           [&] { calls.emplace_back("prompt"); queue.push_back({stop}); });
        check(calls == std::vector<std::string>{"discard", "prompt", "read"},
              "Credential reset showed its prompt before discarding stale input");
        check(resumed && queue.empty(),
              "A reset key sent as soon as the prompt appeared was discarded");
    }
}

void secure_repeats_preserve_scalar_counts() {
    using T = arn::TerminalEventType;
    arn::SecretInputBuffer input;
    input.consume({T::character, "К", 4});
    input.consume({T::character, "🙂", 3});
    check(input.masked() == "*******", "Repeated Unicode must mask one star per scalar");
    input.consume({T::backspace, {}, 2});
    check(input.masked() == "*****", "Repeated Backspace lost scalar counts");
    input.consume({T::enter, {}, 65535});
    check(input.take_submitted_secret() == "КККК🙂", "Secure repeat corrupted the submitted fake value");
    arn::SecretInputBuffer maximum;
    maximum.consume({T::character, "X", 65535});
    check(maximum.masked().size() == 65535, "Maximum bounded repeat was truncated");
    maximum.consume({T::backspace, {}, 65535});
    maximum.consume({T::enter});
    check(!maximum.take_submitted_secret(), "Repeated deletion must not underflow/submit stale data");
}

int run_tests() {
    credential_reset_is_an_explicit_boundary();
    credential_reset_ignores_stale_stop_events();
    credential_reset_keeps_key_sent_when_prompt_appears();
    secure_repeats_preserve_scalar_counts();
    key_aliases_are_redacted_before_dispatch();
    secure_input_survives_resize_and_clears_on_cancel();
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

int main() {
    try { return run_tests(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
