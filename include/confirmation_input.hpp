#pragma once

#include "terminal.hpp"

namespace arn {

// The host renders pending input; reading raw events does not echo it.
template<class Read>
int edit_confirmation(Read&& read,
                      const std::function<void(std::string_view)>& display) {
    std::string choice;
    if (display) display(choice);
    for (;;) {
        const auto event = read();
        switch (event.type) {
        case TerminalEventType::enter:
            return choice == "y" || choice == "Y" ? 'y'
                : choice.empty() ? '\n' : 'n';
        case TerminalEventType::escape: return 27;
        case TerminalEventType::interrupt: return 3;
        case TerminalEventType::end_of_input: return '\n';
        case TerminalEventType::backspace: choice.clear(); break;
        case TerminalEventType::character:
            for (char value : event.text) {
                if (value == 'y' || value == 'Y' || value == 'n' || value == 'N')
                    choice.assign(1, value);
            }
            break;
        default: continue;
        }
        if (display) display(choice);
    }
}

} // namespace arn
