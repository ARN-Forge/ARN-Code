#include "confirmation_input.hpp"
#include <stdexcept>
#include <vector>

using arn::TerminalEvent;
using T = arn::TerminalEventType;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void scenario(std::vector<TerminalEvent> events, int expected,
              std::vector<std::string> expected_display) {
    std::size_t next = 0;
    std::vector<std::string> displayed;
    const int result = arn::edit_confirmation([&] {
        check(next < events.size(), "Confirmation consumed past submission");
        return events[next++];
    }, [&](std::string_view choice) { displayed.emplace_back(choice); });
    check(result == expected, "Unexpected decision");
    check(next == events.size(), "Decision returned before Enter or cancellation");
    check(displayed == expected_display, "Pending decision was not rendered correctly");
}

int main() {
    scenario({{T::character, "y"}, {T::enter}}, 'y', {"", "y"});
    scenario({{T::character, "Y"}, {T::enter}}, 'y', {"", "Y"});
    scenario({{T::character, "n"}, {T::enter}}, 'n', {"", "n"});
    scenario({{T::character, "N"}, {T::enter}}, 'n', {"", "N"});
    scenario({{T::enter}}, '\n', {""});
    scenario({{T::character, "y"}, {T::backspace}, {T::character, "n"}, {T::enter}},
             'n', {"", "y", "", "n"});
    scenario({{T::character, "y"}, {T::backspace}, {T::enter}}, '\n', {"", "y", ""});
    scenario({{T::character, "?"}, {T::enter}}, '\n', {"", ""});
    scenario({{T::resize}, {T::character, "y"}, {T::escape}}, 27, {"", "y"});
    scenario({{T::interrupt}}, 3, {""});
    scenario({{T::end_of_input}}, '\n', {""});
    scenario({{T::character, "y"}, {T::resize}, {T::backspace}, {T::resize},
              {T::character, "n"}, {T::enter}}, 'n', {"", "y", "", "n"});
    scenario({{T::character, "y"}, {T::resize}, {T::interrupt}}, 3, {"", "y"});
    scenario({{T::character, "y"}, {T::resize}, {T::end_of_input}}, '\n', {"", "y"});
    // Confirmation owns only its local choice, never a credential input value.
    scenario({{T::character, "FAKE_SECRET_123"}, {T::enter}}, '\n', {"", ""});
    // A previous decision must not authorize the next independent request.
    scenario({{T::character, "y"}, {T::enter}}, 'y', {"", "y"});
    scenario({{T::resize}, {T::enter}}, '\n', {""});
    // A repeat is one pending choice, not automatic approval/submission.
    scenario({{T::character, "y", 65535}, {T::enter, {}, 65535}}, 'y', {"", "y"});
    scenario({{T::character, "y", 4}, {T::backspace, {}, 65535}, {T::enter}},
             '\n', {"", "y", ""});
    scenario({{T::escape, {}, 65535}}, 27, {""});
    scenario({{T::interrupt, {}, 65535}}, 3, {""});
}
